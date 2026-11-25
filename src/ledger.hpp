#pragma once

#include "rounds.hpp"

#include <filesystem>

namespace dicewatch {

inline constexpr std::size_t max_ledger_entries = 200;

struct LedgerEntry {
    std::string key;
    std::string session;
    std::int64_t ended_unix = 0;
    Round round;
    std::string recipient;
};

struct LedgerLoad {
    std::vector<LedgerEntry> entries;
    std::string error;
};

[[nodiscard]] bool valid_ledger_session(std::string_view session);
[[nodiscard]] bool valid_ledger_recipient(std::string_view recipient);
[[nodiscard]] LedgerLoad load_ledger(const std::filesystem::path& profile);
[[nodiscard]] std::string save_ledger_entry(const std::filesystem::path& profile, const LedgerEntry& entry);

} // namespace dicewatch
