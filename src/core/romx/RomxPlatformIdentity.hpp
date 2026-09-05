#pragma once

#include "RomxGameEntryAdapter.hpp"

#include <romx/romx.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace beiklive::romx
{
bool isPsp(const GameEntry& entry);
bool isThreeDs(const GameEntry& entry);
std::filesystem::path pspSaveRoot();
std::filesystem::path pspCheatRoot();

bool readRomxEntryBytes(const romx_reader_t* reader, uint32_t index,
                        uint64_t offset, uint64_t size,
                        std::vector<uint8_t>& output);
std::string readThreeDsTitleIdFromRomx(const std::string& path);
}
