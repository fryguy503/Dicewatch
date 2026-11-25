#include "raid_summary.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace dicewatch;

#define CHECK(...) do { \
    if (!(__VA_ARGS__)) throw std::runtime_error("Line " + std::to_string(__LINE__) + ": " #__VA_ARGS__); \
} while (false)

LedgerEntry entry(std::string key, std::string session, std::int64_t ended, std::string label,
                  std::string recipient, std::vector<Roll> rolls = {}) {
    Round round;
    round.spec = {{200, 1000}, std::move(label), std::nullopt};
    round.rolls = std::move(rolls);
    return {std::move(key), std::move(session), ended, std::move(round), std::move(recipient)};
}

std::size_t utf16_units(std::string_view text) {
    std::size_t units = 0;
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i++]);
        if (lead < 0x80) {
            ++units;
            continue;
        }
        CHECK(lead >= 0xc2 && lead <= 0xf4);
        const auto extra = lead < 0xe0 ? 1u : lead < 0xf0 ? 2u : 3u;
        CHECK(extra <= text.size() - i);
        auto point = lead & (0x7fu >> (extra + 1));
        for (unsigned n = 0; n < extra; ++n) {
            const auto tail = static_cast<unsigned char>(text[i++]);
            CHECK((tail & 0xc0) == 0x80);
            point = (point << 6) | (tail & 0x3f);
        }
        CHECK(point >= (extra == 1 ? 0x80u : extra == 2 ? 0x800u : 0x10000u));
        CHECK(point <= 0x10ffff && (point < 0xd800 || point > 0xdfff));
        units += point > 0xffff ? 2 : 1;
    }
    return units;
}

void check_message(const std::string& message) {
    CHECK(message.compare(0, 4, "```\n") == 0);
    CHECK(message.find("```", 3) == message.size() - 3);
    CHECK(utf16_units(message) <= 2000);
}

void selection_and_results() {
    const std::vector<LedgerEntry> entries = {
        entry("four", "chosen", 32, "Last drop", "Cora"),
        entry("other", "another", 1, "Foreign raid loot", "Other"),
        entry("one", "chosen", 10, "First drop", "Mira", {{"Thalen", 912, {}}}),
        entry("two", "chosen", 15, "Second drop", "", {{"Vale", 990, {}}, {"Lark", 990, {}}}),
        entry("three", "chosen", 15, "Third drop", "Serenna"),
    };
    const auto pages = make_raid_summary("Vex Thal", entries, "chosen");
    CHECK(pages.size() == 1);
    const auto& message = pages.front();
    check_message(message);
    CHECK(message.find("Raid loot summary: Vex Thal\n") != std::string::npos);
    CHECK(message.find("1. First drop [200-1000]\n   Roll winner: Thalen (912)\n   Loot recipient: Mira\n") != std::string::npos);
    CHECK(message.find("2. Second drop [200-1000]\n   Roll tie: Vale, Lark (990)\n   Loot recipient: Pending assignment\n") != std::string::npos);
    CHECK(message.find("3. Third drop") < message.find("4. Last drop"));
    CHECK(message.find("Foreign raid loot") == std::string::npos);
    CHECK(message.find("Roll result: No eligible rolls\n   Loot recipient: Serenna") != std::string::npos);

    const auto empty = make_raid_summary("", entries, "missing");
    CHECK(empty.size() == 1);
    check_message(empty.front());
    CHECK(empty.front().find("Unnamed raid\nNo loot rounds recorded for this raid.") != std::string::npos);
}

void hostile_fields() {
    const auto pages = make_raid_summary("Raid @everyone\n```\n>>> injected", {
        entry("one", "s", 1, "@here https://example.invalid [fake](https://host.test)\n```",
              u8"<@&12345> @here www.evil.test https\uFF1A//evil\u3002test\u202E",
              {{"<@54321> ` discord.gg/invite \xc0\xaf", 888, {}}}),
    }, "s");
    CHECK(pages.size() == 1);
    const auto& message = pages.front();
    check_message(message);
    CHECK(message.find('@') == std::string::npos);
    CHECK(message.find("https://") == std::string::npos);
    CHECK(message.find("discord.gg") == std::string::npos);
    CHECK(message.find("www.evil.test") == std::string::npos);
    CHECK(message.find(u8"\u202E") == std::string::npos);
    CHECK(message.find("\n>>>") == std::string::npos);
    CHECK(message.find("(at)everyone") != std::string::npos);
    CHECK(message.find("example[.]invalid") != std::string::npos);
    CHECK(message.find("<(at)&12345>") != std::string::npos);
    CHECK(message.find("??") != std::string::npos);
}

void paging_and_large_ties() {
    const std::string emoji = u8"\U0001f48e";
    std::string long_text;
    for (std::size_t i = 0; i < 100; ++i) long_text += emoji;
    std::vector<LedgerEntry> entries;
    for (int i = 0; i < 200; ++i) {
        std::vector<Roll> tied;
        for (int n = 0; n < 6; ++n) tied.push_back({std::to_string(n) + long_text, 801, {}});
        entries.push_back(entry(std::to_string(i), "raid", i, "Drop " + std::to_string(i) + " " + long_text,
                                long_text, std::move(tied)));
    }

    const auto pages = make_raid_summary(long_text, entries, "raid");
    CHECK(pages.size() > 1);
    std::string all;
    for (std::size_t i = 0; i < pages.size(); ++i) {
        const auto& message = pages[i];
        check_message(message);
        CHECK(message.find(" (page " + std::to_string(i + 1) + "/" + std::to_string(pages.size()) + ")\n") != std::string::npos);
        CHECK(message.find(" + 2 more (801)") != std::string::npos);
        all += message;
    }

    std::size_t cursor = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto label = "\n" + std::to_string(i + 1) + ". Drop " + std::to_string(i) + " ";
        const auto at = all.find(label, cursor);
        CHECK(at != std::string::npos && all.find(label, at + 1) == std::string::npos);
        cursor = at + 1;
    }
}

} // namespace

int main() {
    try {
        selection_and_results();
        hostile_fields();
        paging_and_large_ties();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "Dicewatch raid summary tests passed.\n";
    return EXIT_SUCCESS;
}
