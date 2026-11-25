#include "announcements.hpp"

#include <algorithm>
#include <cstddef>

namespace dicewatch {
namespace {

constexpr std::size_t max_tie_names = 3;

struct Rune {
    char32_t value;
    std::size_t bytes;
    bool valid;
};

// Consumes one invalid byte at a time so malformed input cannot hide a following control.
Rune first_rune(std::string_view text) {
    const auto lead = static_cast<unsigned char>(text.front());
    if (lead < 0x80) return {lead, 1, true};
    constexpr Rune invalid{0, 1, false};
    if (lead < 0xc2 || lead > 0xf4) return invalid;
    const std::size_t bytes = lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
    if (text.size() < bytes) return invalid;
    const auto minimum = bytes == 2 ? 0x80u : bytes == 3 ? 0x800u : 0x10000u;
    auto value = static_cast<char32_t>(lead & (0x7fu >> bytes));
    for (std::size_t offset = 1; offset < bytes; ++offset) {
        const auto next = static_cast<unsigned char>(text[offset]);
        if ((next & 0xc0) != 0x80) return invalid;
        value = (value << 6) | (next & 0x3f);
    }
    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return invalid;
    return {value, bytes, true};
}

bool unsafe_space(char32_t value) {
    return value < 0x20 || (value >= 0x7f && value <= 0x9f) || value == 0x061c ||
        value == 0x200b || value == 0x200e || value == 0x200f ||
        (value >= 0x2028 && value <= 0x202e) || (value >= 0x2060 && value <= 0x2069) || value == 0xfeff;
}

void append_ellipsis(std::string& output, std::size_t limit) {
    constexpr std::string_view ellipsis = u8"\u2026";
    while (output.size() > limit - ellipsis.size()) {
        auto last = output.size() - 1;
        while ((static_cast<unsigned char>(output[last]) & 0xc0) == 0x80) --last;
        output.resize(last);
    }
    output += ellipsis;
}

std::string safe_field(std::string_view input, std::size_t limit, std::string_view fallback) {
    std::string output;
    while (!input.empty()) {
        const auto rune = first_rune(input);
        auto display = input.substr(0, rune.bytes);
        input.remove_prefix(rune.bytes);
        if (!rune.valid) display = "?";
        else if (unsafe_space(rune.value)) display = " ";
        else if (rune.value == '$' && !input.empty() && input.front() == '{') display = "$ ";
        else if (rune.value == '%') display = u8"\uff05";
        else if (rune.value == '/') display = u8"\u2215";
        else if (rune.value == ';') display = ",";
        else if (rune.value == '<') display = "(";
        else if (rune.value == '>') display = ")";

        if (display == " " && (output.empty() || output.back() == ' ')) continue;
        if (display.size() > limit - output.size()) {
            append_ellipsis(output, limit);
            break;
        }
        output += display;
    }
    if (!output.empty() && output.back() == ' ') output.pop_back();
    return output.empty() ? std::string(fallback) : output;
}

const char* channel_prefix(AnnouncementChannel channel) {
    switch (channel) {
    case AnnouncementChannel::Raid: return "/rsay ";
    case AnnouncementChannel::Group: return "/gsay ";
    case AnnouncementChannel::Guild: return "/guildsay ";
    case AnnouncementChannel::Say: return "/say ";
    }
    return nullptr;
}

bool safe_chat_characters(std::string_view text) {
    while (!text.empty()) {
        const auto rune = first_rune(text);
        if (!rune.valid || unsafe_space(rune.value)) return false;
        text.remove_prefix(rune.bytes);
    }
    return true;
}

bool safe_chat_body(std::string_view text) {
    if (text.empty() || text.size() > max_announcement_bytes || text.front() == '/' || text.front() == ' ' ||
        text.back() == ' ' || text.find("${") != std::string_view::npos ||
        text.find_first_of("%;<>") != std::string_view::npos) return false;
    const auto first = text.find(item_link_delimiter);
    if (first == std::string_view::npos) return safe_chat_characters(text);

    // Only the two delimiters of a single complete item link may bypass the control-character guard.
    const auto last = text.rfind(item_link_delimiter);
    const auto tag = text.substr(first, last - first + 1);
    return parse_item_link(tag).has_value() && safe_chat_characters(text.substr(0, first)) &&
        safe_chat_characters(tag.substr(1, tag.size() - 2)) && safe_chat_characters(text.substr(last + 1));
}

AnnouncementText item_announcement(const char* prefix, const RoundSpec& spec, const std::string& suffix) {
    return {prefix + safe_field(spec.label, max_label_bytes, "unnamed loot") + suffix,
            spec.item ? prefix + spec.item->raw + suffix : std::string{}};
}

std::string award_result(const Round& round) {
    const auto ranked = round.standings();
    if (ranked.empty()) return "No eligible rolls.";

    const auto other = std::find_if(ranked.begin(), ranked.end(), [](const Standing& row) { return row.rank != 1; });
    const auto tied = static_cast<std::size_t>(other - ranked.begin());
    const auto value = std::to_string(ranked.front().value);
    if (tied == 1) {
        return "Roll winner " + safe_field(ranked.front().player, max_player_bytes, "Unknown player") +
            " (" + value + ").";
    }

    std::string result = "Roll tie at " + value + " between ";
    for (std::size_t index = 0; index < std::min(tied, max_tie_names); ++index) {
        if (index) result += ", ";
        result += safe_field(ranked[index].player, max_player_bytes, "Unknown player");
    }
    if (tied > max_tie_names) result += " and " + std::to_string(tied - max_tie_names) + " more";
    return result + '.';
}

} // namespace

const char* announcement_mode_name(AnnouncementMode mode) {
    switch (mode) {
    case AnnouncementMode::Off: return "off";
    case AnnouncementMode::Copy: return "copy";
    case AnnouncementMode::Send: return "send";
    }
    return "";
}

const char* announcement_channel_name(AnnouncementChannel channel) {
    switch (channel) {
    case AnnouncementChannel::Raid: return "raid";
    case AnnouncementChannel::Group: return "group";
    case AnnouncementChannel::Guild: return "guild";
    case AnnouncementChannel::Say: return "say";
    }
    return "";
}

std::optional<AnnouncementMode> parse_announcement_mode(std::string_view text) {
    if (text == "off") return AnnouncementMode::Off;
    if (text == "copy") return AnnouncementMode::Copy;
    if (text == "send") return AnnouncementMode::Send;
    return std::nullopt;
}

std::optional<AnnouncementChannel> parse_announcement_channel(std::string_view text) {
    if (text == "raid") return AnnouncementChannel::Raid;
    if (text == "group") return AnnouncementChannel::Group;
    if (text == "guild") return AnnouncementChannel::Guild;
    if (text == "say") return AnnouncementChannel::Say;
    return std::nullopt;
}

AnnouncementText opening_announcement(const Round& round) {
    const auto& range = round.spec.range;
    if (!range.valid()) return {};
    std::string suffix = ". Use /random ";
    if (range.low != 0) suffix += std::to_string(range.low) + ' ';
    suffix += std::to_string(range.high);
    suffix += round.settings.order == WinnerOrder::Low ? ". Lowest roll wins." : ". Highest roll wins.";
    const int seconds = round.settings.duration_seconds;
    if (round.deadline && seconds > 0 && seconds <= max_duration_seconds) {
        suffix += " Ends in " + std::to_string(seconds) + (seconds == 1 ? " second." : " seconds.");
    }
    return item_announcement("Loot roll open for ", round.spec, suffix);
}

AnnouncementText award_announcement(const LedgerEntry& entry) {
    const auto recipient = safe_field(entry.recipient, max_player_bytes, "");
    if (recipient.empty()) return {};
    return item_announcement("Awarded ", entry.round.spec, " to " + recipient + ". " + award_result(entry.round));
}

std::optional<std::string> chat_command(AnnouncementChannel channel, std::string_view message) {
    const auto* prefix = channel_prefix(channel);
    if (!prefix || !safe_chat_body(message)) return std::nullopt;
    return std::string(prefix) + std::string(message);
}

} // namespace dicewatch
