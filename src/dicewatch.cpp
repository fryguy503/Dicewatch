#include "announcements.hpp"
#include "clipboard.hpp"
#include "ledger.hpp"
#include "raid_summary.hpp"
#include "rounds.hpp"
#include "hailstone_plugin.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <random>
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
enum class AnnouncementEvent { Opened, Awarded };
enum class NoticeKind { Submitted, Copied, Blocked };

struct Preferences {
    Settings defaults;
    View view = View::Ranked;
    bool mini = false;
    AnnouncementSettings announcements;
};

struct AnnouncementNotice {
    AnnouncementEvent event;
    NoticeKind kind;
    std::string message;
    std::string status;
    std::uint64_t revision = 0;
};

struct Runtime {
    Tracker tracker;
    Preferences preferences;
    std::filesystem::path profile;
    std::string character;
    std::string message;
    std::string session;
    std::vector<LedgerEntry> ledger;
    std::vector<std::string> pending_ledger;
    std::string ledger_load_error;
    std::string ledger_write_error;
    std::optional<AnnouncementNotice> announcement;
    std::uint64_t observed_history = 0;
    std::uint64_t ledger_revision = 0;
    std::uint64_t announcement_revision = 0;
    RoundId selected = 0;
    Time refresh_at;
    Time ledger_retry_at;
    bool open = false;
    bool online = false;
    bool zoning = false;
    bool initialized = false;
    bool error = false;
};

Runtime state;
bool roll_command_registered = false;
bool panel_command_registered = false;
bool dice_command_registered = false;
bool model_registered = false;
bool action_registered = false;
bool ledger_registered = false;
bool report_registered = false;

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

std::optional<AnnouncementSettings> read_announcements(std::string_view start_mode, std::string_view start_channel,
                                                       std::string_view award_mode, std::string_view award_channel) {
    const auto first_mode = parse_announcement_mode(start_mode);
    const auto first_channel = parse_announcement_channel(start_channel);
    const auto last_mode = parse_announcement_mode(award_mode);
    const auto last_channel = parse_announcement_channel(award_channel);
    if (!first_mode || !first_channel || !last_mode || !last_channel) return std::nullopt;
    return AnnouncementSettings{*first_mode, *last_mode, *first_channel, *last_channel};
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

bool chat_allowed() {
    return HAILSTONE_HOST_HAS(host, do_command) && HAILSTONE_HOST_HAS(host, caps) &&
        (host->caps(host->host) & HAILSTONE_CAP_CHAT) != 0;
}

const char* channel_title(AnnouncementChannel channel) {
    switch (channel) {
    case AnnouncementChannel::Raid: return "Raid say";
    case AnnouncementChannel::Group: return "Group";
    case AnnouncementChannel::Guild: return "Guild";
    case AnnouncementChannel::Say: return "Say";
    }
    return "Chat";
}

const char* chat_unavailable(AnnouncementChannel channel) {
    if (!chat_allowed()) return "Auto-send needs Hailstone chat permission. The message is ready to copy.";
    if (!state.online) return "Enter a zone to send chat. The message is ready to copy.";
    if (channel == AnnouncementChannel::Say) return nullptr;
    HailstonePlayer player{};
    player.size = sizeof player;
    if (!host->get_player(host->host, &player) || !player.in_zone || player.game_state != GAMESTATE_INGAME) {
        return "Chat channel availability is unknown. The message is ready to copy.";
    }
    switch (channel) {
    case AnnouncementChannel::Raid:
        return player.in_raid ? nullptr : "Raid say needs a raid. The message is ready to copy.";
    case AnnouncementChannel::Group:
        return player.in_group ? nullptr : "Group chat needs a group. The message is ready to copy.";
    case AnnouncementChannel::Guild: {
        HailstoneSpawn spawn{};
        spawn.size = sizeof spawn;
        if (!HAILSTONE_HOST_HAS(host, get_spawn) || !host->get_spawn(host->host, player.spawn_id, &spawn)) {
            return "Guild membership is unavailable. The message is ready to copy.";
        }
        return spawn.guild_id != 0 && spawn.guild_id != std::numeric_limits<std::uint32_t>::max() ? nullptr
            : "Guild chat needs a guild. The message is ready to copy.";
    }
    case AnnouncementChannel::Say: return nullptr;
    }
    return "Choose a chat channel. The message is ready to copy.";
}

void announce(AnnouncementEvent event, AnnouncementText message) {
    const auto& settings = state.preferences.announcements;
    const auto mode = event == AnnouncementEvent::Opened ? settings.start_mode : settings.award_mode;
    if (mode == AnnouncementMode::Off || message.plain.empty()) return;

    AnnouncementNotice result{event, NoticeKind::Blocked, std::move(message.plain), {}, ++state.announcement_revision};
    if (mode == AnnouncementMode::Copy) {
        if (write_clipboard(result.message)) {
            result.kind = NoticeKind::Copied;
            result.status = "Announcement copied to the clipboard.";
        } else {
            result.status = "The clipboard is unavailable. The announcement is ready for manual copy.";
        }
    } else {
        const auto channel = event == AnnouncementEvent::Opened ? settings.start_channel : settings.award_channel;
        if (const auto* error = chat_unavailable(channel)) {
            result.status = error;
        } else {
            auto command = chat_command(channel, message.with_item_link);
            if (!command) command = chat_command(channel, result.message);
            if (command) {
                host->do_command(host->host, command->c_str());
                result.kind = NoticeKind::Submitted;
                result.status = std::string("Announcement submitted to ") + channel_title(channel) + '.';
            } else {
                result.status = "This announcement is ready for manual copy.";
            }
        }
    }
    state.announcement = std::move(result);
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
        (version != "dicewatch1" && version != "dicewatch2") || (mini != 0 && mini != 1)) return {};
    const auto settings = read_defaults(duplicate, order, duration);
    const auto layout = read_view(view);
    if (!settings || !layout) return {};
    Preferences preferences{*settings, *layout, mini != 0, {}};
    if (version == "dicewatch2") {
        std::string start_mode, start_channel, award_mode, award_channel;
        if (!(line >> start_mode >> start_channel >> award_mode >> award_channel)) return {};
        const auto announcements = read_announcements(start_mode, start_channel, award_mode, award_channel);
        if (!announcements) return {};
        preferences.announcements = *announcements;
    }
    return (line >> extra) ? Preferences{} : preferences;
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
        const auto& announcements = preferences.announcements;
        out << "dicewatch2 " << duplicate_name(preferences.defaults.duplicates) << ' '
            << order_name(preferences.defaults.order) << ' ' << preferences.defaults.duration_seconds << ' '
            << view_name(preferences.view) << ' ' << static_cast<int>(preferences.mini) << ' '
            << announcement_mode_name(announcements.start_mode) << ' ' << announcement_channel_name(announcements.start_channel) << ' '
            << announcement_mode_name(announcements.award_mode) << ' ' << announcement_channel_name(announcements.award_channel) << '\n';
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

std::string new_session() {
    constexpr std::string_view hex = "0123456789abcdef";
    std::random_device random;
    std::string token;
    token.reserve(32);
    for (int word = 0; word < 4; ++word) {
        const auto value = static_cast<std::uint32_t>(random());
        for (int shift = 28; shift >= 0; shift -= 4) token.push_back(hex[(value >> shift) & 15]);
    }
    return token;
}

std::string ledger_key(RoundId id) {
    return state.session + '-' + std::to_string(id);
}

auto find_ledger(std::string_view key) {
    return std::find_if(state.ledger.begin(), state.ledger.end(), [&](const LedgerEntry& entry) {
        return entry.key == key;
    });
}

std::string local_date(std::int64_t unix_time) {
    const auto time = static_cast<std::time_t>(unix_time);
    std::tm local{};
#if defined(_WIN32)
    if (localtime_s(&local, &time) != 0) return {};
#else
    if (!localtime_r(&time, &local)) return {};
#endif
    std::array<char, 32> date{};
    if (std::strftime(date.data(), date.size(), "%Y-%m-%d %H:%M", &local) == 0) return {};
    return date.data();
}

void sync_ledger(Time now, bool flush = false) {
    if (state.session.empty()) return;
    const auto revision = state.tracker.history_revision();
    const bool changed = state.observed_history != revision;
    if (!changed && (state.pending_ledger.empty() || (!flush && now < state.ledger_retry_at))) return;

    if (changed) {
        const auto wall = std::chrono::system_clock::now();
        for (auto it = state.tracker.history().rbegin(); it != state.tracker.history().rend(); ++it) {
            const auto key = ledger_key(it->id);
            if (find_ledger(key) != state.ledger.end()) continue;
            const auto elapsed = std::max(Clock::duration::zero(), now - *it->ended_at);
            const auto ended = std::chrono::duration_cast<std::chrono::seconds>((wall - elapsed).time_since_epoch());
            state.ledger.insert(state.ledger.begin(), LedgerEntry{key, state.session, ended.count(), *it, {}});
            state.pending_ledger.push_back(key);
        }
        state.observed_history = revision;
    }

    const auto previous_error = state.ledger_write_error;
    state.ledger_write_error.clear();
    state.pending_ledger.erase(std::remove_if(state.pending_ledger.begin(), state.pending_ledger.end(), [](const std::string& key) {
        const auto entry = find_ledger(key);
        if (entry == state.ledger.end()) return true;
        auto error = save_ledger_entry(state.profile, *entry);
        if (error.empty()) return true;
        state.ledger_write_error = std::move(error);
        return false;
    }), state.pending_ledger.end());
    state.ledger_retry_at = now + std::chrono::seconds(3);

    if (state.ledger.size() > max_ledger_entries) {
        state.ledger.resize(max_ledger_entries);
        const auto pending = std::remove_if(state.pending_ledger.begin(), state.pending_ledger.end(), [](const std::string& key) {
            return find_ledger(key) == state.ledger.end();
        });
        if (pending != state.pending_ledger.end()) {
            state.ledger_write_error = "Some older rounds could not be saved before the ledger reached its display limit.";
            state.pending_ledger.erase(pending, state.pending_ledger.end());
        }
    }
    if (changed || previous_error != state.ledger_write_error) ++state.ledger_revision;
}

void leave_game() {
    sync_ledger(Clock::now(), true);
    state.online = false;
    state.open = false;
    state.tracker = {};
    state.preferences = {};
    state.profile.clear();
    state.character.clear();
    state.session.clear();
    state.ledger.clear();
    state.pending_ledger.clear();
    state.ledger_load_error.clear();
    state.ledger_write_error.clear();
    state.announcement.reset();
    state.observed_history = 0;
    state.ledger_revision = 0;
    state.announcement_revision = 0;
    state.selected = 0;
    state.refresh_at = {};
    state.ledger_retry_at = {};
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
        state.session = new_session();
        if (!state.profile.empty()) {
            state.preferences = load_preferences(state.profile);
            auto ledger = load_ledger(state.profile);
            state.ledger = std::move(ledger.entries);
            state.ledger_load_error = std::move(ledger.error);
        } else {
            state.ledger_load_error = "The character profile is unavailable. New rounds can only be kept for this session.";
        }
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

int action_reply(HailstoneOut* out, bool ok, const std::string& message) {
    Output output(out);
    output.table();
    output.boolean("ok", ok);
    output.string("message", message);
    output.close();
    return 1;
}

void emit_announcements(Output& output) {
    Round sample;
    sample.spec = {{200, 1000}, "Band of Echoes", {}};
    sample.settings = state.preferences.defaults;
    if (sample.settings.duration_seconds > 0) sample.deadline = Time{} + std::chrono::seconds(sample.settings.duration_seconds);

    const auto& settings = state.preferences.announcements;
    output.table("announcements");
    output.string("start_mode", announcement_mode_name(settings.start_mode));
    output.string("start_channel", announcement_channel_name(settings.start_channel));
    output.string("award_mode", announcement_mode_name(settings.award_mode));
    output.string("award_channel", announcement_channel_name(settings.award_channel));
    output.boolean("chat_allowed", chat_allowed());
    output.boolean("clipboard_available", clipboard_available());
    output.string("start_preview", opening_announcement(sample).plain);

    sample.settings.order = WinnerOrder::High;
    sample.deadline.reset();
    sample.ended_at = Time{};
    sample.end_reason = EndReason::Manual;
    sample.rolls = {{"Trust", 921, {}, RollStatus::Counted}, {"Wren", 803, {}, RollStatus::Counted}};
    output.string("award_preview", award_announcement(LedgerEntry{{}, {}, 0, std::move(sample), "Wren"}).plain);
    output.close();

    if (state.announcement) {
        const auto& notice = *state.announcement;
        output.table("announcement");
        output.string("event", notice.event == AnnouncementEvent::Opened ? "start" : "award");
        output.string("kind", notice.kind == NoticeKind::Submitted ? "sent" : notice.kind == NoticeKind::Copied ? "copy" : "blocked");
        output.string("message", notice.message);
        output.string("status", notice.status);
        output.integer("revision", static_cast<std::int64_t>(notice.revision));
        output.close();
    }
}

void emit_round(Output& output, const Round& round, Time now, bool detail, bool summary,
                const char* key = nullptr, const LedgerEntry* receipt = nullptr) {
    const auto standings = round.standings();
    const auto leader = leading_text(round, standings);
    output.table(key);
    output.integer("id", static_cast<std::int64_t>(round.id));
    output.string("label", round.spec.label);
    output.integer("low", round.spec.range.low);
    output.integer("high", round.spec.range.high);
    output.string("order", order_name(round.settings.order));
    output.boolean("closed", !round.open());
    output.string("status", status(round));
    output.integer("total", static_cast<std::int64_t>(round.rolls.size()));
    output.integer("counted", static_cast<std::int64_t>(standings.size()));
    output.integer("dropped", static_cast<std::int64_t>(round.dropped_rolls));
    output.integer("remaining", round.remaining_seconds(now).value_or(-1));
    output.string("leader", leader);
    output.boolean("linked", round.spec.item.has_value());
    if (receipt) {
        output.string("key", receipt->key);
        output.string("session", receipt->session);
        output.string("date", local_date(receipt->ended_unix));
        output.string("recipient", receipt->recipient);
    }
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
            output.string("ledger_session", state.session);
            output.integer("ledger_revision", static_cast<std::int64_t>(state.ledger_revision));
            emit_announcements(output);
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
            if (const auto* round = state.tracker.find(state.selected)) {
                const auto entry = round->open() ? state.ledger.end() : find_ledger(ledger_key(round->id));
                emit_round(output, *round, now, true, true, "current", entry == state.ledger.end() ? nullptr : &*entry);
            }
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

int panel_ledger(void*, const HailstoneArgs* args, HailstoneOut* out) noexcept {
    try {
        Output output(out);
        output.table();
        output.string("current_session", state.session);
        const auto now = Clock::now();
        if (state.open && !state.session.empty()) sync_ledger(now);
        const auto draft = string_arg(args, 0, 64);
        const auto error = !state.ledger_write_error.empty() ? state.ledger_write_error : state.ledger_load_error;
        if (!error.empty()) output.string("error", error);
        output.array("entries");
        if (state.open) {
            for (const auto& entry : state.ledger) emit_round(output, entry.round, now, false, false, nullptr, &entry);
        }
        output.close();
        output.array("sessions");
        if (state.open && !state.session.empty()) {
            output.table();
            output.string("key", state.session);
            output.string("label", "This session");
            output.close();
            std::vector<std::string_view> sessions{state.session};
            for (const auto& entry : state.ledger) {
                if (std::find(sessions.begin(), sessions.end(), entry.session) != sessions.end()) continue;
                sessions.push_back(entry.session);
                output.table();
                output.string("key", entry.session);
                output.string("label", local_date(entry.ended_unix));
                output.close();
            }
        }
        output.close();
        if (state.open && draft) {
            const auto selected = find_ledger(*draft);
            if (selected != state.ledger.end()) emit_round(output, selected->round, now, true, true, "selected", &*selected);
        }
        output.close();
        return 1;
    } catch (...) {
        return 0;
    }
}

int panel_raid_summary(void*, const HailstoneArgs* args, HailstoneOut* out) noexcept {
    try {
        Output output(out);
        output.array(nullptr);
        const auto session = string_arg(args, 0, 32);
        if (state.open && !state.session.empty() && session && valid_ledger_session(*session)) {
            std::string label = "This session";
            if (*session != state.session) {
                const auto entry = std::find_if(state.ledger.begin(), state.ledger.end(), [&](const LedgerEntry& record) {
                    return record.session == *session;
                });
                label = entry == state.ledger.end() ? "Previous session" : local_date(entry->ended_unix);
            }
            for (const auto& page : make_raid_summary(label, state.ledger, *session)) output.string(nullptr, page);
        }
        output.close();
        return 1;
    } catch (...) {
        return 0;
    }
}

std::string record_award(const HailstoneArgs* args) {
    if (state.session.empty() || state.profile.empty()) return "The character's loot ledger is unavailable.";
    const auto key = string_arg(args, 1, 64);
    const auto name = string_arg(args, 2, max_player_bytes);
    if (!key || !name) return "Select a finished round and enter up to 64 bytes for the recipient.";
    auto recipient = *name;
    const auto first = recipient.find_first_not_of(" \t");
    recipient = first == std::string_view::npos ? std::string_view{} : recipient.substr(first, recipient.find_last_not_of(" \t") - first + 1);
    if (!valid_ledger_recipient(recipient)) return "Use a recipient name without control characters.";
    const auto entry = find_ledger(*key);
    if (entry == state.ledger.end()) return "This round is no longer in the loot ledger.";
    const auto fold = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    const bool changed = !std::equal(entry->recipient.begin(), entry->recipient.end(), recipient.begin(), recipient.end(),
                                    [&](char left, char right) { return fold(left) == fold(right); });
    if (entry->recipient == recipient &&
        std::find(state.pending_ledger.begin(), state.pending_ledger.end(), *key) == state.pending_ledger.end()) return {};
    auto updated = *entry;
    updated.recipient = recipient;
    const auto error = save_ledger_entry(state.profile, updated);
    if (!error.empty()) return error;
    entry->recipient = std::move(updated.recipient);
    state.pending_ledger.erase(std::remove(state.pending_ledger.begin(), state.pending_ledger.end(), *key), state.pending_ledger.end());
    if (state.pending_ledger.empty()) state.ledger_write_error.clear();
    ++state.ledger_revision;
    if (changed && !entry->recipient.empty() && entry->session == state.session && state.online) {
        announce(AnnouncementEvent::Awarded, award_announcement(*entry));
    }
    return {};
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
        const auto now = Clock::now();
        const auto result = state.tracker.start(std::move(*parsed.spec), settings, now);
        sync_ledger(now);
        if (result.id) {
            state.selected = result.id;
            note("Opened " + range_label(range) + " for rolls.");
            if (const auto* round = state.tracker.find(result.id)) {
                announce(AnnouncementEvent::Opened, opening_announcement(*round));
            }
        } else {
            note(result.error, true);
        }
    }
    if (from_chat) say(state.message);
}

int panel_action(void*, const HailstoneArgs* args, HailstoneOut* out) noexcept {
    try {
        const auto action = string_arg(args, 0, 24);
        if (!action) return 0;
        if (*action == "hide") {
            state.open = false;
            return 0;
        }
        if (!state.open) return 0;
        if (*action == "announcements") {
            const auto start_mode = string_arg(args, 1, 8);
            const auto start_channel = string_arg(args, 2, 8);
            const auto award_mode = string_arg(args, 3, 8);
            const auto award_channel = string_arg(args, 4, 8);
            const auto settings = start_mode && start_channel && award_mode && award_channel
                ? read_announcements(*start_mode, *start_channel, *award_mode, *award_channel) : std::nullopt;
            if (!settings) return action_reply(out, false, "Choose valid announcement modes and channels.");
            state.preferences.announcements = *settings;
            return action_reply(out, true, save_preferences() ? "Announcement settings saved."
                : "Announcement settings saved for this session.");
        }
        if (*action == "announcement_dismiss") {
            const auto revision = integer_arg(args, 1);
            if (state.announcement && revision && *revision > 0 &&
                static_cast<std::uint64_t>(*revision) == state.announcement->revision) state.announcement.reset();
            return 0;
        }
        if (*action == "announcement_copy") {
            if (!state.announcement) return action_reply(out, false, "There is no announcement to copy.");
            const auto revision = integer_arg(args, 1);
            if (!revision || *revision <= 0 || static_cast<std::uint64_t>(*revision) != state.announcement->revision) {
                return action_reply(out, false, "The announcement changed. Review the new text before copying.");
            }
            if (!write_clipboard(state.announcement->message)) {
                return action_reply(out, false, "The clipboard is unavailable. Select the text and use Copy.");
            }
            state.announcement->kind = NoticeKind::Copied;
            state.announcement->status = "Announcement copied to the clipboard.";
            state.announcement->revision = ++state.announcement_revision;
            return action_reply(out, true, state.announcement->status);
        }
        if (*action == "award") {
            const auto error = record_award(args);
            return action_reply(out, error.empty(), error.empty() ? "Updated loot recipient." : error);
        }
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
            const auto now = Clock::now();
            if (id != state.selected || !state.tracker.end(id, now)) note("This round is already closed or no longer selected.", true);
            else note("Round ended.");
            sync_ledger(now);
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
    if (report_registered) host->remove_ui_function(host->host, "raid_summary");
    if (ledger_registered) host->remove_ui_function(host->host, "ledger");
    if (action_registered) host->remove_ui_function(host->host, "action");
    if (model_registered) host->remove_ui_function(host->host, "state");
    if (roll_command_registered) host->remove_command(host->host, "/itemroll");
    if (panel_command_registered) host->remove_command(host->host, "/dicewatch");
    if (dice_command_registered) host->remove_command(host->host, "/dice");
    dice_command_registered = false;
    report_registered = ledger_registered = action_registered = model_registered = roll_command_registered = panel_command_registered = false;
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
        ledger_registered = host->add_ui_function(host->host, "ledger", panel_ledger, nullptr) != 0;
        report_registered = host->add_ui_function(host->host, "raid_summary", panel_raid_summary, nullptr) != 0;
        if (!(panel_command_registered && roll_command_registered && model_registered && action_registered &&
              ledger_registered && report_registered)) {
            unregister();
            if (HAILSTONE_HOST_HAS(host, log)) host->log(host->host, 2, "Dicewatch commands or UI were unavailable.");
            return;
        }
        dice_command_registered = host->add_command(host->host, "/dice", show_command, nullptr) != 0;
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
        sync_ledger(now);
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
