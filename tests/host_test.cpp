#include "hailstone_plugin.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

extern "C" {
std::uint32_t hailstone_plugin_abi();
void hailstone_plugin_set_host(const PluginHostApi* api);
void InitializePlugin() noexcept;
void ShutdownPlugin() noexcept;
void SetGameState(int game_state) noexcept;
void OnBeginZone() noexcept;
void OnZoned() noexcept;
int OnPacket(int stream, int direction, HailstonePacket* packet) noexcept;
}

using HostValue = std::variant<bool, std::int64_t, std::string>;

struct HailstoneArgs {
    std::vector<HostValue> values;
};

struct Node {
    enum class Kind { Scalar, Object, Array };
    Kind kind = Kind::Scalar;
    HostValue value = false;
    std::map<std::string, Node> fields;
    std::vector<Node> items;

    const Node& at(const char* key) const { return fields.at(key); }
    bool boolean(const char* key) const { return std::get<bool>(at(key).value); }
    std::int64_t integer(const char* key) const { return std::get<std::int64_t>(at(key).value); }
    const std::string& string(const char* key) const { return std::get<std::string>(at(key).value); }
};

struct HailstoneOut {
    struct Scope {
        Node* node;
        std::string key;
    };
    Node root;
    std::vector<Scope> scopes;
    bool root_written = false;
};

namespace {

#define CHECK(...) do { \
    if (!(__VA_ARGS__)) throw std::runtime_error("Line " + std::to_string(__LINE__) + ": " #__VA_ARGS__); \
} while (false)

constexpr std::uint32_t permission_mask =
    HAILSTONE_CAP_READ | HAILSTONE_CAP_COMMANDS | HAILSTONE_CAP_PACKETS | HAILSTONE_CAP_UI;
constexpr std::uint16_t random_reply_opcode = 0x106b;

Node& append(HailstoneOut& out, Node node) {
    if (out.scopes.empty()) {
        CHECK(!out.root_written);
        out.root_written = true;
        out.root = std::move(node);
        return out.root;
    }
    auto& scope = out.scopes.back();
    if (scope.node->kind == Node::Kind::Array) return scope.node->items.emplace_back(std::move(node));
    CHECK(scope.node->kind == Node::Kind::Object && !scope.key.empty());
    auto [entry, inserted] = scope.node->fields.emplace(std::move(scope.key), std::move(node));
    CHECK(inserted);
    scope.key.clear();
    return entry->second;
}

void begin_node(HailstoneOut& out, Node::Kind kind) {
    Node node;
    node.kind = kind;
    auto& inserted = append(out, std::move(node));
    out.scopes.push_back({&inserted, {}});
}

class TestProfile {
public:
    TestProfile() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        directory = std::filesystem::temp_directory_path() / ("dicewatch-host-" + std::to_string(nonce));
        CHECK(std::filesystem::create_directory(directory));
    }

    ~TestProfile() {
        std::error_code ignore;
        std::filesystem::remove(directory / "dicewatch-settings.txt", ignore);
        std::filesystem::remove(directory, ignore);
    }

    TestProfile(const TestProfile&) = delete;
    TestProfile& operator=(const TestProfile&) = delete;

    std::filesystem::path directory;
};

class FakeHost {
public:
    std::uint32_t capabilities = permission_mask;
    std::int32_t game_state = GAMESTATE_INGAME;
    bool in_zone = true;
    std::string character = "Trust";
    std::vector<std::string> chat;

    explicit FakeHost(const std::filesystem::path& directory) : profile_(directory.string()) {
        api_.abi = HAILSTONE_PLUGIN_ABI;
        api_.size = sizeof api_;
        api_.host = this;

        api_.caps = [](void* opaque) { return self(opaque).capabilities; };
        api_.write_chat = [](void* opaque, const char* line, std::uint32_t) { self(opaque).chat.emplace_back(line); };
        api_.get_player = [](void* opaque, HailstonePlayer* player) {
            const auto& fixture = self(opaque);
            CHECK(player->size == sizeof *player);
            player->spawn_id = 7;
            player->game_state = fixture.game_state;
            player->in_zone = fixture.in_zone ? 1 : 0;
            CHECK(fixture.character.size() < sizeof player->name);
            std::memcpy(player->name, fixture.character.c_str(), fixture.character.size() + 1);
            return 1;
        };
        api_.character_config_dir = [](void* opaque, char* out, std::size_t capacity) {
            const auto& profile = self(opaque).profile_;
            CHECK(profile.size() < capacity);
            std::memcpy(out, profile.c_str(), profile.size() + 1);
            return static_cast<int>(profile.size());
        };

        api_.add_command = [](void* opaque, const char* name, HailstoneCommandFn fn, void* context) {
            return self(opaque).commands_.emplace(name, std::make_pair(fn, context)).second ? 1 : 0;
        };
        api_.remove_command = [](void* opaque, const char* name) { self(opaque).commands_.erase(name); };
        api_.add_ui_function = [](void* opaque, const char* name, HailstoneUiFn fn, void* context) {
            return self(opaque).functions_.emplace(name, std::make_pair(fn, context)).second ? 1 : 0;
        };
        api_.remove_ui_function = [](void* opaque, const char* name) { self(opaque).functions_.erase(name); };

        api_.arg = [](void*, const HailstoneArgs* args, std::size_t index, HailstoneValue* value) {
            if (index >= args->values.size()) return 0;
            const auto& arg = args->values[index];
            if (const auto* text = std::get_if<std::string>(&arg)) *value = {HAILSTONE_STRING, 0, 0, text->c_str()};
            else if (const auto* number = std::get_if<std::int64_t>(&arg)) *value = {HAILSTONE_INT, *number, 0, nullptr};
            else *value = {HAILSTONE_BOOL, std::get<bool>(arg) ? 1 : 0, 0, nullptr};
            return 1;
        };
        api_.out_begin_table = [](void*, HailstoneOut* out) { begin_node(*out, Node::Kind::Object); };
        api_.out_begin_array = [](void*, HailstoneOut* out) { begin_node(*out, Node::Kind::Array); };
        api_.out_key = [](void*, HailstoneOut* out, const char* key) {
            CHECK(!out->scopes.empty() && out->scopes.back().key.empty());
            out->scopes.back().key = key;
        };
        api_.out_end = [](void*, HailstoneOut* out) {
            CHECK(!out->scopes.empty() && out->scopes.back().key.empty());
            out->scopes.pop_back();
        };
        api_.out_value = [](void*, HailstoneOut* out, const HailstoneValue* value) {
            Node node;
            switch (value->kind) {
            case HAILSTONE_STRING: node.value = std::string(value->s); break;
            case HAILSTONE_INT: node.value = value->i; break;
            case HAILSTONE_BOOL: node.value = value->i != 0; break;
            default: throw std::runtime_error("Unexpected UI value type");
            }
            (void)append(*out, std::move(node));
        };
    }

    ~FakeHost() {
        ShutdownPlugin();
        hailstone_plugin_set_host(nullptr);
    }

    FakeHost(const FakeHost&) = delete;
    FakeHost& operator=(const FakeHost&) = delete;

    void initialize() {
        CHECK(hailstone_plugin_abi() == abi());
        hailstone_plugin_set_host(&api_);
        InitializePlugin();
        CHECK(commands_.count("/itemroll") == 1 && commands_.count("/dicewatch") == 1);
        CHECK(functions_.count("state") == 1 && functions_.count("action") == 1);
    }

    void shutdown() {
        ShutdownPlugin();
        CHECK(commands_.empty() && functions_.empty());
    }

    void command(const char* name, const std::string& args = {}) {
        const auto& [fn, context] = commands_.at(name);
        fn(context, args.c_str());
    }

    Node model() {
        const auto& [fn, context] = functions_.at("state");
        HailstoneOut out;
        const HailstoneArgs args;
        CHECK(fn(context, &args, &out) == 1);
        CHECK(out.root_written && out.scopes.empty());
        return std::move(out.root);
    }

    void action(std::vector<HostValue> values) {
        const auto& [fn, context] = functions_.at("action");
        HailstoneOut out;
        const HailstoneArgs args{std::move(values)};
        CHECK(fn(context, &args, &out) == 0);
        CHECK(!out.root_written && out.scopes.empty());
    }

private:
    static constexpr std::uint32_t abi() { return HAILSTONE_PLUGIN_ABI; }
    static FakeHost& self(void* opaque) { return *static_cast<FakeHost*>(opaque); }

    std::string profile_;
    PluginHostApi api_{};
    std::map<std::string, std::pair<HailstoneCommandFn, void*>> commands_;
    std::map<std::string, std::pair<HailstoneUiFn, void*>> functions_;
};

std::array<std::uint8_t, 76> reply(std::uint32_t low, std::uint32_t high,
                                  std::uint32_t result, std::string_view player) {
    std::array<std::uint8_t, 76> payload{};
    const auto write_le32 = [&](std::size_t at, std::uint32_t value) {
        for (std::size_t n = 0; n < 4; ++n) {
            payload[at + n] = static_cast<std::uint8_t>(value & 0xff);
            value >>= 8;
        }
    };
    write_le32(0, low);
    write_le32(4, high);
    write_le32(8, result);
    CHECK(player.size() <= 64);
    for (std::size_t n = 0; n < player.size(); ++n) payload[12 + n] = static_cast<std::uint8_t>(player[n]);
    return payload;
}

void dispatch(std::array<std::uint8_t, 76> payload, int stream = HAILSTONE_STREAM_ZONE,
              int direction = HAILSTONE_IN, std::uint16_t opcode = random_reply_opcode,
              std::size_t size = 76, bool has_data = true) {
    auto* const data = has_data ? payload.data() : nullptr;
    HailstonePacket packet{opcode, data, size};
    const auto original = payload;
    CHECK(OnPacket(stream, direction, &packet) == 0);
    CHECK(packet.opcode == opcode && packet.size == size && packet.data == data && payload == original);
}

std::string stock_item_link() {
    // Tag code, five-character item ID and the remaining fixed-width hexadecimal fields.
    return std::string(1, '\x12') + "001BCD" + std::string(50, '0') + "Greaves of the Vale" + '\x12';
}

void commands_roll_packets_and_ui() {
    TestProfile profile;
    FakeHost fake(profile.directory);
    fake.initialize();
    CHECK(!fake.model().boolean("open"));

    fake.command("/itemroll", "1000 Trust's armor");
    auto state = fake.model();
    CHECK(state.boolean("open") && state.boolean("ready"));
    const auto first = state.integer("selected");
    CHECK(first > 0 && state.at("current").integer("low") == 0 && state.at("current").integer("high") == 1000);
    CHECK(state.at("current").string("order") == "high");
    CHECK(state.at("current").string("label") == "Trust's armor" && !state.at("current").boolean("linked"));

    fake.command("/itemroll", "200-1000 " + stock_item_link());
    state = fake.model();
    const auto second = state.integer("selected");
    CHECK(second > first && state.at("current").boolean("linked"));
    CHECK(state.at("current").string("label") == "Greaves of the Vale");
    CHECK(state.at("current").integer("low") == 200 && state.at("current").integer("high") == 1000);

    fake.command("/itemroll", "200-1000 Another round");
    state = fake.model();
    CHECK(state.boolean("error") && state.at("rounds").items.size() == 2);
    CHECK(state.at("current").string("label") == "Greaves of the Vale");
    CHECK(fake.chat.back().find("already uses") != std::string::npos);

    // A decoded Bastion/RoF2 reply for 0-1000 belongs to the first round while the second is selected.
    dispatch(reply(0, 1000, 921, "Trust"));
    dispatch(reply(200, 1000, 731, "Wren"));
    dispatch(reply(200, 1000, 999, "wren"));
    state = fake.model();
    CHECK(state.at("rounds").items.at(0).integer("total") == 1);
    CHECK(state.at("current").integer("total") == 2 && state.at("current").integer("counted") == 1);
    CHECK(state.at("current").at("ranked").items.at(0).integer("value") == 731);
    CHECK(state.at("current").at("log").items.at(0).string("status") == "Duplicate");

    const auto valid = reply(200, 1000, 800, "Second");
    dispatch(valid, HAILSTONE_STREAM_WORLD);
    dispatch(valid, HAILSTONE_STREAM_ZONE, HAILSTONE_OUT);
    dispatch(valid, HAILSTONE_STREAM_ZONE, HAILSTONE_IN, 0x106a);
    dispatch(valid, HAILSTONE_STREAM_ZONE, HAILSTONE_IN, random_reply_opcode, 75);
    dispatch(valid, HAILSTONE_STREAM_ZONE, HAILSTONE_IN, random_reply_opcode, 77);
    dispatch(valid, HAILSTONE_STREAM_ZONE, HAILSTONE_IN, random_reply_opcode, 76, false);
    CHECK(OnPacket(HAILSTONE_STREAM_ZONE, HAILSTONE_IN, nullptr) == 0);
    dispatch(reply(1, 1000, 800, "Second"));
    dispatch(reply(200, 1000, 1001, "Second"));
    dispatch(reply(200, 1000, 199, "Second"));
    dispatch(reply(1000, 200, 400, "Second"));
    dispatch(reply(200, 0xffffffffu, 800, "Second"));
    dispatch(reply(200, 1000, 800, ""));
    dispatch(reply(200, 1000, 800, std::string(64, 'X')));
    dispatch(reply(200, 1000, 800, "Bad\nName"));
    CHECK(fake.model().at("current").integer("total") == 2);

    fake.action({std::string("select"), first});
    state = fake.model();
    const auto& ranked = state.at("current").at("ranked").items.at(0);
    CHECK(ranked.string("player") == "Trust" && ranked.integer("value") == 921);

    fake.action({std::string("timer"), first, std::int64_t(3600)});
    CHECK(fake.model().at("current").integer("remaining") > 0);
    fake.action({std::string("timer"), second, std::int64_t(1)});
    CHECK(fake.model().boolean("error"));
    fake.action({std::string("timer"), first, std::int64_t(0)});
    CHECK(fake.model().at("current").integer("remaining") == -1);
    fake.action({std::string("end"), first});
    CHECK(fake.model().at("current").boolean("closed"));
    CHECK(fake.model().at("history").items.at(0).string("summary").find("921") != std::string::npos);
    fake.action({std::string("select"), second});
    CHECK(!fake.model().at("current").boolean("closed"));
    fake.shutdown();
}

void zone_and_capability_changes() {
    TestProfile profile;
    FakeHost fake(profile.directory);
    fake.initialize();
    fake.command("/itemroll", "500 Loot");
    const auto original = fake.model().integer("selected");

    fake.in_zone = false;
    OnBeginZone();
    CHECK(!fake.model().boolean("ready"));
    dispatch(reply(0, 500, 220, "Zoning"));
    fake.command("/itemroll", "400 Other loot");
    CHECK(!fake.model().boolean("ready") && fake.model().at("rounds").items.size() == 1);
    CHECK(fake.model().at("current").integer("total") == 0);

    fake.in_zone = true;
    OnZoned();
    CHECK(fake.model().boolean("ready") && fake.model().integer("selected") == original);
    dispatch(reply(0, 500, 300, "Zoned"));
    CHECK(fake.model().at("current").integer("total") == 1);

    fake.capabilities &= ~HAILSTONE_CAP_PACKETS;
    dispatch(reply(0, 500, 400, "Denied"));
    CHECK(!fake.model().boolean("ready") && fake.model().at("current").integer("total") == 1);
    fake.capabilities = permission_mask;

    fake.game_state = GAMESTATE_CHARSELECT;
    fake.in_zone = false;
    SetGameState(GAMESTATE_CHARSELECT);
    CHECK(!fake.model().boolean("open"));
    fake.character = "Other";
    fake.game_state = GAMESTATE_INGAME;
    fake.in_zone = true;
    SetGameState(GAMESTATE_INGAME);
    fake.command("/dicewatch");
    CHECK(fake.model().boolean("ready") && fake.model().at("rounds").items.empty());
    CHECK(fake.model().at("history").items.empty());
}

void preferences() {
    TestProfile profile;
    {
        std::ofstream invalid(profile.directory / "dicewatch-settings.txt", std::ios::binary);
        invalid << "dicewatch1 best low 3601 both 1\n";
    }
    FakeHost fake(profile.directory);
    fake.initialize();
    fake.command("/dicewatch");
    CHECK(fake.model().at("defaults").string("duplicate") == "first");

    fake.action({std::string("defaults"), std::string("best"), std::string("low"), std::int64_t(12)});
    CHECK(fake.model().at("defaults").string("duplicate") == "best");
    fake.action({std::string("defaults"), std::string("broken"), std::string("high"), std::int64_t(0)});
    CHECK(fake.model().boolean("error") && fake.model().at("defaults").string("duplicate") == "best");
    fake.action({std::string("layout"), std::string("both"), true});
    CHECK(fake.model().boolean("mini") && fake.model().string("view") == "both");

    fake.action({std::string("hide")});
    CHECK(!fake.model().boolean("open"));
    fake.shutdown();
    fake.initialize();
    fake.command("/dicewatch");
    const auto state = fake.model();
    CHECK(state.boolean("mini") && state.string("view") == "both");
    CHECK(state.at("defaults").string("duplicate") == "best" && state.at("defaults").string("order") == "low");
    CHECK(state.at("defaults").integer("duration") == 12);
    fake.command("/itemroll", "2500 Lowest wins");
    CHECK(fake.model().at("current").string("order") == "low");
}

} // namespace

int main() {
    try {
        commands_roll_packets_and_ui();
        zone_and_capability_changes();
        preferences();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "Dicewatch host ABI tests passed.\n";
    return EXIT_SUCCESS;
}
