#include "announcements.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using namespace dicewatch;
using namespace std::chrono_literals;

#define CHECK(...) do { \
    if (!(__VA_ARGS__)) throw std::runtime_error("Line " + std::to_string(__LINE__) + ": " #__VA_ARGS__); \
} while (false)

void options() {
    const AnnouncementSettings defaults;
    CHECK(defaults.start_mode == AnnouncementMode::Off && defaults.award_mode == AnnouncementMode::Off);
    CHECK(defaults.start_channel == AnnouncementChannel::Raid && defaults.award_channel == AnnouncementChannel::Raid);
    for (const auto mode : {AnnouncementMode::Off, AnnouncementMode::Copy, AnnouncementMode::Send}) {
        CHECK(parse_announcement_mode(announcement_mode_name(mode)) == mode);
    }
    for (const auto channel : {AnnouncementChannel::Raid, AnnouncementChannel::Group,
                               AnnouncementChannel::Guild, AnnouncementChannel::Say}) {
        CHECK(parse_announcement_channel(announcement_channel_name(channel)) == channel);
    }
    CHECK(!parse_announcement_mode("SEND") && !parse_announcement_mode("paste"));
    CHECK(!parse_announcement_channel("/rsay") && !parse_announcement_channel("ooc"));
    CHECK(!chat_command(static_cast<AnnouncementChannel>(47), "Loot roll"));
}

void starting() {
    Tracker tracker;
    const auto start = Time{} + 10s;
    const auto a = tracker.start({{0, 500}, "Halo of Dawn", std::nullopt}, {}, start);
    CHECK(a.id);
    CHECK(opening_announcement(*tracker.find(a.id)).plain ==
        "Loot roll open for Halo of Dawn. Use /random 500. Highest roll wins.");
    CHECK(opening_announcement(*tracker.find(a.id)).with_item_link.empty());

    const auto raw_tag = std::string(1, '\x12') + "001BCD" + std::string(50, '0') + "Ancient Crown" + '\x12';
    auto linked = parse_itemroll("200-1000 " + raw_tag);
    CHECK(linked.spec && linked.spec->item);
    const auto b = tracker.start(*linked.spec, {DuplicatePolicy::First, WinnerOrder::Low, 1}, start);
    CHECK(b.id);
    const auto opening = opening_announcement(*tracker.find(b.id));
    CHECK(opening.plain == "Loot roll open for Ancient Crown. Use /random 200 1000. Lowest roll wins. Ends in 1 second.");
    CHECK(opening.with_item_link == "Loot roll open for " + raw_tag +
        ". Use /random 200 1000. Lowest roll wins. Ends in 1 second.");
    CHECK(chat_command(AnnouncementChannel::Raid, opening.with_item_link) == "/rsay " + opening.with_item_link);
    CHECK(tracker.set_timer(b.id, 30, start + 1ms));
    CHECK(opening_announcement(*tracker.find(b.id)).plain.find("Ends in 30 seconds.") != std::string::npos);
    CHECK(tracker.set_timer(b.id, 0, start + 2ms));
    CHECK(opening_announcement(*tracker.find(b.id)).plain.find("Ends in") == std::string::npos);

    Round invalid;
    invalid.spec.range = {300, 100};
    CHECK(opening_announcement(invalid).plain.empty());
}

void awarding() {
    Tracker tracker;
    const auto start = Time{} + 10s;
    const auto a = tracker.start({{0, 500}, "Halo of Dawn", std::nullopt}, {}, start);
    CHECK(a.id);
    CHECK(tracker.ingest("Trust", {0, 500}, 471, start + 1ms));
    CHECK(tracker.ingest("Bryn", {0, 500}, 412, start + 2ms));
    CHECK(tracker.ingest("Bryn", {0, 500}, 498, start + 3ms));
    CHECK(tracker.end(a.id, start + 4ms));

    LedgerEntry entry{{}, {}, 0, tracker.history().front(), "Mirael"};
    CHECK(award_announcement(entry).plain == "Awarded Halo of Dawn to Mirael. Roll winner Trust (471).");

    entry.round.rolls.push_back({"Rhian", 471, start + 3ms, RollStatus::Counted});
    CHECK(award_announcement(entry).plain == "Awarded Halo of Dawn to Mirael. Roll tie at 471 between Trust, Rhian.");
    entry.round.rolls.push_back({"Deryn", 471, start + 3ms, RollStatus::Counted});
    entry.round.rolls.push_back({"Wren", 471, start + 3ms, RollStatus::Counted});
    CHECK(award_announcement(entry).plain.find("between Trust, Rhian, Deryn and 1 more.") != std::string::npos);

    entry.round.rolls.clear();
    CHECK(award_announcement(entry).plain == "Awarded Halo of Dawn to Mirael. No eligible rolls.");
    entry.recipient.clear();
    CHECK(award_announcement(entry).plain.empty());
}

void text_and_dispatch() {
    CHECK(chat_command(AnnouncementChannel::Raid, "Roll 500") == "/rsay Roll 500");
    CHECK(chat_command(AnnouncementChannel::Group, "Roll 500") == "/gsay Roll 500");
    CHECK(chat_command(AnnouncementChannel::Guild, "Roll 500") == "/guildsay Roll 500");
    CHECK(chat_command(AnnouncementChannel::Say, "Roll 500") == "/say Roll 500");
    CHECK(!chat_command(AnnouncementChannel::Say, ""));
    CHECK(!chat_command(AnnouncementChannel::Say, " "));
    CHECK(!chat_command(AnnouncementChannel::Say, "/other"));
    CHECK(!chat_command(AnnouncementChannel::Say, "text\n/other"));
    CHECK(!chat_command(AnnouncementChannel::Say, "text\r/other"));
    CHECK(!chat_command(AnnouncementChannel::Say, "text; other"));
    CHECK(!chat_command(AnnouncementChannel::Say, "text ${TEST}"));
    CHECK(!chat_command(AnnouncementChannel::Say, "text %t"));
    CHECK(!chat_command(AnnouncementChannel::Say, "text \x12 tag \x12"));
    CHECK(!chat_command(AnnouncementChannel::Say, "text \xC2\x85 other"));
    CHECK(!chat_command(AnnouncementChannel::Say, "text \xE2\x80\xA8 other"));
    CHECK(!chat_command(AnnouncementChannel::Say, "text \xC0\xAF"));
    CHECK(!chat_command(AnnouncementChannel::Say, "text <meta>"));
    CHECK(!chat_command(AnnouncementChannel::Say, std::string("line\0break", 10)));
    const std::string maximum(max_announcement_bytes, 'X');
    const auto largest_command = chat_command(AnnouncementChannel::Guild, maximum);
    CHECK(largest_command && largest_command->size() <= 512);
    CHECK(!chat_command(AnnouncementChannel::Guild, maximum + 'X'));

    Round model;
    model.spec.range = {0, 500};
    model.spec.label = std::string("Name\r\n/other ${TEST}; <%t> ") + '\x12' + "tag" + '\x12' + "\xE2\x80\xAE" +
        " Cr\xC3\xA8me";
    const auto message = opening_announcement(model).plain;
    CHECK(message.find("/random 500") != std::string::npos);
    CHECK(message.find("/other") == std::string::npos && message.find("Cr\xC3\xA8me") != std::string::npos);
    CHECK(message.find("${") == std::string::npos && message.find("%t") == std::string::npos);
    CHECK(chat_command(AnnouncementChannel::Raid, message));

    LedgerEntry award{{}, {}, 0, model, "Tr\xC3\xBAst\xE2\x80\xA8/other"};
    award.round.rolls = {{"Rhian\xC2\x85%t", 251, {}, RollStatus::Counted}};
    CHECK(chat_command(AnnouncementChannel::Guild, award_announcement(award).plain));

    award.recipient = std::string(max_player_bytes, '%');
    award.round.spec.label = std::string(max_label_bytes, '/');
    award.round.rolls = {};
    for (const char ch : {'A', 'B', 'C', 'D'}) {
        award.round.rolls.push_back({std::string(1, ch) + std::string(max_player_bytes - 1, '%'),
                                    32000, {}, RollStatus::Counted});
    }
    const auto bounded = award_announcement(award).plain;
    CHECK(bounded.size() <= max_announcement_bytes);
    CHECK(bounded.find("Roll tie at 32000") != std::string::npos);
    CHECK(bounded.find("and 1 more.") != std::string::npos);
    CHECK(chat_command(AnnouncementChannel::Guild, bounded));
}

void tagged_chat_validation() {
    const auto metadata = std::string(1, item_link_delimiter) + "0000F4241" + std::string(60, '0') + "114ACFDB";
    const auto tag = metadata + "Crown of Dawn" + item_link_delimiter;
    const auto item = parse_item_link(tag);
    CHECK(item && item->id == 1000001);
    CHECK(chat_command(AnnouncementChannel::Say, "Awarded " + tag + " to Trust.") ==
          "/say Awarded " + tag + " to Trust.");
    CHECK(!chat_command(AnnouncementChannel::Say, "Awarded " + tag + ' ' + tag));
    CHECK(!chat_command(AnnouncementChannel::Say, "Awarded " + tag.substr(0, tag.size() - 1)));
    CHECK(!chat_command(AnnouncementChannel::Say, "Awarded " + tag + "\n/other"));
    CHECK(!chat_command(AnnouncementChannel::Say, "Awarded " + std::string(max_announcement_bytes, 'X') + tag));
    for (const char* label : {"Orb ${Target.Name}", "Orb %t", "Orb; /other", "<Orb>", "Orb\xE2\x80\x8B"}) {
        const auto unsafe_tag = metadata + label + item_link_delimiter;
        CHECK(parse_item_link(unsafe_tag));
        CHECK(!chat_command(AnnouncementChannel::Say, "Awarded " + unsafe_tag + " to Trust."));
    }
}

} // namespace

int main() {
    try {
        options();
        starting();
        awarding();
        text_and_dispatch();
        tagged_chat_validation();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "Dicewatch announcement tests passed.\n";
    return EXIT_SUCCESS;
}
