#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dicewatch {

using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;
using RoundId = std::uint64_t;

inline constexpr int max_roll_value = 32000;
inline constexpr int max_duration_seconds = 3600;
inline constexpr std::size_t max_label_bytes = 160;
inline constexpr std::size_t max_player_bytes = 64;
inline constexpr std::size_t max_rolls_per_round = 1024;
inline constexpr std::size_t max_round_slots = 16;
inline constexpr std::size_t max_history_rounds = 24;

struct Range {
    int low = 0;
    int high = 100;

    [[nodiscard]] bool valid() const;
    [[nodiscard]] bool operator==(const Range& other) const;
};

enum class DuplicatePolicy { First, Best, Last };
enum class WinnerOrder { High, Low };
enum class RollStatus { Counted, Duplicate, Replaced };
enum class EndReason { Manual, Timed };

struct Settings {
    DuplicatePolicy duplicates = DuplicatePolicy::First;
    WinnerOrder order = WinnerOrder::High;
    int duration_seconds = 0;
};

struct ItemLink {
    std::string raw;
    std::string label;
    std::uint32_t id = 0;
};

struct RoundSpec {
    Range range;
    std::string label;
    std::optional<ItemLink> item;
};

struct ParseResult {
    std::optional<RoundSpec> spec;
    std::string error;
};

struct StartResult {
    RoundId id = 0;
    std::string error;
};

struct Roll {
    std::string player;
    int value = 0;
    Time received_at;
    RollStatus status = RollStatus::Counted;
};

struct Standing {
    std::size_t rank = 0;
    std::string player;
    int value = 0;
};

struct Round {
    RoundId id = 0;
    RoundSpec spec;
    Settings settings;
    Time started_at;
    std::optional<Time> deadline;
    std::optional<Time> ended_at;
    std::optional<EndReason> end_reason;
    std::vector<Roll> rolls;
    std::size_t dropped_rolls = 0;

    [[nodiscard]] bool open() const { return !ended_at.has_value(); }
    [[nodiscard]] std::optional<int> remaining_seconds(Time now) const;
    [[nodiscard]] std::vector<Standing> standings() const;
};

// Receives the raw argument tail from Hailstone's native command handler.
[[nodiscard]] ParseResult parse_itemroll(std::string_view args);

class Tracker {
public:
    [[nodiscard]] StartResult start(RoundSpec spec, Settings settings, Time now);
    [[nodiscard]] bool ingest(std::string_view player, Range range, int value, Time now);
    [[nodiscard]] bool end(RoundId id, Time now);
    [[nodiscard]] bool set_timer(RoundId id, int seconds, Time now);
    void tick(Time now);

    [[nodiscard]] const Round* find(RoundId id) const;
    [[nodiscard]] const std::vector<Round>& rounds() const { return rounds_; }
    [[nodiscard]] const std::vector<Round>& history() const { return history_; }

private:
    void finish(Round& round, EndReason reason, Time now);

    RoundId next_id_ = 1;
    std::vector<Round> rounds_;
    std::vector<Round> history_;
};

} // namespace dicewatch
