#pragma once

#include "ledger.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace dicewatch {

enum class AnnouncementMode { Off, Copy, Send };
enum class AnnouncementChannel { Raid, Group, Guild, Say };

struct AnnouncementSettings {
    AnnouncementMode start_mode = AnnouncementMode::Off;
    AnnouncementMode award_mode = AnnouncementMode::Off;
    AnnouncementChannel start_channel = AnnouncementChannel::Raid;
    AnnouncementChannel award_channel = AnnouncementChannel::Raid;
};

// Leave room for the fixed chat command prefix within a 512-byte line.
inline constexpr std::size_t max_announcement_bytes = 500;

[[nodiscard]] const char* announcement_mode_name(AnnouncementMode mode);
[[nodiscard]] const char* announcement_channel_name(AnnouncementChannel channel);
[[nodiscard]] std::optional<AnnouncementMode> parse_announcement_mode(std::string_view text);
[[nodiscard]] std::optional<AnnouncementChannel> parse_announcement_channel(std::string_view text);

[[nodiscard]] std::string opening_announcement(const Round& round);
// Empty when there is no saved recipient.
[[nodiscard]] std::string award_announcement(const LedgerEntry& entry);
// Accepts one safe message body and adds an allowlisted in-game channel command.
[[nodiscard]] std::optional<std::string> chat_command(AnnouncementChannel channel, std::string_view message);

} // namespace dicewatch
