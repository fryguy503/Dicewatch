#include "ledger.hpp"

#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

using namespace dicewatch;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

#define CHECK(...) do { \
    if (!(__VA_ARGS__)) throw std::runtime_error("Line " + std::to_string(__LINE__) + ": " #__VA_ARGS__); \
} while (false)

constexpr const char* session = "0123456789abcdef0123456789abcdef";
constexpr const char* other_session = "10123456789abcdef0123456789abcde";
const Time start = Time{} + 24h;

class Profile {
public:
    const fs::path path = fs::temp_directory_path() /
        ("dicewatch-ledger-test-" + std::to_string(Clock::now().time_since_epoch().count()));

    Profile() { CHECK(fs::create_directory(path)); }
    ~Profile() {
        remove_subdir(path / "loot-ledger");
        remove_subdir(path / "outside");
        std::error_code ignore;
        fs::remove(path, ignore);
    }

    Profile(const Profile&) = delete;
    Profile& operator=(const Profile&) = delete;

private:
    static void remove_subdir(const fs::path& directory) {
        std::error_code error;
        const auto type = fs::symlink_status(directory, error);
        if (!error && fs::is_directory(type)) {
            const fs::directory_iterator end;
            for (fs::directory_iterator current(directory, error); !error && current != end; current.increment(error)) {
                std::error_code ignore;
                fs::remove(current->path(), ignore);
            }
        }
        fs::remove(directory, error);
    }
};

fs::path receipt(const Profile& profile, const LedgerEntry& entry) {
    return profile.path / "loot-ledger" / (entry.key + ".dwl");
}

LedgerEntry fixture(RoundId id = 1, std::int64_t stamp = 1764000000, std::string token = session,
                    bool timed = false) {
    const auto tag = std::string(1, '\x12') + "0000F4241" + std::string(68, '0') + "Unbroken Crown" + '\x12';
    auto parsed = parse_itemroll("200-1000 " + tag);
    CHECK(parsed.spec && parsed.spec->item && parsed.spec->item->id == 1000001);
    Tracker tracker;
    const auto result = tracker.start(std::move(*parsed.spec), {DuplicatePolicy::Best, WinnerOrder::Low, timed ? 30 : 0}, start);
    CHECK(result.id);
    CHECK(tracker.ingest("Talia", {200, 1000}, 800, start + 150ms));
    CHECK(tracker.ingest("Trust", {200, 1000}, 330, start + 1s + 50ms));
    CHECK(tracker.ingest("talia", {200, 1000}, 200, start + 2s + 500ms));
    CHECK(tracker.ingest("Trust", {200, 1000}, 900, start + 4s + 73ms));
    if (timed) tracker.tick(start + 30s);
    else CHECK(tracker.end(result.id, start + 8s + 625ms));
    auto round = tracker.history().front();
    round.id = id;
    const auto key = token + '-' + std::to_string(id);
    return {key, std::move(token), stamp, std::move(round), {}};
}

void write_file(const fs::path& file, const std::string& bytes) {
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    CHECK(stream);
}

void roundtrip() {
    Profile profile;
    const auto empty = load_ledger(profile.path);
    CHECK(empty.entries.empty() && empty.error.empty());
    CHECK(!fs::exists(profile.path / "loot-ledger"));

    auto first = fixture();
    CHECK(save_ledger_entry(profile.path, first).empty());
    auto second = fixture(1, 1764000500, other_session, true);
    second.recipient = "Talia";
    CHECK(save_ledger_entry(profile.path, second).empty());

    auto loaded = load_ledger(profile.path);
    CHECK(loaded.error.empty() && loaded.entries.size() == 2);
    CHECK(loaded.entries[0].session == other_session && loaded.entries[0].recipient == "Talia");
    CHECK(loaded.entries[0].round.end_reason == EndReason::Timed);
    CHECK(loaded.entries[0].round.settings.duration_seconds == 30);
    CHECK(loaded.entries[0].round.ended_at == Time{} + 30s);
    const auto& ancient = loaded.entries[1];
    CHECK(ancient.session == session && ancient.key == first.key && ancient.recipient.empty());
    CHECK(ancient.round.spec.item && ancient.round.spec.item->raw == first.round.spec.item->raw);
    CHECK(ancient.round.spec.label == "Unbroken Crown");
    CHECK(ancient.round.spec.range == Range{200, 1000});
    CHECK(ancient.ended_unix == first.ended_unix);
    CHECK(ancient.round.ended_at == Time{} + 8s + 625ms && !ancient.round.open());
    CHECK(ancient.round.settings.order == WinnerOrder::Low);
    CHECK(ancient.round.settings.duplicates == DuplicatePolicy::Best);
    CHECK(ancient.round.rolls[0].status == RollStatus::Replaced);
    CHECK(ancient.round.rolls[3].status == RollStatus::Duplicate);
    CHECK(ancient.round.rolls[3].received_at == Time{} + 4s + 73ms);
    CHECK(ancient.round.standings().front().value == 200);

    first.recipient = "Trust";
    CHECK(save_ledger_entry(profile.path, first).empty());
    loaded = load_ledger(profile.path);
    CHECK(loaded.entries.size() == 2 && loaded.entries[1].recipient == "Trust");
    first.recipient.clear();
    CHECK(save_ledger_entry(profile.path, first).empty());
    const auto cleared = load_ledger(profile.path);
    CHECK(cleared.entries[1].recipient.empty());
}

void validation() {
    Profile profile;
    CHECK(valid_ledger_session(session) && valid_ledger_session(other_session));
    CHECK(!valid_ledger_session("ABCDEF0123456789ABCDEF0123456789") &&
          !valid_ledger_session("0123456789abcdef0123456789abcdez"));
    CHECK(valid_ledger_recipient("") && valid_ledger_recipient("Rene\xc3\xa9"));
    CHECK(!valid_ledger_recipient(" Trust") && !valid_ledger_recipient("Trust\t") &&
          !valid_ledger_recipient("Tru\xE2\x80\xAE" "st") && !valid_ledger_recipient(std::string(65, 'a')));

    auto row = fixture();
    row.key = "../receipt";
    CHECK(!save_ledger_entry(profile.path, row).empty());
    row = fixture();
    row.key = std::string(session) + "-01";
    CHECK(!save_ledger_entry(profile.path, row).empty());
    row = fixture();
    row.session = other_session;
    CHECK(!save_ledger_entry(profile.path, row).empty());
    row = fixture();
    row.round.id = 0;
    CHECK(!save_ledger_entry(profile.path, row).empty());
    row = fixture();
    row.ended_unix = 0;
    CHECK(!save_ledger_entry(profile.path, row).empty());
    row = fixture();
    row.ended_unix = 253402300800;
    CHECK(!save_ledger_entry(profile.path, row).empty());
    row = fixture();
    row.round.ended_at = start - 1ms;
    CHECK(!save_ledger_entry(profile.path, row).empty());
    row = fixture();
    row.round.ended_at.reset();
    CHECK(!save_ledger_entry(profile.path, row).empty());
    row = fixture();
    row.round.rolls[1].status = RollStatus::Duplicate;
    CHECK(!save_ledger_entry(profile.path, row).empty());
    row = fixture();
    row.round.rolls[1].received_at = start + 20s;
    CHECK(!save_ledger_entry(profile.path, row).empty());
    row = fixture();
    row.round.spec.range = {1, -1};
    CHECK(!save_ledger_entry(profile.path, row).empty());
    row = fixture();
    row.round.spec.item->id = 1;
    CHECK(!save_ledger_entry(profile.path, row).empty());
    row = fixture();
    row.recipient = "Trust\nWinner";
    CHECK(!save_ledger_entry(profile.path, row).empty());
    CHECK(!fs::exists(profile.path / "loot-ledger"));
}

void damage_and_safe_paths() {
    Profile profile;
    const auto record = fixture(1);
    CHECK(save_ledger_entry(profile.path, record).empty());
    const auto truncated = fixture(2);
    CHECK(save_ledger_entry(profile.path, truncated).empty());
    fs::resize_file(receipt(profile, truncated), 31);

    const auto modified = fixture(3);
    CHECK(save_ledger_entry(profile.path, modified).empty());
    {
        std::fstream file(receipt(profile, modified), std::ios::in | std::ios::out | std::ios::binary);
        file.seekp(-1, std::ios::end);
        file.put('\x7f');
        file.close();
        CHECK(file);
    }
    const auto oversized = fixture(4);
    write_file(receipt(profile, oversized), std::string(256 * 1024, 'x'));
    const auto wrong_name = fixture(5);
    fs::copy_file(receipt(profile, record), receipt(profile, wrong_name));
    write_file(profile.path / "loot-ledger" / "foreign.tmp", "An abandoned stage file");
    const auto original = load_ledger(profile.path);
    CHECK(!original.error.empty() && original.entries.size() == 1 && original.entries[0].key == record.key);
    // Repairing an existing damaged receipt is an atomic same-key replacement.
    CHECK(save_ledger_entry(profile.path, truncated).empty());
    const auto repaired = load_ledger(profile.path);
    CHECK(repaired.entries.size() == 2);

    const auto outside = profile.path / "outside";
    CHECK(fs::create_directory(outside));
    const auto marker = outside / "marker";
    write_file(marker, "not a receipt");
    const auto alias = fixture(8);
    std::error_code error;
    fs::create_hard_link(marker, receipt(profile, alias), error);
    if (!error) {
        CHECK(!save_ledger_entry(profile.path, alias).empty());
        CHECK(fs::file_size(marker) == 13);
        CHECK(load_ledger(profile.path).entries.size() == 2);
    }
    error.clear();
    const auto link = fixture(9);
    fs::create_symlink(marker, receipt(profile, link), error);
    if (!error) {
        CHECK(!save_ledger_entry(profile.path, link).empty());
        CHECK(fs::file_size(marker) == 13);
        CHECK(load_ledger(profile.path).entries.size() == 2);
    }

    Profile unsafe_profile;
    error.clear();
    fs::create_directory_symlink(outside, unsafe_profile.path / "loot-ledger", error);
    if (!error) {
        CHECK(!save_ledger_entry(unsafe_profile.path, record).empty());
        CHECK(!load_ledger(unsafe_profile.path).error.empty());
        CHECK(!fs::exists(outside / (record.key + ".dwl")));
    }
}

void capacity_and_concurrent_creates() {
    Profile profile;
    constexpr std::size_t concurrent = 8;
    std::array<std::thread, concurrent> threads;
    std::array<std::string, concurrent> errors;
    for (std::size_t index = 0; index < concurrent; ++index) {
        threads[index] = std::thread([&, index] {
            auto row = fixture(index + 1, 1764000000 + static_cast<std::int64_t>(index));
            errors[index] = save_ledger_entry(profile.path, row);
        });
    }
    for (auto& thread : threads) thread.join();
    for (const auto& error : errors) CHECK(error.empty());
    CHECK(load_ledger(profile.path).entries.size() == concurrent);

    for (std::size_t id = concurrent + 1; id <= max_ledger_entries + 5; ++id) {
        auto row = fixture(id, 1764000000 + static_cast<std::int64_t>(id - 1));
        if (id == max_ledger_entries + 5) row.recipient = "Trust";
        CHECK(save_ledger_entry(profile.path, row).empty());
    }
    const auto loaded = load_ledger(profile.path);
    CHECK(loaded.error.empty() && loaded.entries.size() == max_ledger_entries);
    CHECK(loaded.entries.front().round.id == max_ledger_entries + 5 && loaded.entries.front().recipient == "Trust");
    CHECK(loaded.entries.back().round.id == 6);
    CHECK(fs::exists(receipt(profile, fixture(1))));
    CHECK(std::distance(fs::directory_iterator(profile.path / "loot-ledger"), fs::directory_iterator{}) ==
          static_cast<std::ptrdiff_t>(max_ledger_entries + 5));
}

} // namespace

int main() {
    try {
        roundtrip();
        validation();
        damage_and_safe_paths();
        capacity_and_concurrent_creates();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "Dicewatch ledger tests passed.\n";
    return EXIT_SUCCESS;
}
