#include "RomxStatsAdapter.hpp"

#include <romx/romx.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <sstream>

namespace beiklive::romx
{
namespace
{
void assignError(std::string* output, const std::string& message)
{
    if (output != nullptr)
        *output = message;
}

std::string errorText(const char* operation, const romx_error_t& error,
                      romx_result_t result)
{
    std::ostringstream stream;
    stream << operation << " failed (" << result << ")";
    if (error.message[0] != '\0')
        stream << ": " << error.message;
    return stream.str();
}

std::string timestampString(uint64_t unixSeconds)
{
    if (unixSeconds == 0)
        return {};
    const std::time_t value = static_cast<std::time_t>(unixSeconds);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &value) != 0)
        return {};
#else
    if (localtime_r(&value, &local) == nullptr)
        return {};
#endif
    char buffer[32]{};
    if (std::strftime(buffer, sizeof(buffer), "%y-%m-%d %H-%M-%S", &local) == 0)
        return {};
    return buffer;
}
}

uint64_t parseTimestamp(const std::string& value)
{
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (std::sscanf(value.c_str(), "%d-%d-%d %d-%d-%d",
                    &year, &month, &day, &hour, &minute, &second) != 6)
        return 0;
    if (year < 0 || year > 99 || month < 1 || month > 12 ||
        day < 1 || day > 31 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 59)
        return 0;
    std::tm local{};
    local.tm_year = (2000 + year) - 1900;
    local.tm_mon = month - 1;
    local.tm_mday = day;
    local.tm_hour = hour;
    local.tm_min = minute;
    local.tm_sec = second;
    local.tm_isdst = -1;
    const std::time_t result = std::mktime(&local);
    return result < 0 ? 0 : static_cast<uint64_t>(result);
}

SyncResult readStats(const std::string& path, GameEntry& entry,
                     std::string* error)
{
    romx_reader_t* reader = nullptr;
    romx_error_t err{};
    const romx_result_t openResult =
        romx_reader_open_path(path.c_str(), nullptr, &reader, &err);
    if (openResult != ROMX_OK)
    {
        assignError(error, errorText("romx_reader_open_path", err, openResult));
        return SyncResult::Failed;
    }
    romx_mutable_stats_t stats = ROMX_MUTABLE_STATS_INIT;
    romx_result_t result = romx_mutable_stats_read(reader, "default", &stats, &err);
    if (result != ROMX_OK)
        result = romx_mutable_stats_read(reader, "libretro", &stats, &err);
    if (result != ROMX_OK)
    {
        romx_reader_close(reader);
        if (result == ROMX_E_MUTABLE_ABSENT || result == ROMX_E_MUTABLE_ENTRY)
            return SyncResult::Skipped;
        assignError(error, errorText("romx_mutable_stats_read", err, result));
        return SyncResult::Failed;
    }
    if (stats.flags & ROMX_MUTABLE_STATS_HAS_PLAY_TIME)
        entry.playTime = static_cast<int>(std::min<uint64_t>(
            stats.play_time_seconds, INT32_MAX));
    if (stats.flags & ROMX_MUTABLE_STATS_HAS_LAUNCH_COUNT)
        entry.playCount = static_cast<int>(std::min<uint64_t>(
            stats.launch_count, INT32_MAX));
    if (stats.flags & ROMX_MUTABLE_STATS_HAS_FAVORITE)
        entry.favourite = stats.favorite != 0;
    if (stats.flags & ROMX_MUTABLE_STATS_HAS_LAST_PLAYED)
        entry.lastPlayed = timestampString(stats.last_played_unix_seconds);
    romx_reader_close(reader);
    return SyncResult::Success;
}
}
