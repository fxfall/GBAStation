#pragma once

#include <cstdint>
#include <string>

#include "third_party/mgba/src/platform/libretro/libretro.h"

namespace beiklive::romx
{
class LaunchSession;
}

namespace beiklive::romx_vfs
{

/**
 * Build a deterministic virtual path for a ROMX entrypoint.  The path keeps
 * the RIDX filename/extension visible to path-oriented cores without exposing
 * the physical container path to the core.
 */
std::string makeVirtualPath(const std::string& sourcePath,
                            const std::string& entrypointPath);

/**
 * Bind one ROMX launch session to the Libretro VFS bridge.  The session must
 * outlive all VFS handles and remains owned by the caller.
 */
bool activate(const void* owner, const std::string& virtualPath,
              beiklive::romx::LaunchSession* session);

/** Request deactivation after the core has been unloaded.  Open handles are
 * allowed to finish before the binding is released. */
void deactivate(const void* owner);

retro_vfs_interface* interfacePtr();

} // namespace beiklive::romx_vfs

