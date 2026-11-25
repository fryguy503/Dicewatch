#pragma once

#include "ledger.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace dicewatch {

// Always returns at least one standalone Discord message.
[[nodiscard]] std::vector<std::string> make_raid_summary(
    std::string_view session_label, const std::vector<LedgerEntry>& entries, std::string_view session);

} // namespace dicewatch
