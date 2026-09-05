#include "RomxSavePaths.hpp"

#include <utility>

namespace beiklive::romx
{
SavePathMapper makePspSaveOutputMapper(std::string& targetDirectory,
                                       std::string requestedDirectory)
{
    return [&targetDirectory,
            selectedSourceDirectory = std::move(requestedDirectory)](
               const std::filesystem::path& relative, uint32_t /*index*/,
               uint32_t /*count*/) mutable -> std::optional<std::filesystem::path> {
        auto component = relative.begin();
        if (component == relative.end())
            return std::nullopt;

        const std::string sourceDirectory = component->string();
        if (selectedSourceDirectory.empty())
            selectedSourceDirectory = sourceDirectory;
        if (sourceDirectory != selectedSourceDirectory)
            return std::nullopt;
        if (targetDirectory.empty())
            targetDirectory = sourceDirectory;

        std::filesystem::path mapped = targetDirectory;
        ++component;
        for (; component != relative.end(); ++component)
            mapped /= *component;
        return mapped;
    };
}
}
