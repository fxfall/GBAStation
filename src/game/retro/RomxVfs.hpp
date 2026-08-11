#pragma once

#include <cstdint>
#include <string>

#include "third_party/mgba/src/platform/libretro/libretro.h"

struct romx_payload_mapping;
typedef struct romx_payload_mapping romx_payload_mapping_t;

namespace beiklive::romx_vfs
{

/** 成功时接管 mapping；传入空 mapping 时使用有界区域读取。 */
bool activate(const void* owner, const std::string& virtualPath,
              const std::string& sourcePath, std::uint64_t payloadSize,
              romx_payload_mapping_t* mapping);

/** 延迟释放当前 mapping，直到核心关闭所有 VFS 句柄。 */
void deactivate(const void* owner);

retro_vfs_interface* interfacePtr();

} // 命名空间 beiklive::romx_vfs
