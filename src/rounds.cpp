#include "rounds.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <system_error>
#include <utility>

namespace dicewatch {
namespace {

constexpr std::size_t max_command_bytes = 512;
constexpr char item_delimiter = '\x12';

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

bool valid_text(std::string_view text, std::size_t max_bytes) {
    if (text.empty() || text.size() > max_bytes) return false;
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i++]);
        std::uint32_t codepoint = lead;
        std::uint32_t minimum = 0;
        std::size_t extra = 0;
        if (lead >= 0xF0 && lead <= 0xF4) {
            codepoint = lead & 0x07;
            minimum = 0x10000;
            extra = 3;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            codepoint = lead & 0x0F;
            minimum = 0x800;
            extra = 2;
        } else if (lead >= 0xC2 && lead <= 0xDF) {
            codepoint = lead & 0x1F;
            minimum = 0x80;
            extra = 1;
        } else if (lead >= 0x80) {
            return false;
        }
        if (extra > text.size() - i) return false;
        for (std::size_t n = 0; n < extra; ++n) {
            const auto byte = static_cast<unsigned char>(text[i++]);
            if ((byte & 0xC0) != 0x80) return false;
            codepoint = (codepoint << 6) | (byte & 0x3F);
        }
        if (codepoint < minimum || codepoint > 0x10FFFF ||
            (codepoint >= 0xD800 && codepoint <= 0xDFFF) ||
            codepoint < 0x20 || (codepoint >= 0x7F && codepoint <= 0x9F) ||
            codepoint == 0x061C || codepoint == 0x200E || codepoint == 0x200F ||
            (codepoint >= 0x2028 && codepoint <= 0x202E) ||
            (codepoint >= 0x2066 && codepoint <= 0x2069)) {
            return false;
        }
    }
    return true;
}

template <typename Integer>
bool parse_number(std::string_view text, Integer& value, int base = 10) {
    if (text.empty()) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, base);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

std::optional<Range> parse_range(std::string_view text) {
    Range range;
    const auto separator = text.find('-');
    if (separator == std::string_view::npos) {
        if (!parse_number(text, range.high)) return std::nullopt;
    } else if (!parse_number(text.substr(0, separator), range.low) ||
               !parse_number(text.substr(separator + 1), range.high)) {
        return std::nullopt;
    }
    if (!range.valid()) return std::nullopt;
    return range;
}

bool is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

std::optional<ItemLink> parse_link(std::string_view raw) {
    if (raw.size() < 3 || raw.size() > max_command_bytes ||
        raw.front() != item_delimiter || raw[1] != '0' || raw.back() != item_delimiter) {
        return std::nullopt;
    }
    const auto body = raw.substr(2, raw.size() - 3);
    // Hailstone selects the wide link first when the metadata is entirely hexadecimal.
    for (const std::size_t id_width : {8u, 5u}) {
        const std::size_t metadata_size = id_width * 7 + 20;
        if (body.size() < metadata_size) continue;
        const auto metadata = body.substr(0, metadata_size);
        if (!std::all_of(metadata.begin(), metadata.end(), is_hex)) continue;
        const auto visible = body.substr(metadata_size);
        const auto label = trim(visible);
        std::uint32_t id = 0;
        if (label.empty() || !valid_text(visible, max_label_bytes) ||
            !parse_number(metadata.substr(0, id_width), id, 16) || id == 0) {
            return std::nullopt;
        }
        return ItemLink{std::string(raw), std::string(label), id};
    }
    return std::nullopt;
}

bool valid_settings(Settings settings) {
    return settings.duration_seconds >= 0 && settings.duration_seconds <= max_duration_seconds &&
        (settings.order == WinnerOrder::High || settings.order == WinnerOrder::Low) &&
        (settings.duplicates == DuplicatePolicy::First ||
         settings.duplicates == DuplicatePolicy::Best ||
         settings.duplicates == DuplicatePolicy::Last);
}

char lower_ascii(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}

bool same_player(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
        [](char x, char y) { return lower_ascii(x) == lower_ascii(y); });
}

bool better(int candidate, int incumbent, WinnerOrder order) {
    return order == WinnerOrder::High ? candidate > incumbent : candidate < incumbent;
}

std::string range_text(const Range& range) {
    return std::to_string(range.low) + "-" + std::to_string(range.high);
}

std::optional<Time> deadline_after(int seconds, Time now) {
    if (seconds == 0) return std::nullopt;
    const auto duration = std::chrono::seconds(seconds);
    return now > Time::max() - duration ? Time::max() : now + duration;
}

} // namespace

bool Range::valid() const {
    return low >= 0 && high >= low && high > 0 && high <= max_roll_value;
}

bool Range::operator==(const Range& other) const {
    return low == other.low && high == other.high;
}

ParseResult parse_itemroll(std::string_view args) {
    if (args.size() > max_command_bytes) return {std::nullopt, "Itemroll command is too long."};
    args = trim(args);
    const auto split = args.find_first_of(" \t");
    if (split == std::string_view::npos) {
        return {std::nullopt, "Use /itemroll <max or min-max> <item link or note>."};
    }
    const auto range = parse_range(args.substr(0, split));
    if (!range) return {std::nullopt, "Use a range within 0-32000 with a positive maximum at or above the minimum."};

    auto label = trim(args.substr(split));
    if (label.find(item_delimiter) != std::string_view::npos) {
        if (label.size() >= 2 && label.front() == '"' && label.back() == '"') {
            label = label.substr(1, label.size() - 2);
        }
        auto item = parse_link(label);
        if (!item) return {std::nullopt, "Use one complete item link or enter a plain-text note."};
        return {RoundSpec{*range, item->label, std::move(item)}, {}};
    }
    if (!valid_text(label, max_label_bytes)) {
        return {std::nullopt, "Enter an item link or a note of up to 160 UTF-8 bytes without control characters."};
    }
    return {RoundSpec{*range, std::string(label), std::nullopt}, {}};
}

std::optional<int> Round::remaining_seconds(Time now) const {
    if (!open() || !deadline) return std::nullopt;
    now = std::max(now, started_at);
    if (now >= *deadline) return 0;
    return static_cast<int>(std::chrono::ceil<std::chrono::seconds>(*deadline - now).count());
}

std::vector<Standing> Round::standings() const {
    std::vector<const Roll*> counted;
    for (const auto& roll : rolls) {
        if (roll.status == RollStatus::Counted) counted.push_back(&roll);
    }
    std::stable_sort(counted.begin(), counted.end(), [&](const Roll* a, const Roll* b) {
        return better(a->value, b->value, settings.order);
    });
    std::vector<Standing> result;
    result.reserve(counted.size());
    for (const auto* roll : counted) {
        const auto rank = result.empty() || result.back().value != roll->value
            ? result.size() + 1 : result.back().rank;
        result.push_back({rank, roll->player, roll->value});
    }
    return result;
}

StartResult Tracker::start(RoundSpec spec, Settings settings, Time now) {
    tick(now);
    if (!spec.range.valid()) return {0, "Use a range within 0-32000 with a positive maximum."};
    if (!valid_settings(settings)) return {0, "Use valid round settings and a timer of 0-3600 seconds."};

    if (spec.item) {
        auto item = parse_link(spec.item->raw);
        if (!item) return {0, "Use one complete item link or enter a plain-text note."};
        spec.label = item->label;
        spec.item = std::move(item);
    } else {
        const auto label = trim(spec.label);
        if (!label.empty() && !valid_text(label, max_label_bytes)) {
            return {0, "Enter a note of up to 160 UTF-8 bytes without control characters."};
        }
        spec.label = label.empty() ? "Loot roll" : std::string(label);
    }

    auto slot = std::find_if(rounds_.begin(), rounds_.end(), [&](const Round& round) {
        return round.spec.range == spec.range;
    });
    if (slot != rounds_.end() && slot->open()) {
        return {0, "An open round already uses " + range_text(spec.range) + ". End it or choose another range."};
    }
    if (slot == rounds_.end() && rounds_.size() >= max_round_slots) {
        slot = std::find_if(rounds_.begin(), rounds_.end(), [](const Round& round) { return !round.open(); });
        if (slot == rounds_.end()) return {0, "End a round before opening another one."};
    }

    Round round;
    round.id = next_id_;
    round.spec = std::move(spec);
    round.settings = settings;
    round.started_at = now;
    round.deadline = deadline_after(settings.duration_seconds, now);
    if (slot == rounds_.end()) rounds_.push_back(std::move(round));
    else *slot = std::move(round);
    return {next_id_++, {}};
}

bool Tracker::ingest(std::string_view player, Range range, int value, Time now) {
    tick(now);
    if (value < range.low || value > range.high || player != trim(player) || !valid_text(player, max_player_bytes)) {
        return false;
    }
    auto round = std::find_if(rounds_.begin(), rounds_.end(), [&](const Round& candidate) {
        return candidate.open() && candidate.spec.range == range;
    });
    if (round == rounds_.end() || now < round->started_at) return false;
    if (round->rolls.size() >= max_rolls_per_round) {
        if (round->dropped_rolls < std::numeric_limits<std::size_t>::max()) ++round->dropped_rolls;
        return false;
    }

    auto& rolls = round->rolls;
    const auto previous = std::find_if(rolls.begin(), rolls.end(), [&](const Roll& entry) {
        return entry.status == RollStatus::Counted && same_player(entry.player, player);
    });
    if (previous == rolls.end()) {
        rolls.push_back({std::string(player), value, now, RollStatus::Counted});
        return true;
    }
    const auto index = static_cast<std::size_t>(previous - rolls.begin());
    const auto policy = round->settings.duplicates;
    const bool replace = policy == DuplicatePolicy::Last ||
        (policy == DuplicatePolicy::Best && better(value, previous->value, round->settings.order));
    rolls.push_back({std::string(player), value, now, replace ? RollStatus::Counted : RollStatus::Duplicate});
    if (replace) rolls[index].status = RollStatus::Replaced;
    return true;
}

bool Tracker::end(RoundId id, Time now) {
    tick(now);
    for (auto& round : rounds_) {
        if (round.id == id && round.open() && now >= round.started_at) {
            finish(round, EndReason::Manual, now);
            return true;
        }
    }
    return false;
}

bool Tracker::set_timer(RoundId id, int seconds, Time now) {
    tick(now);
    if (seconds < 0 || seconds > max_duration_seconds) return false;
    for (auto& round : rounds_) {
        if (round.id == id && round.open() && now >= round.started_at) {
            round.settings.duration_seconds = seconds;
            round.deadline = deadline_after(seconds, now);
            return true;
        }
    }
    return false;
}

void Tracker::tick(Time now) {
    for (auto& round : rounds_) {
        if (round.open() && round.deadline && now >= *round.deadline) {
            finish(round, EndReason::Timed, *round.deadline);
        }
    }
}

const Round* Tracker::find(RoundId id) const {
    const auto round = std::find_if(rounds_.begin(), rounds_.end(), [id](const Round& candidate) {
        return candidate.id == id;
    });
    return round == rounds_.end() ? nullptr : &*round;
}

void Tracker::finish(Round& round, EndReason reason, Time now) {
    auto closed = round;
    closed.ended_at = now;
    closed.end_reason = reason;
    closed.deadline.reset();
    const auto position = std::find_if(history_.cbegin(), history_.cend(), [&](const Round& saved) {
        return now >= *saved.ended_at;
    });
    if (position != history_.cend() || history_.size() < max_history_rounds) {
        history_.insert(position, std::move(closed));
        if (history_.size() > max_history_rounds) history_.pop_back();
    }
    round.ended_at = now;
    round.end_reason = reason;
    round.deadline.reset();
    ++history_revision_;
}

} // namespace dicewatch
