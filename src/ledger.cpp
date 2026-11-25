#include "ledger.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <limits>
#include <memory>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace dicewatch {
namespace {

constexpr std::string_view ledger_magic = "DICELED1";
constexpr std::string_view record_suffix = ".dwl";
constexpr std::size_t header_bytes = 24;
constexpr std::size_t max_record_bytes = 128 * 1024;
constexpr std::size_t max_key_bytes = 32 + 1 + 20;
constexpr std::size_t max_item_bytes = 512;
constexpr std::uint64_t max_unix_seconds = 253402300799; // 9999-12-31 23:59:59 UTC
constexpr auto max_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::duration::max()).count();

bool valid_text(std::string_view text, std::size_t limit) {
    if (text.empty() || text.size() > limit) return false;
    const auto parsed = parse_itemroll("100 " + std::string(text));
    return parsed.spec && !parsed.spec->item && parsed.spec->label == text;
}

bool parse_key(std::string_view key, RoundId& id) {
    if (key.size() <= 33 || key.size() > max_key_bytes || key[32] != '-' ||
        !valid_ledger_session(key.substr(0, 32)) || key[33] == '0') return false;
    const auto part = key.substr(33);
    const auto result = std::from_chars(part.data(), part.data() + part.size(), id);
    return result.ec == std::errc{} && result.ptr == part.data() + part.size() && id != 0;
}

std::string attempt_as_label(const RoundSpec& spec) {
    return std::to_string(spec.range.low) + "-" + std::to_string(spec.range.high) + " " +
        (spec.item ? spec.item->raw : spec.label);
}

std::optional<std::uint64_t> elapsed_milliseconds(Time started, Time at) {
    if (at < started) return std::nullopt;
    using Rep = Clock::duration::rep;
    static_assert(std::is_integral_v<Rep>);
    using Unsigned = std::make_unsigned_t<Rep>;
    const auto ticks = static_cast<Unsigned>(at.time_since_epoch().count()) -
        static_cast<Unsigned>(started.time_since_epoch().count());
    if (ticks > static_cast<Unsigned>(std::numeric_limits<Rep>::max())) return std::nullopt;
    const auto duration = Clock::duration(static_cast<Rep>(ticks));
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(duration).count());
}

std::optional<Time> restore_time(std::uint64_t milliseconds) {
    if (milliseconds > static_cast<std::uint64_t>(max_elapsed_ms)) return std::nullopt;
    return Time{} + std::chrono::milliseconds(static_cast<std::int64_t>(milliseconds));
}

bool valid_settings(const Settings& settings) {
    return settings.duration_seconds >= 0 && settings.duration_seconds <= max_duration_seconds &&
        (settings.order == WinnerOrder::High || settings.order == WinnerOrder::Low) &&
        (settings.duplicates == DuplicatePolicy::First || settings.duplicates == DuplicatePolicy::Best ||
         settings.duplicates == DuplicatePolicy::Last);
}

bool valid_audit(const Round& round) {
    std::vector<RollStatus> statuses(round.rolls.size(), RollStatus::Counted);
    std::unordered_map<std::string, std::size_t> counted;
    auto previous_time = round.started_at;
    for (std::size_t index = 0; index < round.rolls.size(); ++index) {
        const auto& roll = round.rolls[index];
        if (!valid_text(roll.player, max_player_bytes) || roll.value < round.spec.range.low ||
            roll.value > round.spec.range.high || roll.received_at < previous_time ||
            roll.received_at > *round.ended_at) return false;
        previous_time = roll.received_at;
        auto player = roll.player;
        for (auto& ch : player) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
        auto [prior, inserted] = counted.emplace(std::move(player), index);
        if (!inserted) {
            const int before = round.rolls[prior->second].value;
            const bool better = round.settings.order == WinnerOrder::High ? roll.value > before : roll.value < before;
            if (round.settings.duplicates == DuplicatePolicy::Last ||
                (round.settings.duplicates == DuplicatePolicy::Best && better)) {
                statuses[prior->second] = RollStatus::Replaced;
                prior->second = index;
            } else {
                statuses[index] = RollStatus::Duplicate;
            }
        }
    }
    for (std::size_t index = 0; index < round.rolls.size(); ++index) {
        if (round.rolls[index].status != statuses[index]) return false;
    }
    return true;
}

bool valid_entry(const LedgerEntry& entry) {
    const auto& round = entry.round;
    RoundId id = 0;
    if (!parse_key(entry.key, id) || id != round.id || entry.session != entry.key.substr(0, 32) ||
        entry.ended_unix <= 0 || static_cast<std::uint64_t>(entry.ended_unix) > max_unix_seconds ||
        !round.spec.range.valid() || !valid_settings(round.settings) ||
        !round.ended_at || round.deadline ||
        (round.end_reason != EndReason::Manual && round.end_reason != EndReason::Timed) ||
        (round.end_reason == EndReason::Timed && round.settings.duration_seconds == 0) ||
        !elapsed_milliseconds(round.started_at, *round.ended_at) ||
        round.rolls.size() > max_rolls_per_round ||
        (round.dropped_rolls != 0 && round.rolls.size() != max_rolls_per_round) ||
        round.spec.label.size() > max_label_bytes ||
        (round.spec.item && round.spec.item->raw.size() > max_item_bytes) ||
        !valid_ledger_recipient(entry.recipient)) return false;

    const auto parsed = parse_itemroll(attempt_as_label(round.spec));
    if (!parsed.spec || parsed.spec->label != round.spec.label ||
        parsed.spec->item.has_value() != round.spec.item.has_value()) return false;
    if (round.spec.item &&
        (round.spec.item->id != parsed.spec->item->id || round.spec.item->label != parsed.spec->item->label ||
         round.spec.item->raw != parsed.spec->item->raw)) return false;
    return valid_audit(round);
}

class Writer {
public:
    void number(std::uint64_t value, unsigned bytes) {
        for (unsigned n = 0; n < bytes; ++n) {
            data.push_back(static_cast<char>(value & 0xff));
            value >>= 8;
        }
    }

    void text(std::string_view value) {
        number(value.size(), 2);
        data.append(value);
    }

    std::string data;
};

class Reader {
public:
    explicit Reader(std::string_view data) : data_(data) {}

    std::uint64_t number(unsigned bytes) {
        if (!available(bytes)) return 0;
        std::uint64_t value = 0;
        for (unsigned n = 0; n < bytes; ++n) {
            value |= static_cast<std::uint64_t>(static_cast<unsigned char>(data_[n])) << (n * 8);
        }
        data_.remove_prefix(bytes);
        return value;
    }

    std::string text(std::size_t max_bytes) {
        const auto size = static_cast<std::size_t>(number(2));
        if (size > max_bytes || !available(size)) {
            good_ = false;
            return {};
        }
        auto value = std::string(data_.substr(0, size));
        data_.remove_prefix(size);
        return value;
    }

    bool exhausted() const { return good_ && data_.empty(); }

private:
    bool available(std::size_t bytes) {
        if (bytes > data_.size()) good_ = false;
        return good_;
    }

    std::string_view data_;
    bool good_ = true;
};

// A checksum detects accidental corruption. It does not authenticate local files.
std::uint64_t checksum(std::string_view timestamp, std::string_view body) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto part : {timestamp, body}) {
        for (const char byte : part) {
            hash ^= static_cast<unsigned char>(byte);
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

std::string encode(const LedgerEntry& entry) {
    const auto& round = entry.round;
    Writer body;
    body.text(entry.key);
    body.number(static_cast<unsigned>(round.spec.range.low), 2);
    body.number(static_cast<unsigned>(round.spec.range.high), 2);
    body.number(static_cast<unsigned>(round.settings.duplicates), 1);
    body.number(static_cast<unsigned>(round.settings.order), 1);
    body.number(static_cast<unsigned>(round.settings.duration_seconds), 2);
    body.number(static_cast<unsigned>(*round.end_reason), 1);
    body.number(*elapsed_milliseconds(round.started_at, *round.ended_at), 8);
    body.number(round.dropped_rolls, 8);
    body.text(round.spec.label);
    body.text(round.spec.item ? std::string_view(round.spec.item->raw) : std::string_view{});
    body.text(entry.recipient);
    body.number(round.rolls.size(), 2);
    for (const auto& roll : round.rolls) {
        body.text(roll.player);
        body.number(static_cast<unsigned>(roll.value), 2);
        body.number(*elapsed_milliseconds(round.started_at, roll.received_at), 8);
        body.number(static_cast<unsigned>(roll.status), 1);
    }

    Writer record;
    record.data = ledger_magic;
    record.number(static_cast<std::uint64_t>(entry.ended_unix), 8);
    record.number(checksum(std::string_view(record.data).substr(ledger_magic.size(), 8), body.data), 8);
    record.data += body.data;
    return record.data;
}

std::optional<std::int64_t> header_timestamp(std::string_view prefix) {
    if (prefix.size() != header_bytes || prefix.substr(0, ledger_magic.size()) != ledger_magic) return std::nullopt;
    Reader header(prefix.substr(ledger_magic.size()));
    const auto timestamp = header.number(8);
    if (timestamp == 0 || timestamp > max_unix_seconds) return std::nullopt;
    return static_cast<std::int64_t>(timestamp);
}

std::optional<LedgerEntry> decode(std::string_view prefix, std::string_view body, std::string_view expected_key,
                                  std::int64_t stamp) {
    Reader hash(prefix.substr(16));
    if (hash.number(8) != checksum(prefix.substr(8, 8), body)) return std::nullopt;
    Reader input(body);
    LedgerEntry entry;
    auto& round = entry.round;
    entry.ended_unix = stamp;
    entry.key = input.text(max_key_bytes);
    if (entry.key != expected_key || !parse_key(entry.key, round.id)) return std::nullopt;
    entry.session = entry.key.substr(0, 32);
    round.spec.range.low = static_cast<int>(input.number(2));
    round.spec.range.high = static_cast<int>(input.number(2));
    const auto duplicate = input.number(1);
    const auto order = input.number(1);
    round.settings.duration_seconds = static_cast<int>(input.number(2));
    const auto reason = input.number(1);
    round.ended_at = restore_time(input.number(8));
    const auto dropped = input.number(8);
    if (duplicate > static_cast<unsigned>(DuplicatePolicy::Last) || order > static_cast<unsigned>(WinnerOrder::Low) ||
        reason > static_cast<unsigned>(EndReason::Timed) || !round.ended_at ||
        dropped > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) return std::nullopt;
    round.settings.duplicates = static_cast<DuplicatePolicy>(duplicate);
    round.settings.order = static_cast<WinnerOrder>(order);
    round.end_reason = static_cast<EndReason>(reason);
    round.dropped_rolls = static_cast<std::size_t>(dropped);
    round.spec.label = input.text(max_label_bytes);
    auto raw = input.text(max_item_bytes);
    if (!raw.empty()) {
        round.spec.item = ItemLink{std::move(raw), {}, 0};
        auto parsed = parse_itemroll(attempt_as_label(round.spec));
        if (!parsed.spec || !parsed.spec->item || parsed.spec->label != round.spec.label) return std::nullopt;
        round.spec.item = std::move(parsed.spec->item);
    }
    entry.recipient = input.text(max_player_bytes);
    const auto count = input.number(2);
    if (count > max_rolls_per_round) return std::nullopt;
    round.rolls.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        auto player = input.text(max_player_bytes);
        const auto value = static_cast<int>(input.number(2));
        const auto elapsed = restore_time(input.number(8));
        const auto status = input.number(1);
        if (!elapsed || status > static_cast<unsigned>(RollStatus::Replaced)) return std::nullopt;
        round.rolls.push_back({std::move(player), value, *elapsed, static_cast<RollStatus>(status)});
    }
    if (!input.exhausted() || !valid_entry(entry)) return std::nullopt;
    return entry;
}

std::string temporary_name(std::string_view record) {
    static std::atomic<std::uint64_t> counter{0};
#ifdef _WIN32
    const auto process = GetCurrentProcessId();
#else
    const auto process = getpid();
#endif
    return "." + std::string(record) + "." + std::to_string(process) + "." +
        std::to_string(Clock::now().time_since_epoch().count()) + "." +
        std::to_string(counter.fetch_add(1, std::memory_order_relaxed)) + ".tmp";
}

enum class FolderStatus { Open, Missing, Unsafe };

#ifdef _WIN32

class File {
public:
    explicit File(HANDLE handle = INVALID_HANDLE_VALUE) : handle_(handle) {}
    ~File() { if (*this) CloseHandle(handle_); }

    File(const File&) = delete;
    File& operator=(const File&) = delete;
    explicit operator bool() const { return handle_ != INVALID_HANDLE_VALUE; }

    bool directory() const {
        BY_HANDLE_FILE_INFORMATION info{};
        return GetFileInformationByHandle(handle_, &info) &&
            (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
    }

    std::optional<std::size_t> readable_size() const {
        BY_HANDLE_FILE_INFORMATION info{};
        if (GetFileType(handle_) != FILE_TYPE_DISK || !GetFileInformationByHandle(handle_, &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            info.nNumberOfLinks > 1) return std::nullopt;
        return info.nFileSizeHigh == 0 ? info.nFileSizeLow : std::numeric_limits<std::size_t>::max();
    }

    bool read(std::string& data) const {
        DWORD count = 0;
        return ReadFile(handle_, data.data(), static_cast<DWORD>(data.size()), &count, nullptr) &&
            count == data.size();
    }

    bool write(std::string_view data) const {
        DWORD count = 0;
        return WriteFile(handle_, data.data(), static_cast<DWORD>(data.size()), &count, nullptr) &&
            count == data.size() && FlushFileBuffers(handle_);
    }

private:
    HANDLE handle_;
};

class Folder {
public:
    Folder(const std::filesystem::path& profile, bool create) : path_(profile / "loot-ledger") {
        if (profile.empty()) return;
        if (create && !CreateDirectoryW(path_.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return;
        // Omitting FILE_SHARE_DELETE pins the directory object during all name-based operations below.
        const auto raw = CreateFileW(path_.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (raw == INVALID_HANDLE_VALUE) {
            if (!create && GetLastError() == ERROR_FILE_NOT_FOUND) status = FolderStatus::Missing;
            return;
        }
        directory_.emplace(raw);
        if (directory_->directory()) status = FolderStatus::Open;
    }

    template <typename Visit>
    bool scan(Visit visit) const {
        std::error_code ec;
        std::filesystem::directory_iterator end;
        for (std::filesystem::directory_iterator current(path_, ec); !ec && current != end; current.increment(ec)) {
            const auto name = current->path().filename().native();
            if (!std::all_of(name.begin(), name.end(), [](wchar_t ch) { return static_cast<std::uint32_t>(ch) < 128; })) continue;
            std::string filename(name.size(), '\0');
            std::transform(name.begin(), name.end(), filename.begin(), [](wchar_t ch) { return static_cast<char>(ch); });
            visit(filename);
        }
        return !ec;
    }

    File open(const std::string& name) const {
        const auto path = path_ / name;
        return File(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    }

    bool replace(const std::string& name, std::string_view data) const {
        const auto destination = path_ / name;
        const auto attributes = GetFileAttributesW(destination.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            if (GetLastError() != ERROR_FILE_NOT_FOUND) return false;
        } else {
            // A final reparse point is never opened through and may not be used as an existing receipt.
            const auto previous = open(name);
            if (!previous || !previous.readable_size()) return false;
        }
        const auto staged = path_ / temporary_name(name);
        bool written;
        {
            const File file(CreateFileW(staged.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                       FILE_ATTRIBUTE_NORMAL, nullptr));
            if (!file) return false;
            written = file.write(data);
        }
        if (written && MoveFileExW(staged.c_str(), destination.c_str(),
                                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        DeleteFileW(staged.c_str());
        return false;
    }

    FolderStatus status = FolderStatus::Unsafe;

private:
    std::filesystem::path path_;
    std::optional<File> directory_;
};

#else

bool sync_file(int fd) {
    int result;
    do { result = fsync(fd); } while (result < 0 && errno == EINTR);
    return result == 0;
}

class File {
public:
    explicit File(int fd = -1) : fd_(fd) {}
    ~File() { if (*this) close(fd_); }

    File(const File&) = delete;
    File& operator=(const File&) = delete;
    explicit operator bool() const { return fd_ >= 0; }

    std::optional<std::size_t> readable_size() const {
        struct stat info{};
        if (fstat(fd_, &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink > 1 || info.st_size < 0) return std::nullopt;
        const auto size = static_cast<std::uintmax_t>(info.st_size);
        if (size > std::numeric_limits<std::size_t>::max()) return std::nullopt;
        return static_cast<std::size_t>(size);
    }

    bool read(std::string& data) const {
        std::size_t offset = 0;
        while (offset < data.size()) {
            const auto count = ::read(fd_, data.data() + offset, data.size() - offset);
            if (count == -1 && errno == EINTR) continue;
            if (count <= 0) return false;
            offset += static_cast<std::size_t>(count);
        }
        return true;
    }

    bool write(std::string_view data) const {
        while (!data.empty()) {
            const auto count = ::write(fd_, data.data(), data.size());
            if (count == -1 && errno == EINTR) continue;
            if (count <= 0) return false;
            data.remove_prefix(static_cast<std::size_t>(count));
        }
        return sync_file(fd_);
    }

    int get() const { return fd_; }

private:
    int fd_;
};

class Folder {
public:
    Folder(const std::filesystem::path& profile, bool create) {
        if (profile.empty()) return;
        const File parent(::open(profile.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (!parent) return;
        if (create) {
            if (mkdirat(parent.get(), "loot-ledger", S_IRWXU) == 0) {
                // Directory fsync is unsupported on some filesystems. The new directory is already visible.
                (void)sync_file(parent.get());
            } else if (errno != EEXIST) {
                return;
            }
        }
        const int fd = openat(parent.get(), "loot-ledger", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) {
            if (!create && errno == ENOENT) status = FolderStatus::Missing;
            return;
        }
        directory_.emplace(fd);
        status = FolderStatus::Open;
    }

    template <typename Visit>
    bool scan(Visit visit) const {
        const int scan_fd = fcntl(directory_->get(), F_DUPFD_CLOEXEC, 0);
        if (scan_fd < 0) return false;
        auto* stream = fdopendir(scan_fd);
        if (!stream) {
            close(scan_fd);
            return false;
        }
        struct CloseDir { void operator()(DIR* dir) const { closedir(dir); } };
        std::unique_ptr<DIR, CloseDir> owned(stream);
        for (;;) {
            errno = 0;
            const auto* entry = readdir(stream);
            if (!entry) return errno == 0;
            visit(std::string(entry->d_name));
        }
    }

    File open(const std::string& name) const {
        return File(openat(directory_->get(), name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    }

    bool replace(const std::string& name, std::string_view data) const {
        const int directory = directory_->get();
        struct stat info{};
        if (fstatat(directory, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0) {
            if (!S_ISREG(info.st_mode) || info.st_nlink != 1) return false;
        } else if (errno != ENOENT) {
            return false;
        }
        const auto staged = temporary_name(name);
        const File file(openat(directory, staged.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                               S_IRUSR | S_IWUSR));
        if (!file) return false;
        if (file.write(data) && renameat(directory, staged.c_str(), directory, name.c_str()) == 0) {
            // The atomic rename has already committed. A directory sync failure cannot undo it.
            (void)sync_file(directory);
            return true;
        }
        unlinkat(directory, staged.c_str(), 0);
        return false;
    }

    FolderStatus status = FolderStatus::Unsafe;

private:
    std::optional<File> directory_;
};

#endif

bool newer(const LedgerEntry& entry, std::int64_t stamp, std::string_view key) {
    return entry.ended_unix > stamp || (entry.ended_unix == stamp && entry.key > key);
}

} // namespace

bool valid_ledger_session(std::string_view session) {
    return session.size() == 32 && std::all_of(session.begin(), session.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

bool valid_ledger_recipient(std::string_view recipient) {
    return recipient.empty() || valid_text(recipient, max_player_bytes);
}

LedgerLoad load_ledger(const std::filesystem::path& profile) {
    const Folder folder(profile, false);
    if (folder.status == FolderStatus::Missing) return {};
    if (folder.status != FolderStatus::Open) return {{}, "The loot ledger folder is unavailable or unsafe."};

    LedgerLoad result;
    bool damaged = false;
    const bool scanned = folder.scan([&](const std::string& name) {
        const auto filename = std::string_view(name);
        if (filename.size() <= record_suffix.size() ||
            filename.substr(filename.size() - record_suffix.size()) != record_suffix) return;
        const auto key = filename.substr(0, filename.size() - record_suffix.size());
        RoundId id;
        if (!parse_key(key, id)) {
            damaged = true;
            return;
        }

        const auto file = folder.open(name);
        const auto size = file ? file.readable_size() : std::nullopt;
        std::string prefix(header_bytes, '\0');
        if (!size || *size <= header_bytes || *size > max_record_bytes || !file.read(prefix)) {
            damaged = true;
            return;
        }
        const auto stamp = header_timestamp(prefix);
        if (!stamp) {
            damaged = true;
            return;
        }
        if (result.entries.size() == max_ledger_entries && newer(result.entries.back(), *stamp, key)) return;

        std::string body(*size - header_bytes, '\0');
        auto entry = file.read(body) ? decode(prefix, body, key, *stamp) : std::nullopt;
        if (!entry) {
            damaged = true;
            return;
        }
        const auto pos = std::find_if(result.entries.begin(), result.entries.end(), [&](const LedgerEntry& stored) {
            return !newer(stored, *stamp, key);
        });
        result.entries.insert(pos, std::move(*entry));
        if (result.entries.size() > max_ledger_entries) result.entries.pop_back();
    });
    if (!scanned) result.error = "The loot ledger could not be read completely.";
    else if (damaged) result.error = "Some loot ledger records were unreadable or invalid.";
    return result;
}

std::string save_ledger_entry(const std::filesystem::path& profile, const LedgerEntry& entry) {
    if (!valid_entry(entry)) return "The loot ledger record is invalid.";
    const auto content = encode(entry);
    if (content.size() > max_record_bytes) return "The loot ledger record is too large.";
    const Folder folder(profile, true);
    if (folder.status != FolderStatus::Open) return "The loot ledger folder is unavailable or unsafe.";
    if (!folder.replace(entry.key + std::string(record_suffix), content)) return "The loot ledger record could not be saved.";
    return {};
}

} // namespace dicewatch
