# ROMX / libromx 重构工作汇报

## 任务目标

将 GBAStation 原先写在 `src/core/PackedRom.cpp` 中的 ROMX Footer、区域校验、metadata、cover 和 payload 解包逻辑迁移到独立 `libromx`，并在前端增加模块化接口，让内置核心优先使用 payload 映射或 VFS。

## 输入与参考

- 本地 `/Volumes/Repositories/retroarch-romx` 的 `romx_frontend` 与 `romx_ra_vfs` 实现。
- 独立 `/Volumes/Repositories/romx` 项目；GBAStation 当前接入 `third_party/libromx` 子模块提交 `ccf2ee6`。
- 当前 GBAStation 的 `PackedRom`、`LibretroLoader`、mGBA native、melonDS 加载路径。

## 已完成

1. 新增 `src/core/RomxFrontend.hpp/.cpp`：
   - 通过 `romx_reader_open_path`、`romx_reader_get_info`、`romx_reader_validate` 读取容器结构和校验信息。
   - 通过 libromx region API 读取 metadata、ROM Header 和 cover。
   - metadata 采用前端宽松提取：只读取需要的键，不在前端限制 `schema_version`。
   - 保留 GBA/GB/GBC、NES/FDS、SNES、NDS、3DS、Genesis Header 识别和标题回退。
   - 保留 1024 条按文件大小和修改时间的 Info 缓存，避免扫描目录时重复校验。

2. `src/core/PackedRom.cpp/.hpp` 已移除。上游本身已删除旧兼容层，当前业务入口全部指向 `RomxFrontend`，不再保留重复 API。

3. 新增 `src/game/retro/RomxVfs.hpp/.cpp`：
   - 为 libretro 核心提供正常文件回退和 `romx:/...` 虚拟路径。
   - ROMX 文件句柄使用 libromx reader 的 payload region read；可选 mapping 时直接从 guarded mapping 读取。
   - 对 ROMX 句柄限制为只读，提供 size/tell/seek/read/stat 等核心需要的接口。
   - 关闭游戏后延迟到最后一个 VFS 句柄关闭再释放映射。

4. `LibretroLoader`：
   - `need_fullpath=true` 的核心接收 `romx:/<name>.<format>`，不再先生成临时 ROM。
   - `need_fullpath=false` 的核心接收 `romx_reader_map_payload` 返回的 payload 指针和大小，映射保持到 `retro_unload_game`。
   - 当目标平台无法建立 payload mapping 时，才回退到 libromx 的缓存 payload 文件，保持内存型 libretro 核心的兼容性。
   - `GET_VFS_INTERFACE` 返回新的前端 VFS，普通文件仍可用 stdio。

5. 内置核心接入：
   - FCEUmm/Snes9x 调用方不再提前 `prepareRomForLaunch`，由 `LibretroLoader` 按核心 ABI 选择 VFS 或映射。
   - mGBA native 优先使用 libromx payload mapping + `VFileFromConstMemory`，映射保持到 mGBA unload；不支持映射的平台回退到 libromx 解包。
   - melonDS 优先使用 payload mapping 调用 `NDSCart::ParseROM(const u8*,...)`；由于 melonDS 当前 `ParseROM` 会在核心内部复制 ROM，仍保留提取回退。
   - Genesis custom core 当前仍使用其既有 path-only `load_rom` 接口，保留 libromx 解包回退；后续若要完全避免临时文件，需要为 Genesis core 增加 memory/VFS 入口。

## 验证

- CMake 已成功配置到 `build_linux_romx`，并构建了独立 `romx` 静态库。
- 受本机 AppleClang 旧 libc++ 警告被 Yoga `-Werror` 阻断，完整 GBAStation 链接未完成；这不是本次 ROMX 编译错误。
- 本次修改涉及的 `PackedRom`、RomxFrontend、RomxVfs、LibretroLoader、FCEUmm、Snes9x、mGBA native、Genesis source 均通过 `-fsyntax-only` 检查。
- melonDS 源文件在当前桌面配置不加入 `compile_commands`（上游默认关闭 native melonDS），因此保留上游配置检查；其 ROMX 入口仅使用 `RomxLaunchSession`。
- 本轮新增适配器、启动会话、数据库、页面和核心改动均通过 `-fsyntax-only`；`romx` 目标重新构建成功。
- `romx_payload_view_tests`、phase 1–8、C++ phase 8、冻结 fixture conformance 和 writer golden fixture 均通过。

## upstream v0.3.9 同步评估（2026-08-12）

- 已执行 `git fetch upstream --prune`，`upstream/main` 从 `4d346b04` 更新到 `7d52c747`（v0.3.9）。
- 已在本地完成 `upstream/main` 的合并工作树，冲突文件按上游 v0.3.9 版本重置后，再以薄适配调用接回 ROMX 模块；不推送 GitHub。
- 实际冲突文件共 5 个：`CMakeLists.txt`、`resources/changelog`、`src/main.cpp`、`src/ui/page/DataManagementPage.cpp`、`src/ui/page/StartPage.cpp`。
- 冲突主要集中在上游的扫描导入重写、PSP/3DS 元数据与封面提取、主页面启动入口；`RomxFrontend`、`RomxGameEntryAdapter`、`RomxLaunchSession`、`RomxVfs` 等新模块本身没有冲突。
- `src/core/PackedRom.cpp/.hpp` 已按上游删除，避免旧解析接口再次成为合并冲突点。
- `DataManagementPage` 和 `StartPage` 采用上游 v0.3.9 的扫描、PSP/3DS 元数据和外部 NRO 流程；ROMX 只通过 `RomxGameEntryAdapter` 与 `RomxLaunchSession` 接入。

## 当前边界与后续建议

- `libromx` 以 git submodule 放在 `third_party/libromx`，也可以通过 CMake `ROMX_SOURCE_DIR` 指向外部 checkout；Switch Actions checkout 已启用递归 submodule。
- FCEUmm、Genesis 等核心内部仍可能把逻辑 ROM 复制到自己的工作区；前端已经不再复制 ROMX 容器，也不再把 metadata/cover/footer 暴露给核心。
- 若要彻底实现 Genesis/melonDS 大 ROM 的零拷贝，需要继续改核心自身的 ROM 存储接口，而不是在 GBAStation 的 ROMX 解析层重复实现 Footer。

## 与 upstream 的改动量评估

- 本轮模块化改造相对之前的 ROMX 实现明显减少了核心解析层的直接改动：原 `PackedRom.cpp` 的约 1200 行 Footer、校验和区域逻辑已缩减为兼容 façade，实际解析集中到 `RomxFrontend`，VFS 集中到 `RomxVfs`。
- 但整个 `romx-support` 分支相对 `upstream/main` 仍保留早期提交中的 UI、数据库、平台识别、启动流程和构建环境改动；当前工作树与 upstream 的历史差异不能仅靠本轮重构消除。
- 后续新增 ROMX 字段、metadata 显示、封面处理或核心适配，应优先放进 `RomxFrontend`、`RomxVfs` 及新的 GameEntry 适配模块，让 `main.cpp`、页面和核心入口只保留薄接口，以降低后续 upstream 合并冲突。

## 本轮入口与启动会话重构

- 新增 `src/core/RomxGameEntryAdapter.hpp/.cpp`：统一 ROMX metadata、ROM Header 标题回退、平台/core、CRC32、开发商/日期/类型/地区、body SHA-256、metadata JSON、封面提取、NDS 内置图标和 3DS Title ID 到 `GameEntry` 的赋值。
- `src/main.cpp`、`src/ui/page/DataManagementPage.cpp`、`src/ui/page/StartPage.cpp` 和 `src/ui/page/FileListPage.cpp` 不再直接访问 ROMX `Info` 或调用区域解析；页面详情统一使用适配后的 `GameEntry`。
- 新增 `src/core/RomxLaunchSession.hpp/.cpp`：统一判断 ROMX、建立 payload mapping、取得 payload 大小/逻辑扩展名，以及 mapping 失败后的缓存解包回退。
- mGBA native、melonDS、Genesis、libretro mGBA 启动入口和 Switch 外部 NRO 路径均通过 `RomxLaunchSession` 处理；核心只接收 mapping、VFS 虚拟路径或已物化的标准 ROM 路径。
- `src/core/game_database.cpp` 新写入格式把 ROMX 专属字段放进 `romx` 对象（`developer`、`releaseDate`、`genre`、`region`、`bodySha256`、`metadata`）；读取仍兼容历史平铺字段和 `romxMetadata`，避免升级时丢失旧库数据。
- `src/core/Tools.cpp` 和 `src/game/mgba/GameRun.cpp` 保持上游入口；ROMX 核心路径由 `RomxLaunchSession`/`LibretroLoader` 接管，不再依赖旧 `PackedRom`。
