#include "rounds.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using namespace dicewatch;
using namespace std::chrono_literals;

#define CHECK(...) do { \
    if (!(__VA_ARGS__)) throw std::runtime_error("Line " + std::to_string(__LINE__) + ": " #__VA_ARGS__); \
} while (false)

const Time now = Time{} + 1h;

RoundSpec spec(Range range, std::string label = "Loot") {
    return {range, std::move(label), std::nullopt};
}

std::string item_link(bool wide, const std::string& name) {
    const std::string id = wide ? "000F4241" : "01BCD";
    const std::size_t body_size = wide ? 76 : 55;
    return std::string(1, '\x12') + '0' + id +
        std::string(body_size - id.size(), '0') + name + '\x12';
}

void commands() {
    const auto note = parse_itemroll(" 2500  Random  Note   ");
    CHECK(note.spec && note.spec->range == Range{0, 2500});
    CHECK(note.spec->label == "Random  Note" && !note.spec->item);

    const auto simple_link = item_link(false, "Greaves of the Vale");
    const auto item = parse_itemroll("500 " + simple_link);
    CHECK(item.spec && item.spec->range == Range{0, 500});
    CHECK(item.spec->label == "Greaves of the Vale");
    CHECK(item.spec->item && item.spec->item->id == 0x1BCD && item.spec->item->raw == simple_link);

    const auto wide_link = item_link(true, "Ring of Falling Stars");
    const auto wide = parse_itemroll("200-1000 " + wide_link);
    CHECK(wide.spec && wide.spec->range == Range{200, 1000});
    CHECK(wide.spec->item && wide.spec->item->id == 1000001 && wide.spec->item->raw == wide_link);

    CHECK(parse_itemroll("100 \"" + simple_link + "\"").spec->item);
    CHECK(parse_itemroll("100-100 Fixed point").spec->range == Range{100, 100});
    CHECK(parse_itemroll("32000 Maximum").spec);
    CHECK(parse_itemroll("500 [Plain bracketed note]").spec->label == "[Plain bracketed note]");
    CHECK(parse_itemroll("500 Cr\xc3\xa8me").spec);
    CHECK(parse_itemroll("500 Orb \xF0\x9F\x94\xAE").spec);
}

void invalid_commands() {
    for (const auto* invalid : {
        "", "500", "500  ", "-500 Low", "1--20 Bad", "800-100 Reversed",
        "32001 High", "0 Zero", "0-0 Two zeros", "+500 Plus", "1.5 Decimal",
        "777777777777777777777777 Oversize", "1-3-4 Bad", "500 A\nB",
        "500 A\rB", "500 A\tB", "500 A\x7f", "500 A\xc0\x80", "500 A\xed\xa0\x80",
        "500 A\xe2\x80\xae", "500 A\xc2\x9f", "500 A\xf4\x90\x80\x80",
    }) {
        const auto result = parse_itemroll(invalid);
        CHECK(!result.spec && !result.error.empty());
    }
    CHECK(!parse_itemroll("a" + std::string(700, 'a')).spec);
    CHECK(!parse_itemroll("500 " + std::string(max_label_bytes + 1, 'a')).spec);
    CHECK(parse_itemroll("500 " + std::string(max_label_bytes, 'a')).spec);
    CHECK(!parse_itemroll(std::string_view("500 A\0B", 7)).spec);
    CHECK(!parse_itemroll("500 " + item_link(false, "Ring").substr(0, 8)).spec);
    CHECK(!parse_itemroll("500 " + item_link(false, "Ring") + " suffix").spec);
    CHECK(!parse_itemroll("500 " + item_link(false, "Ring") + " " + item_link(true, "Ring")).spec);
    CHECK(!parse_itemroll("500 " + item_link(false, "No\nLine")).spec);

    auto invalid_hex = item_link(false, "Badge");
    invalid_hex[12] = 'Z';
    CHECK(!parse_itemroll("500 " + invalid_hex).spec);
    auto zero_id = item_link(false, "Badge");
    zero_id.replace(2, 5, "00000");
    CHECK(!parse_itemroll("500 " + zero_id).spec);
}

void concurrent_rounds() {
    Tracker tracker;
    const auto a = tracker.start(spec({0, 100}, "Amulet"), {}, now);
    const auto b = tracker.start(spec({0, 500}, "Band"), {}, now);
    const auto c = tracker.start(spec({200, 500}, "Ring"), {}, now);
    CHECK(a.id && b.id && c.id);
    CHECK(!tracker.start(spec({0, 500}, "Another band"), {}, now).id);
    CHECK(tracker.rounds().size() == 3);

    CHECK(tracker.ingest("Ashlar", {0, 100}, 70, now));
    CHECK(tracker.ingest("Ashlar", {0, 500}, 200, now));
    CHECK(tracker.ingest("Ashlar", {200, 500}, 250, now));
    CHECK(!tracker.ingest("Ashlar", {200, 500}, 100, now));
    CHECK(!tracker.ingest("Other", {1, 500}, 300, now));
    CHECK(!tracker.ingest("No\nName", {0, 500}, 300, now));
    CHECK(!tracker.ingest("Other", {0, 500}, 501, now));
    CHECK(!tracker.ingest("Other", {0, 500}, 200, now - 1ms));
    CHECK(tracker.find(c.id)->standings().front().value == 250);

    CHECK(tracker.end(b.id, now + 1s));
    CHECK(!tracker.end(b.id, now + 2s));
    CHECK(!tracker.ingest("Ashlar", {0, 500}, 500, now + 2s));
    CHECK(tracker.ingest("Another", {200, 500}, 275, now + 2s));
    CHECK(tracker.history().size() == 1);
    CHECK(tracker.history().front().end_reason == EndReason::Manual);

    const auto reopened = tracker.start(spec({0, 500}, "New band"), {}, now + 3s);
    CHECK(reopened.id != b.id && tracker.find(b.id) == nullptr);
    CHECK(tracker.find(reopened.id)->rolls.empty());
    CHECK(tracker.history().front().spec.label == "Band");
}

void duplicates_and_ties() {
    Tracker tracker;
    const auto high = tracker.start(spec({0, 100}), {}, now).id;
    CHECK(tracker.ingest("Mirael", {0, 100}, 96, now));
    CHECK(tracker.ingest("mIRAEL", {0, 100}, 100, now + 1ms));
    CHECK(tracker.ingest("Thalen", {0, 100}, 96, now + 2ms));
    CHECK(tracker.ingest("Orwyn", {0, 100}, 40, now + 3ms));
    CHECK(tracker.find(high)->rolls[1].status == RollStatus::Duplicate);
    const auto ranks = tracker.find(high)->standings();
    CHECK(ranks.size() == 3);
    CHECK(ranks[0].rank == 1 && ranks[0].player == "Mirael" && ranks[0].value == 96);
    CHECK(ranks[1].rank == 1 && ranks[1].value == 96 && ranks[2].rank == 3);

    const auto low = tracker.start(spec({0, 500}), {DuplicatePolicy::Best, WinnerOrder::Low, 0}, now).id;
    CHECK(tracker.ingest("Mirael", {0, 500}, 200, now));
    CHECK(tracker.ingest("Mirael", {0, 500}, 400, now + 1ms));
    CHECK(tracker.ingest("Mirael", {0, 500}, 50, now + 2ms));
    CHECK(tracker.ingest("Mirael", {0, 500}, 50, now + 3ms));
    CHECK(tracker.ingest("Thalen", {0, 500}, 100, now + 4ms));
    const auto& log = tracker.find(low)->rolls;
    CHECK(log[0].status == RollStatus::Replaced && log[1].status == RollStatus::Duplicate);
    CHECK(log[2].status == RollStatus::Counted && log[3].status == RollStatus::Duplicate);
    CHECK(tracker.find(low)->standings().front().value == 50);

    const auto last = tracker.start(spec({0, 1000}), {DuplicatePolicy::Last, WinnerOrder::High, 0}, now).id;
    CHECK(tracker.ingest("Mirael", {0, 1000}, 900, now));
    CHECK(tracker.ingest("Mirael", {0, 1000}, 10, now + 1ms));
    CHECK(tracker.find(last)->rolls[0].status == RollStatus::Replaced);
    CHECK(tracker.find(last)->standings().front().value == 10);
}

void timers() {
    Tracker tracker;
    const auto a = tracker.start(spec({0, 100}), {DuplicatePolicy::First, WinnerOrder::High, 2}, now).id;
    const auto b = tracker.start(spec({0, 500}), {DuplicatePolicy::First, WinnerOrder::High, 9}, now).id;
    CHECK(tracker.find(a)->remaining_seconds(now + 1100ms) == 1);
    CHECK(tracker.find(a)->remaining_seconds(now + 2s) == 0);
    CHECK(tracker.ingest("Early", {0, 100}, 100, now + 1999ms));
    CHECK(!tracker.ingest("Late", {0, 100}, 100, now + 2s));
    CHECK(tracker.find(a)->end_reason == EndReason::Timed);
    CHECK(tracker.find(a)->ended_at == now + 2s);
    CHECK(tracker.find(b)->open());
    CHECK(!tracker.end(a, now + 3s));
    CHECK(tracker.history().size() == 1);

    CHECK(tracker.set_timer(b, 0, now + 4s));
    tracker.tick(now + 9s);
    CHECK(tracker.find(b)->open() && !tracker.find(b)->remaining_seconds(now + 9s));
    CHECK(!tracker.set_timer(b, max_duration_seconds + 1, now + 9s));
    CHECK(tracker.set_timer(b, 2, now + 9s));
    CHECK(!tracker.end(b, now + 11s));
    CHECK(tracker.history().size() == 2);
    CHECK(tracker.history().front().id == b && tracker.history().front().ended_at == now + 11s);
}

void bounds() {
    Tracker tracker;
    for (std::size_t n = 1; n <= max_round_slots; ++n) {
        CHECK(tracker.start(spec({0, static_cast<int>(n)}), {}, now).id);
    }
    CHECK(!tracker.start(spec({0, 100}), {}, now).id);
    CHECK(tracker.end(tracker.rounds().front().id, now));

    auto next = tracker.start(spec({0, 100}, ""), {}, now);
    CHECK(next.id && tracker.find(next.id)->spec.label == "Loot roll");
    CHECK(tracker.rounds().size() == max_round_slots);
    for (std::size_t n = 0; n < max_rolls_per_round; ++n) {
        CHECK(tracker.ingest("Mirael", {0, 100}, 75, now));
    }
    CHECK(!tracker.ingest("Another", {0, 100}, 80, now));
    CHECK(tracker.find(next.id)->dropped_rolls == 1);
    CHECK(tracker.find(next.id)->rolls.size() == max_rolls_per_round);
    CHECK(tracker.end(next.id, now + 1s));

    for (std::size_t n = 0; n < max_history_rounds + 2; ++n) {
        const auto at = now + std::chrono::seconds(n + 2);
        next = tracker.start(spec({0, 100}), {}, at);
        CHECK(next.id && tracker.end(next.id, at));
    }
    CHECK(tracker.history().size() == max_history_rounds);
    CHECK(tracker.history().front().id == next.id);
}

} // namespace

int main() {
    try {
        commands();
        invalid_commands();
        concurrent_rounds();
        duplicates_and_ties();
        timers();
        bounds();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "Dicewatch round tests passed.\n";
    return EXIT_SUCCESS;
}
