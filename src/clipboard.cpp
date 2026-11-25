#include "clipboard.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <dlfcn.h>
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <dlfcn.h>
#include <link.h>
#endif

namespace dicewatch {
namespace {

constexpr std::size_t clipboard_limit = 8192;
constexpr char clipboard_export[] = "SDL_SetClipboardText";

bool valid_text(std::string_view text) noexcept {
    if (text.empty() || text.size() > clipboard_limit) return false;
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i++]);
        if (lead < 0x80) {
            if ((lead < 0x20 && lead != '\t' && lead != '\n' && lead != '\r') || lead == 0x7f) return false;
            continue;
        }
        if (lead < 0xc2 || lead > 0xf4) return false;
        const auto extra = lead < 0xe0 ? 1u : lead < 0xf0 ? 2u : 3u;
        if (extra > text.size() - i) return false;
        char32_t value = lead & (0x7fu >> (extra + 1));
        for (unsigned n = 0; n < extra; ++n) {
            const auto tail = static_cast<unsigned char>(text[i++]);
            if ((tail & 0xc0) != 0x80) return false;
            value = (value << 6) | (tail & 0x3f);
        }
        const auto minimum = extra == 1 ? 0x80u : extra == 2 ? 0x800u : 0x10000u;
        if (value < minimum || value > 0x10ffff || (value >= 0x80 && value <= 0x9f) ||
            (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}

#if defined(_WIN32)

// SDL3 is installed beside hailstone.exe on Windows. A full image path avoids
// accidentally selecting a different already-loaded DLL with the same name.
HMODULE find_sdl() noexcept {
    wchar_t image[32768];
    const auto capacity = static_cast<DWORD>(sizeof(image) / sizeof(image[0]));
    const auto length = GetModuleFileNameW(nullptr, image, capacity);
    if (length == 0 || length >= capacity) return nullptr;
    const auto slash = std::wstring_view(image, length).find_last_of(L"\\/");
    constexpr wchar_t filename[] = L"SDL3.dll";
    constexpr auto filename_size = sizeof(filename) / sizeof(filename[0]);
    if (slash == std::wstring_view::npos || slash + 1 + filename_size > capacity) return nullptr;
    std::copy_n(filename, filename_size, image + slash + 1);
    HMODULE module = nullptr;
    // This acquires a reference only to an image that is already loaded.
    if (!GetModuleHandleExW(0, image, &module)) return nullptr;
    return module;
}

using Setter = bool (__cdecl*)(const char*);

class SdlClipboard {
public:
    SdlClipboard() noexcept : module_(find_sdl()) {}
    ~SdlClipboard() { if (module_) FreeLibrary(module_); }
    SdlClipboard(const SdlClipboard&) = delete;
    SdlClipboard& operator=(const SdlClipboard&) = delete;

    Setter setter() const noexcept {
        if (!module_) return nullptr;
        const auto symbol = GetProcAddress(module_, clipboard_export);
        if (!symbol) return nullptr;
        Setter function = nullptr;
        static_assert(sizeof(function) == sizeof(symbol));
        std::memcpy(&function, &symbol, sizeof(function));
        return function;
    }

private:
    HMODULE module_;
};

#elif defined(__APPLE__) || defined(__linux__)

using Setter = bool (*)(const char*);

bool sdl_image_name(std::string_view name) noexcept {
    const auto slash = name.find_last_of('/');
    if (slash != std::string_view::npos) name.remove_prefix(slash + 1);

#if defined(__APPLE__)
    constexpr std::string_view basename = "libSDL3.dylib";
    constexpr std::string_view prefix = "libSDL3.";
    constexpr std::string_view suffix = ".dylib";
#else
    constexpr std::string_view basename = "libSDL3.so";
    constexpr std::string_view prefix = "libSDL3.so.";
    constexpr std::string_view suffix;
#endif
    if (name == basename) return true;
    if (name.size() <= prefix.size() + suffix.size() || name.substr(0, prefix.size()) != prefix ||
        name.substr(name.size() - suffix.size()) != suffix) return false;
    const auto version = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
    return version.front() >= '0' && version.front() <= '9' && version.back() >= '0' && version.back() <= '9' &&
        std::all_of(version.begin(), version.end(), [](char c) { return (c >= '0' && c <= '9') || c == '.'; });
}

void* existing_image(const char* name) noexcept {
    return dlopen(name, RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD);
}

void* find_versioned_sdl() noexcept {
#if defined(__APPLE__)
    const auto count = _dyld_image_count();
    for (std::uint32_t index = 0; index < count; ++index) {
        const char* name = _dyld_get_image_name(index);
        if (name && sdl_image_name(name)) {
            if (auto* image = existing_image(name)) return image;
        }
    }
    return nullptr;
#else
    // Use the loader's image names without opening anything while it is enumerating.
    std::array<char, 4096> loaded{};
    dl_iterate_phdr([](dl_phdr_info* info, std::size_t, void* output) {
        if (!info->dlpi_name) return 0;
        const std::string_view name = info->dlpi_name;
        auto& path = *static_cast<decltype(loaded)*>(output);
        if (name.size() >= path.size() || !sdl_image_name(name)) return 0;
        std::copy(name.begin(), name.end(), path.begin());
        path[name.size()] = '\0';
        return 1;
    }, &loaded);
    return loaded.front() ? existing_image(loaded.data()) : nullptr;
#endif
}

class SdlClipboard {
public:
    SdlClipboard() noexcept {
#if defined(__APPLE__)
        constexpr const char* names[] = {"libSDL3.0.dylib", "@rpath/libSDL3.0.dylib", "libSDL3.dylib"};
#else
        constexpr const char* names[] = {"libSDL3.so.0", "libSDL3.so"};
#endif
        for (const auto* name : names) {
            module_ = existing_image(name);
            if (module_) return;
        }
        module_ = find_versioned_sdl();
    }
    ~SdlClipboard() { if (module_) dlclose(module_); }
    SdlClipboard(const SdlClipboard&) = delete;
    SdlClipboard& operator=(const SdlClipboard&) = delete;

    Setter setter() const noexcept {
        if (!module_) return nullptr;
        auto* symbol = dlsym(module_, clipboard_export);
        Dl_info info{};
        if (!symbol || dladdr(symbol, &info) == 0 || !info.dli_fname || !sdl_image_name(info.dli_fname)) return nullptr;
        return reinterpret_cast<Setter>(symbol);
    }

private:
    void* module_ = nullptr;
};

#else

class SdlClipboard {
public:
    using Setter = bool (*)(const char*);
    Setter setter() const noexcept { return nullptr; }
};

#endif

} // namespace

bool clipboard_available() noexcept {
    const SdlClipboard clipboard;
    return clipboard.setter() != nullptr;
}

bool write_clipboard(std::string_view text) noexcept {
    if (!valid_text(text)) return false;
    const SdlClipboard clipboard;
    const auto set_text = clipboard.setter();
    if (!set_text) return false;

    std::array<char, clipboard_limit + 1> buffer;
    std::copy(text.begin(), text.end(), buffer.begin());
    buffer[text.size()] = '\0';
    return set_text(buffer.data());
}

} // namespace dicewatch
