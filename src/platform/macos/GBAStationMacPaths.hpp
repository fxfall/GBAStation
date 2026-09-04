#pragma once

#if defined(__APPLE__) && !defined(__SWITCH__)

#include <cstdint>
#include <filesystem>
#include <mach-o/dyld.h>
#include <string>
#include <vector>

namespace beiklive::macos
{

// Resolve paths relative to the executable instead of the process working
// directory.  This is intentionally header-only so the macOS bundle path
// remains isolated from the upstream/common path implementation.
inline std::filesystem::path executablePath()
{
    uint32_t size = 0;
    // A null buffer is the documented size-query form and returns -1 on
    // macOS; only the resulting size determines whether the query worked.
    _NSGetExecutablePath(nullptr, &size);
    if (size == 0)
        return {};

    std::vector<char> buffer(size + 1, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
        return {};

    return std::filesystem::path(buffer.data());
}

// A bundled macOS executable lives at Contents/MacOS/<name>.  Return the
// matching Resources/cores directory only for that layout; non-bundled
// desktop builds fall back to the historical Application Support location.
inline std::filesystem::path bundledCorePath()
{
    const auto executable = executablePath();
    if (executable.empty())
        return {};

    const auto macosDirectory = executable.parent_path();
    const auto contentsDirectory = macosDirectory.parent_path();
    if (macosDirectory.filename() != "MacOS" ||
        contentsDirectory.filename() != "Contents")
        return {};

    return contentsDirectory / "Resources" / "cores";
}

} // namespace beiklive::macos

#endif // defined(__APPLE__) && !defined(__SWITCH__)
