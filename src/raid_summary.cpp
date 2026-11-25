#include "raid_summary.hpp"

#include <algorithm>
#include <cstddef>

namespace dicewatch {
namespace {

constexpr std::size_t message_limit = 2000;
constexpr std::size_t max_tie_names = 4;
// Reserves room for two fences and a page number even with 64-bit page indices.
constexpr std::size_t page_markup_units = 64;

struct Rune {
    char32_t value;
    std::size_t bytes;
    bool valid;
};

Rune first_rune(std::string_view text) {
    const auto lead = static_cast<unsigned char>(text.front());
    if (lead < 0x80) return {lead, 1, true};

    constexpr Rune invalid = {0xfffd, 1, false};
    std::size_t bytes;
    char32_t value;
    char32_t minimum;
    if (lead >= 0xc2 && lead <= 0xdf) {
        bytes = 2;
        value = lead & 0x1f;
        minimum = 0x80;
    } else if (lead >= 0xe0 && lead <= 0xef) {
        bytes = 3;
        value = lead & 0x0f;
        minimum = 0x800;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
        bytes = 4;
        value = lead & 0x07;
        minimum = 0x10000;
    } else {
        return invalid;
    }
    if (bytes > text.size()) return invalid;
    for (std::size_t i = 1; i < bytes; ++i) {
        const auto next = static_cast<unsigned char>(text[i]);
        if ((next & 0xc0) != 0x80) return invalid;
        value = (value << 6) | (next & 0x3f);
    }
    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return invalid;
    return {value, bytes, true};
}

bool unsafe_space(char32_t value) {
    return value < 0x20 || (value >= 0x7f && value <= 0x9f) || value == 0x061c ||
        value == 0x200b || value == 0x200e || value == 0x200f ||
        (value >= 0x2028 && value <= 0x202e) || (value >= 0x2060 && value <= 0x2069) ||
        value == 0xfeff;
}

bool domain_character(char32_t value) {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'z') ||
        (value >= 'A' && value <= 'Z') || value == '-' || value == '.' || value >= 0x80;
}

std::string safe_field(std::string_view input, std::size_t limit, std::string_view fallback) {
    std::string result;
    std::size_t used = 0;
    while (!input.empty()) {
        const auto rune = first_rune(input);
        const auto original = input.substr(0, rune.bytes);
        input.remove_prefix(rune.bytes);
        std::string_view display = original;
        const auto value = rune.value;
        if (!rune.valid) {
            display = "?";
        } else if (unsafe_space(value)) {
            display = " ";
        } else if (value == '@' || value == 0xff20 || value == 0xfe6b) {
            // Pasted messages have no API-level allowed_mentions control.
            display = "(at)";
        } else if (value == '`' || value == 0xff40) {
            display = "'";
        } else if (value == ':' || value == 0xff1a || value == 0xfe55) {
            // Remove URL schemes independently of the code block's rendering.
            display = "[:]";
        } else if (value == 0x2024 || value == 0x3002 || value == 0xff0e || value == 0xff61 ||
                   (value == '.' && !input.empty() && domain_character(first_rune(input).value))) {
            display = "[.]";
        }

        if (display == " " && (result.empty() || result.back() == ' ')) continue;
        const auto units = display == original ? (value > 0xffff ? 2u : 1u) : display.size();
        if (used + units > limit || (used + units == limit && !input.empty())) {
            result += u8"\u2026";
            break;
        }
        result += display;
        used += units;
    }
    if (!result.empty() && result.back() == ' ') result.pop_back();
    if (result.empty()) return std::string(fallback);
    return result;
}

// The output of safe_field and all fixed formatting are already valid UTF-8.
std::size_t utf16_units(std::string_view text) {
    std::size_t units = 0;
    for (const auto byte : text) {
        const auto c = static_cast<unsigned char>(byte);
        if ((c & 0xc0) != 0x80) units += c >= 0xf0 ? 2 : 1;
    }
    return units;
}

std::string format_entry(const LedgerEntry& entry, std::size_t rank) {
    const auto& spec = entry.round.spec;
    auto text = std::to_string(rank) + ". " + safe_field(spec.label, 160, "Loot roll") + " [" +
        std::to_string(spec.range.low) + "-" + std::to_string(spec.range.high) + "]\n";
    const auto ranked = entry.round.standings();
    if (ranked.empty()) {
        text += "   Roll result: No eligible rolls\n";
    } else {
        const auto next_rank = std::find_if(ranked.begin(), ranked.end(), [](const Standing& roll) {
            return roll.rank != 1;
        });
        const auto count = static_cast<std::size_t>(next_rank - ranked.begin());
        text += count > 1 ? "   Roll tie: " : "   Roll winner: ";
        for (std::size_t index = 0; index < std::min(count, max_tie_names); ++index) {
            if (index) text += ", ";
            text += safe_field(ranked[index].player, 64, "Unknown player");
        }
        if (count > max_tie_names) text += " + " + std::to_string(count - max_tie_names) + " more";
        text += " (" + std::to_string(ranked.front().value) + ")\n";
    }
    text += "   Loot recipient: " + safe_field(entry.recipient, 96, "Pending assignment") + "\n";
    return text;
}

} // namespace

std::vector<std::string> make_raid_summary(
    std::string_view session_label, const std::vector<LedgerEntry>& entries, std::string_view session) {
    std::vector<const LedgerEntry*> selected;
    for (const auto& entry : entries) {
        if (entry.session == session) selected.push_back(&entry);
    }
    std::stable_sort(selected.begin(), selected.end(), [](const LedgerEntry* a, const LedgerEntry* b) {
        return a->ended_unix < b->ended_unix;
    });

    const auto title = "Raid loot summary: " + safe_field(session_label, 96, "Unnamed raid");
    const auto page_capacity = message_limit - utf16_units(title) - page_markup_units;
    std::vector<std::string> pages(1);
    if (selected.empty()) pages.front() = "No loot rounds recorded for this raid.\n";

    std::size_t used = 0;
    for (std::size_t i = 0; i < selected.size(); ++i) {
        const auto body = format_entry(*selected[i], i + 1);
        const auto units = utf16_units(body);
        if (used && used + units + 1 > page_capacity) {
            pages.emplace_back();
            used = 0;
        }
        if (used) {
            pages.back() += '\n';
            ++used;
        }
        pages.back() += body;
        used += units;
    }

    for (std::size_t i = 0; i < pages.size(); ++i) {
        auto header = "```\n" + title;
        if (pages.size() > 1) {
            header += " (page " + std::to_string(i + 1) + "/" + std::to_string(pages.size()) + ")";
        }
        pages[i].insert(0, header + '\n');
        pages[i] += "```";
    }
    return pages;
}

} // namespace dicewatch
