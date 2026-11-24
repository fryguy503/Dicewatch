#include "rounds.hpp"
#include "hailstone_plugin.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

HAILSTONE_PLUGIN

namespace {
using namespace dicewatch;

constexpr int required_caps = HAILSTONE_CAP_READ | HAILSTONE_CAP_COMMANDS | HAILSTONE_CAP_PACKETS | HAILSTONE_CAP_UI;
constexpr std::uint16_t random_reply_opcode = 0x106b; // RoF2 OP_RandomReply
constexpr std::size_t random_reply_size = 12 + 64;
constexpr std::size_t command_size = 512;

enum class View { Ranked, Log, Both };

struct Preferences {
    Settings defaults;
    View view = View::Ranked;
    bool mini = false;
};

struct Runtime {
    Tracker tracker;
    Preferences preferences;
    std::filesystem::path profile;
    std::string character;
    std::string message;
    RoundId selected = 0;
    Time refresh_at;
    bool open = false;
    bool online = false;
    bool zoning = false;
    bool initialized = false;
    bool error = false;
};

Runtime state;
bool roll_command_registered = false;
bool panel_command_registered = false;
bool model_registered = false;
bool action_registered = false;

std::optional<std::string_view> bounded_text(const char* text, std::size_t max) {
    if (!text) return std::nullopt;
    for (std::size_t size = 0; size <= max; ++size) {
        if (text[size] == '\0') return std::string_view(text, size);
    }
    return std::nullopt;
}

const char* duplicate_name(DuplicatePolicy policy) {
    switch (policy) {
    case DuplicatePolicy::Best: return "best";
    case DuplicatePolicy::Last: return "last";
    case DuplicatePolicy::First: return "first";
    }
    return "first";
}

const char* order_name(WinnerOrder order) {
    return order == WinnerOrder::Low ? "low" : "high";
}

const char* view_name(View view) {
    switch (view) {
    case View::Log: return "log";
    case View::Both: return "both";
    case View::Ranked: return "ranked";
    }
    return "ranked";
}

std::optional<View> read_view(std::string_view text) {
    if (text == "ranked") return View::Ranked;
    if (text == "log") return View::Log;
    if (text == "both") return View::Both;
    return std::nullopt;
}

std::optional<Settings> read_defaults(std::string_view duplicate, std::string_view order, std::int64_t duration) {
    Settings result;
    if (duplicate == "best") result.duplicates = DuplicatePolicy::Best;
    else if (duplicate == "last") result.duplicates = DuplicatePolicy::Last;
    else if (duplicate != "first") return std::nullopt;

    if (order == "low") result.order = WinnerOrder::Low;
    else if (order != "high") return std::nullopt;
    if (duration < 0 || duration > max_duration_seconds) return std::nullopt;
    result.duration_seconds = static_cast<int>(duration);
    return result;
}

void note(std::string message, bool error = false) {
    state.message = std::move(message);
    state.error = error;
}

void say(const std::string& message) {
    if (HAILSTONE_HOST_HAS(host, write_chat)) {
        const std::string line = "Dicewatch: " + message;
        host->write_chat(host->host, line.c_str(), 0);
    }
}

bool ready() {
    return state.initialized && state.online && HAILSTONE_HOST_HAS(host, caps) &&
        (host->caps(host->host) & required_caps) == required_caps;
}

std::filesystem::path current_profile() {
    if (!HAILSTONE_HOST_HAS(host, character_config_dir)) return {};
    std::array<char, 4096> path{};
    const int length = host->character_config_dir(host->host, path.data(), path.size());
    if (length <= 0 || static_cast<std::size_t>(length) >= path.size() || path[length] != '\0') return {};
    return std::filesystem::path(path.data());
}

Preferences load_preferences(const std::filesystem::path& profile) {
    const auto file = profile / "dicewatch-settings.txt";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(file, ec))) return {};

    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    std::array<char, 128> buffer{};
    in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    if (!in.eof() || in.bad() || in.gcount() <= 0) return {};

    std::istringstream line(std::string(buffer.data(), static_cast<std::size_t>(in.gcount())));
    std::string version, duplicate, order, view, extra;
    int duration, mini;
    if (!(line >> version >> duplicate >> order >> duration >> view >> mini) ||
        version != "dicewatch1" || (mini != 0 && mini != 1) || line >> extra) return {};
    const auto settings = read_defaults(duplicate, order, duration);
    const auto layout = read_view(view);
    return settings && layout ? Preferences{*settings, *layout, mini != 0} : Preferences{};
}

bool save_preferences() {
    if (state.profile.empty()) return false;
    const auto destination = state.profile / "dicewatch-settings.txt";
    // A separate temporary directory gives concurrent client instances exclusive files.
    const auto nonce = std::to_string(Clock::now().time_since_epoch().count());
    const auto temporary = state.profile / (".dicewatch-" + nonce);
    std::error_code ec;
    if (!std::filesystem::create_directory(temporary, ec)) return false;
    const auto staging = temporary / "settings";
    bool saved = false;
    {
        std::ofstream out(staging, std::ios::binary | std::ios::trunc);
        const auto& preferences = state.preferences;
        out << "dicewatch1 " << duplicate_name(preferences.defaults.duplicates) << ' '
            << order_name(preferences.defaults.order) << ' ' << preferences.defaults.duration_seconds << ' '
            << view_name(preferences.view) << ' ' << static_cast<int>(preferences.mini) << '\n';
        out.close();
        if (out) {
            std::filesystem::rename(staging, destination, ec);
            saved = !ec;
        }
    }
    std::filesystem::remove(staging, ec);
    std::filesystem::remove(temporary, ec);
    return saved;
}

void leave_game() {
    state.online = false;
    state.open = false;
    state.tracker = {};
    state.preferences = {};
    state.profile.clear();
    state.character.clear();
    state.selected = 0;
    state.refresh_at = {};
    state.zoning = false;
    state.message.clear();
    state.error = false;
}

void refresh_character() {
    state.online = false;
    if (state.zoning || !HAILSTONE_HOST_HAS(host, get_player) ||
        !(host->caps(host->host) & HAILSTONE_CAP_READ)) return;
    HailstonePlayer player{};
    player.size = sizeof player;
    if (!host->get_player(host->host, &player) || !player.spawn_id || !player.in_zone ||
        player.game_state != GAMESTATE_INGAME) return;
    const auto name_end = std::find(std::begin(player.name), std::end(player.name), '\0');
    if (name_end == std::end(player.name) || name_end == std::begin(player.name)) return;
    std::string name(std::begin(player.name), name_end);
    auto profile = current_profile();
    if (state.character != name || state.profile != profile) {
        leave_game();
        state.character = std::move(name);
        state.profile = std::move(profile);
        if (!state.profile.empty()) state.preferences = load_preferences(state.profile);
    }
    state.online = true;
}

const char* status(const Round& round) {
    if (round.open()) return "Rolling";
    return round.end_reason == EndReason::Timed ? "Auto-ended" : "Ended";
}

const char* roll_status(RollStatus value) {
    if (value == RollStatus::Duplicate) return "Duplicate";
    if (value == RollStatus::Replaced) return "Replaced";
    return "Counted";
}

std::string range_label(Range range) {
    return std::to_string(range.low) + "–" + std::to_string(range.high);
}

std::string leading_text(const Round& round, const std::vector<Standing>& standings) {
    if (standings.empty()) return round.open() ? "Waiting for rolls" : "No eligible rolls";
    const auto tied = std::count_if(standings.begin(), standings.end(), [](const Standing& row) { return row.rank == 1; });
    std::string result = tied > 1 ? "Tie: " : round.open() ? "Leader: " : "Winner: ";
    for (std::ptrdiff_t n = 0; n < std::min<std::ptrdiff_t>(tied, 4); ++n) {
        if (n) result += ", ";
        result += standings[static_cast<std::size_t>(n)].player;
    }
    if (tied > 4) result += " +" + std::to_string(tied - 4);
    result += " · " + std::to_string(standings.front().value);
    return result;
}

std::string summarize(const Round& round, const std::vector<Standing>& standings, const std::string& leader) {
    constexpr std::size_t visible_rows = 40;
    std::string text = round.spec.label + " (" + range_label(round.spec.range) + ")\n" + leader + "\n";
    for (std::size_t n = 0; n < std::min(standings.size(), visible_rows); ++n) {
        const auto& entry = standings[n];
        text += "\n" + std::to_string(entry.rank) + ". " + entry.player + " — " + std::to_string(entry.value);
    }
    if (standings.size() > visible_rows) text += "\n\n+" + std::to_string(standings.size() - visible_rows) + " more players";
    return text;
}

class Output {
public:
    explicit Output(HailstoneOut* out) : out_(out) {}
    void table(const char* key = nullptr) {
        field(key);
        host->out_begin_table(host->host, out_);
    }
    void array(const char* key) {
        field(key);
        host->out_begin_array(host->host, out_);
    }
    void close() { host->out_end(host->host, out_); }
    void string(const char* key, const char* value) {
        field(key);
        const HailstoneValue data{HAILSTONE_STRING, 0, 0, value};
        host->out_value(host->host, out_, &data);
    }
    void string(const char* key, const std::string& value) { string(key, value.c_str()); }
    void integer(const char* key, std::int64_t value) {
        field(key);
        const HailstoneValue data{HAILSTONE_INT, value, 0, nullptr};
        host->out_value(host->host, out_, &data);
    }
    void boolean(const char* key, bool value) {
        field(key);
        const HailstoneValue data{HAILSTONE_BOOL, value ? 1 : 0, 0, nullptr};
        host->out_value(host->host, out_, &data);
    }
private:
    void field(const char* key) { if (key) host->out_key(host->host, out_, key); }
    HailstoneOut* out_;
};

void emit_round(Output& output, const Round& round, Time now, bool detail, bool summary,
                const char* key = nullptr) {
    const auto standings = round.standings();
    const auto leader = leading_text(round, standings);
    output.table(key);
    output.integer("id", static_cast<std::int64_t>(round.id));
    output.string("label", round.spec.label);
    output.integer("low", round.spec.range.low);
    output.integer("high", round.spec.range.high);
    output.boolean("closed", !round.open());
    output.string("status", status(round));
    output.integer("total", static_cast<std::int64_t>(round.rolls.size()));
    output.integer("counted", static_cast<std::int64_t>(standings.size()));
    output.integer("dropped", static_cast<std::int64_t>(round.dropped_rolls));
    output.integer("remaining", round.remaining_seconds(now).value_or(-1));
    output.string("leader", leader);
    output.boolean("linked", round.spec.item.has_value());
    if (summary && !round.open()) output.string("summary", summarize(round, standings, leader));
    if (detail) {
        output.array("ranked");
        for (const auto& entry : standings) {
            output.table();
            output.integer("rank", static_cast<std::int64_t>(entry.rank));
            output.string("player", entry.player);
            output.integer("value", entry.value);
            output.close();
        }
        output.close();
        output.array("log");
        for (auto it = round.rolls.rbegin(); it != round.rolls.rend(); ++it) {
            output.table();
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(it->received_at - round.started_at);
            output.integer("time", std::max<std::int64_t>(0, elapsed.count()));
            output.string("player", it->player);
            output.integer("value", it->value);
            output.string("status", roll_status(it->status));
            output.close();
        }
        output.close();
    }
    output.close();
}

int panel_state(void*, const HailstoneArgs*, HailstoneOut* out) noexcept {
    try {
        Output output(out);
        const auto now = Clock::now();
        output.table();
        output.boolean("open", state.open);
        if (state.open) {
            output.boolean("ready", ready());
            output.string("message", state.message);
            output.boolean("error", state.error);
            output.string("view", view_name(state.preferences.view));
            output.boolean("mini", state.preferences.mini);
            output.integer("selected", static_cast<std::int64_t>(state.selected));
            output.table("defaults");
            output.string("duplicate", duplicate_name(state.preferences.defaults.duplicates));
            output.string("order", order_name(state.preferences.defaults.order));
            output.integer("duration", state.preferences.defaults.duration_seconds);
            output.close();
            output.array("rounds");
            for (const auto& entry : state.tracker.rounds()) emit_round(output, entry, now, false, false);
            output.close();
            output.array("history");
            for (const auto& entry : state.tracker.history()) emit_round(output, entry, now, false, true);
            output.close();
            if (const auto* round = state.tracker.find(state.selected)) emit_round(output, *round, now, true, true, "current");
        }
        output.close();
        return 1;
    } catch (...) {
        return 0;
    }
}

std::optional<std::string_view> string_arg(const HailstoneArgs* args, std::size_t index, std::size_t max) {
    HailstoneValue value{};
    if (!host->arg(host->host, args, index, &value) || value.kind != HAILSTONE_STRING) return std::nullopt;
    return bounded_text(value.s, max);
}

std::optional<std::int64_t> integer_arg(const HailstoneArgs* args, std::size_t index) {
    HailstoneValue value{};
    if (!host->arg(host->host, args, index, &value) || value.kind != HAILSTONE_INT) return std::nullopt;
    return value.i;
}

std::optional<bool> boolean_arg(const HailstoneArgs* args, std::size_t index) {
    HailstoneValue value{};
    if (!host->arg(host->host, args, index, &value) || value.kind != HAILSTONE_BOOL) return std::nullopt;
    return value.i != 0;
}

std::optional<Settings> settings_args(const HailstoneArgs* args, std::size_t offset) {
    const auto duplicate = string_arg(args, offset, 8);
    const auto order = string_arg(args, offset + 1, 8);
    const auto duration = integer_arg(args, offset + 2);
    return duplicate && order && duration ? read_defaults(*duplicate, *order, *duration) : std::nullopt;
}

void start_round(std::string_view input, Settings settings, bool from_chat) {
    auto parsed = parse_itemroll(input);
    if (!parsed.spec) {
        note(std::move(parsed.error), true);
    } else {
        const auto range = parsed.spec->range;
        const auto result = state.tracker.start(std::move(*parsed.spec), settings, Clock::now());
        if (result.id) {
            state.selected = result.id;
            note("Opened " + range_label(range) + " for rolls.");
        } else {
            note(result.error, true);
        }
    }
    if (from_chat) say(state.message);
}

int panel_action(void*, const HailstoneArgs* args, HailstoneOut*) noexcept {
    try {
        const auto action = string_arg(args, 0, 16);
        if (!action) return 0;
        if (*action == "hide") {
            state.open = false;
            return 0;
        }
        if (!state.open) return 0;
        if (*action == "layout") {
            const auto view_text = string_arg(args, 1, 8);
            const auto layout = view_text ? read_view(*view_text) : std::nullopt;
            const auto mini = boolean_arg(args, 2);
            if (layout && mini) {
                state.preferences.view = *layout;
                state.preferences.mini = *mini;
                if (!save_preferences()) note("Layout updated for this session.", false);
            }
            return 0;
        }

        if (!ready()) {
            note("Enter a zone where Dicewatch is permitted to track rolls.", true);
            return 0;
        }
        if (*action == "defaults") {
            const auto settings = settings_args(args, 1);
            if (!settings) note("Invalid round defaults.", true);
            else {
                state.preferences.defaults = *settings;
                if (save_preferences()) note("Saved round defaults.");
                else note("Defaults updated for this session.");
            }
            return 0;
        }
        if (*action == "start") {
            const auto range = string_arg(args, 1, 16);
            const auto label = string_arg(args, 2, max_label_bytes);
            const auto settings = settings_args(args, 3);
            if (!range || !label || !settings) note("Enter a valid range, round name, and settings.", true);
            else {
                std::string input(*range);
                input += ' ';
                input += label->empty() ? std::string_view("Loot roll") : *label;
                start_round(input, *settings, false);
            }
            return 0;
        }

        const auto value = integer_arg(args, 1);
        if (!value || *value <= 0) return 0;
        const auto id = static_cast<RoundId>(*value);
        if (*action == "select") {
            if (state.tracker.find(id)) state.selected = id;
        } else if (*action == "end") {
            if (id != state.selected || !state.tracker.end(id, Clock::now())) note("This round is already closed or no longer selected.", true);
            else note("Round ended.");
        } else if (*action == "timer") {
            const auto seconds = integer_arg(args, 2);
            if (id != state.selected || !seconds || *seconds < 0 || *seconds > max_duration_seconds ||
                !state.tracker.set_timer(id, static_cast<int>(*seconds), Clock::now())) {
                note("Select an open round and enter a valid timer.", true);
            } else {
                note(*seconds == 0 ? "Auto end is off for this round." : "Auto end is set for this round.");
            }
        }
    } catch (...) {}
    return 0;
}

void itemroll_command(void*, const char* args) noexcept {
    try {
        if (!ready()) refresh_character();
        if (!ready()) {
            say("Enter a zone where Dicewatch is permitted to track rolls.");
            return;
        }
        const auto input = bounded_text(args, command_size);
        if (!input) {
            say("The itemroll command was too long.");
            return;
        }
        state.open = true;
        start_round(*input, state.preferences.defaults, true);
    } catch (...) {}
}

void show_command(void*, const char* args) noexcept {
    try {
        const auto input = bounded_text(args, 16);
        if (!input || (!input->empty() && *input != "show" && *input != "hide")) {
            say("Use /dicewatch to open the tracker, or /dicewatch hide to close it.");
            return;
        }
        state.open = *input != "hide";
    } catch (...) {}
}

void unregister() {
    if (action_registered) host->remove_ui_function(host->host, "action");
    if (model_registered) host->remove_ui_function(host->host, "state");
    if (roll_command_registered) host->remove_command(host->host, "/itemroll");
    if (panel_command_registered) host->remove_command(host->host, "/dicewatch");
    action_registered = model_registered = roll_command_registered = panel_command_registered = false;
}

std::uint32_t read_le32(const std::uint8_t* data) {
    return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8) |
        (static_cast<std::uint32_t>(data[2]) << 16) | (static_cast<std::uint32_t>(data[3]) << 24);
}
} // namespace

HAILSTONE_API void InitializePlugin() noexcept {
    try {
        if (!HAILSTONE_HOST_HAS(host, caps) || !HAILSTONE_HOST_HAS(host, get_player) ||
            !HAILSTONE_HOST_HAS(host, add_command) || !HAILSTONE_HOST_HAS(host, remove_command) ||
            !HAILSTONE_HOST_HAS(host, add_ui_function) || !HAILSTONE_HOST_HAS(host, remove_ui_function) ||
            !HAILSTONE_HOST_HAS(host, arg) || !HAILSTONE_HOST_HAS(host, out_begin_table) ||
            !HAILSTONE_HOST_HAS(host, out_begin_array) || !HAILSTONE_HOST_HAS(host, out_key) ||
            !HAILSTONE_HOST_HAS(host, out_end) || !HAILSTONE_HOST_HAS(host, out_value)) return;
        panel_command_registered = host->add_command(host->host, "/dicewatch", show_command, nullptr) != 0;
        roll_command_registered = host->add_command(host->host, "/itemroll", itemroll_command, nullptr) != 0;
        model_registered = host->add_ui_function(host->host, "state", panel_state, nullptr) != 0;
        action_registered = host->add_ui_function(host->host, "action", panel_action, nullptr) != 0;
        if (!(panel_command_registered && roll_command_registered && model_registered && action_registered)) {
            unregister();
            if (HAILSTONE_HOST_HAS(host, log)) host->log(host->host, 2, "Dicewatch commands or UI were unavailable.");
            return;
        }
        state.initialized = true;
        if (HAILSTONE_HOST_HAS(host, add_menu)) host->add_menu(host->host, "Dicewatch", "/dicewatch");
        refresh_character();
    } catch (...) {
        unregister();
        state.initialized = false;
    }
}

HAILSTONE_API void ShutdownPlugin() noexcept {
    try {
        unregister();
        leave_game();
        state.initialized = false;
    } catch (...) {}
}

HAILSTONE_API void SetGameState(int game_state) noexcept {
    try {
        if (!state.initialized) return;
        if (game_state == GAMESTATE_INGAME) refresh_character();
        else leave_game();
    } catch (...) {
        state.online = false;
    }
}

HAILSTONE_API void OnBeginZone() noexcept {
    state.zoning = true;
    state.online = false;
}

HAILSTONE_API void OnZoned() noexcept {
    try {
        state.zoning = false;
        if (state.initialized) refresh_character();
    } catch (...) {
        state.online = false;
    }
}

HAILSTONE_API void OnPulse() noexcept {
    try {
        if (!state.initialized) return;
        const auto now = Clock::now();
        state.tracker.tick(now);
        if (!state.online && !state.zoning && now >= state.refresh_at) {
            refresh_character();
            state.refresh_at = now + std::chrono::seconds(1);
        }
    } catch (...) {}
}

HAILSTONE_API int OnPacket(int stream, int direction, HailstonePacket* packet) noexcept {
    try {
        if (stream != HAILSTONE_STREAM_ZONE || direction != HAILSTONE_IN || !packet ||
            packet->opcode != random_reply_opcode || packet->size != random_reply_size ||
            !packet->data || !ready()) return 0;
        const std::uint8_t* const data = packet->data;
        const auto low = read_le32(data);
        const auto high = read_le32(data + 4);
        const auto value = read_le32(data + 8);
        if (low > high || high > max_roll_value || value < low || value > high) return 0;

        const auto* const name = reinterpret_cast<const char*>(data + 12);
        const auto* const end = std::find(name, name + 64, '\0');
        if (end == name || end == name + 64) return 0;
        (void)state.tracker.ingest(std::string_view(name, static_cast<std::size_t>(end - name)),
                                   {static_cast<int>(low), static_cast<int>(high)},
                                   static_cast<int>(value), Clock::now());
    } catch (...) {}
    return 0;
}
