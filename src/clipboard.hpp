#pragma once

#include <string_view>

namespace dicewatch {

// Call on Hailstone's main thread. The clipboard contents are never read.
[[nodiscard]] bool clipboard_available() noexcept;
[[nodiscard]] bool write_clipboard(std::string_view text) noexcept;

} // namespace dicewatch
