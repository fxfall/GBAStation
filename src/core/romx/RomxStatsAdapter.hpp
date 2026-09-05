#pragma once

#include "RomxGameEntryAdapter.hpp"

#include <cstdint>
#include <string>

namespace beiklive::romx
{
SyncResult readStats(const std::string& path, GameEntry& entry,
                     std::string* error = nullptr);
uint64_t parseTimestamp(const std::string& value);
}
