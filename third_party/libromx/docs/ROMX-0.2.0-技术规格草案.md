# ROMX 0.2.0 技术规格草案

> 这是一份基于当前仓库源码、公开头文件、实现文档、测试和相邻标准仓库冻结夹具的审阅材料，不是新的规范文本，也不改变实现。
>
> 标记约定：`[规范]` 表示 `/Volumes/Repositories/romx/docs/ROMX-SPEC.md` 或其中文版本的明确要求；`[代码实现]` 表示当前 `/Volumes/Repositories/libromx` 的实现行为；`[测试证据]` 表示 `tests/` 测试或 `/Volumes/Repositories/romx/tests/fixtures/` 夹具；`[已决]` 表示曾经的发布门禁事项已经由冻结记录和实现/测试决议关闭。

> **决议记录说明：** 本文件按最终发布门禁状态维护。所有曾经的待确认事项已经由
> `ROMX-0.2.0-待确认事项已决.md` 关闭；固定的 wire 快照见
> `ROMX-0.2.0-来源冻结.md`。

## 0. 证据边界与版本

- `[规范]` ROMX 0.2.0 使用 footer wire version 2、RIDX version 1、metadata schema `0.2.0`；所有整数为无符号 little-endian，reserved/flags 必须为零。
- `[代码实现]` `include/romx/romx.h` 暴露 C99 ABI；`include/romx/romx.hpp` 仅提供异常和 RAII C++ wrapper；核心 C 代码不依赖 GUI、模拟器、RetroArch 或 C++ STL。
- `[测试证据]` `tests/test_v2.c`、`tests/test_writer_mutable.c` 的输出都标注 ROMX 0.2.0；三份标准夹具的 manifest 明确记录 footer、region、RIDX、checksum 和验证状态。
- `[已决][规范][测试证据]` 规范、schema 和 fixtures 由相邻 `/Volumes/Repositories/romx` 提供；ROMX 0.2.0 的来源 commit、文档/schema hash 及 fixture README hash 已固定在 `ROMX-0.2.0-来源冻结.md` 与 `ROMX-0.2.0-PROVENANCE.md`，并由 CMake 安装发布门禁文档。游戏字节不复制进 libromx。

## 1. 范围、目标与非目标

- `[规范]` ROMX 是一个以原始 payload 为 offset 0 的容器；标准化 payload、RIDX、可选 metadata/PNG cover、可选固定容量 mutable region 和 128 字节 footer。
- `[代码实现]` reader 可读取、校验、按 entry positional read、映射 entrypoint、暴露 VFS；writer 可流式写入多个输入、生成 RIDX、metadata、cover、mutable capacity、immutable SHA-256、footer，并通过临时文件发布。
- `[代码实现]` extraction 提取 entrypoint 原始字节，支持临时文件、SHA-256 自校验、durable/replace 选项；cover 仅复制/验证，不强制图片转换库。
- `[规范]` 原始 ROM payload 逐字节保持不变；metadata 的 `crc32` 是原始 ROM/entrypoint 的外部身份值，不是整个 ROMX 文件；footer immutable SHA-256 覆盖 immutable 区域而不覆盖 mutable/footer。
- `[规范]` 不规定模拟器启动、前端目录、LPL、云同步、自动恢复/同步、payload 压缩/加密/补丁/字节交换和 mutable 自动扩容。
- `[代码实现]` probe 是可选的 payload 识别/metadata/cover 生成辅助，不改变 ROMX 容器边界；未知格式可返回 `ROMX_E_UNSUPPORTED`。
- `[已决][规范][代码实现]` salvage 明确不属于 0.2.0：普通 reader 在 footer 缺失/无效时 fail-closed，C ABI 不提供 salvage handle；独立 consumer 恢复工具必须显式标记且不得写回 mutable。

## 2. 顶层物理布局

`[规范]` 文件从偏移 0 开始依次为：payload → RIDX → metadata（可选）→ cover（可选）→ immutable zero padding（可选）→ mutable region（可选）→ 最后 128 字节 footer。

`[代码实现]` `src/footer.c` 解析 footer 后，`src/ridx.c` 推导所有 region；payload 必须从 0 开始，RIDX 从 `payload_size` 开始；有 mutable 时其起点为 `footer_offset - mutable_capacity`，否则 footer 前即 immutable end。

| 区域 | 起点/长度 | 约束与校验 |
|---|---|---|
| Payload | `[规范]` offset=0，size=`footer.payload_size`，大于 0 | `[代码实现]` entry ranges 必须落在 payload 内；无索引字节只能是零；entrypoint 必须 offset 0 |
| RIDX | `[规范]` offset=`payload_size`，size=`64 + entry_count*512` | `[代码实现]` 头、长度、index CRC、entry flags/path/range、重叠和 zero gap 均校验 |
| Metadata | `[规范]` 紧随 RIDX，可选，footer size 为 0 表示 absent | `[代码实现]` 读取时受上限约束，并严格 UTF-8/JSON/schema 校验 |
| Cover | `[规范]` 紧随 metadata 或 RIDX，可选 PNG | `[代码实现]` `src/png.c` 校验 PNG 结构、chunk CRC、大小和维度 |
| Immutable padding | `[规范]` 只允许在 cover 后至 mutable 起点的零字节 | `[代码实现]` 需要 4096 对齐；非零 padding 使结构失败 |
| Mutable region | `[规范]` 固定容量，起点 4096 对齐，容量为 4096 倍且至少 12288 | `[代码实现]` mutable header/目录坏时标记 unavailable，不影响 immutable 解析 |
| Footer | `[规范]` 文件最后 128 字节 | `[代码实现]` footer CRC、magic/version/range/hash/reserved 校验 |

`[测试证据]` `minimal-single` 的 payload=24592、RIDX=576、footer offset=25168；`single-complete` 的 metadata offset=25168、cover offset=25236、mutable offset=28672、mutable size=12288、footer offset=40960；manifest 与 `tests/test_v2.c` 一致。

## 3. Footer（128 字节）

`[规范]` Footer 完整固定 128 字节；整数均 little-endian；footer CRC 计算时把自身字段视为零。

| Offset | Size | 类型 | 字段/取值 | 约束与校验 |
|---:|---:|---|---|---|
| 0x00 | 4 | bytes/ASCII | `magic="ROMX"` | `[规范]` 必须精确匹配；`[代码实现]` 否则 `ROMX_E_INVALID_FOOTER` |
| 0x04 | 4 | uint32 LE | `wire_version=2` | `[规范]` 仅接受 2；`[代码实现]` `ROMX_FORMAT_VERSION` |
| 0x08 | 8 | uint64 LE | `payload_size` | `[规范]` >0，RIDX 从此开始；需无溢出落在 footer 前 |
| 0x10 | 8 | uint64 LE | `metadata_size` | 0=absent；非零时紧随 RIDX |
| 0x18 | 8 | uint64 LE | `cover_size` | 0=absent；非零时紧随 metadata/RIDX |
| 0x20 | 8 | uint64 LE | `mutable_capacity` | 0=absent；非零时 ≥12288、4096 倍，且 mutable 起点不越界 |
| 0x28 | 2 | uint16 LE | `platform_id` | `[规范]` 0=unspecified，0x0001.. 标准注册，0x8000..0xFFFE 私有，0xFFFF 禁止；`[代码实现]` 仅拒绝 0xFFFF，未知值返回 NULL 名称 |
| 0x2A | 2 | uint16 LE | `launch_format_id` | `[规范]` 0=unspecified，1 RAW_SINGLE_FILE，2 CUE，3 GDI，4 M3U，5 CCD，6 MDS，7 TOC，8 DIRECTORY，9 ROMSET，0xA SPLIT_FILE_SET；0xFFFF 禁止 |
| 0x2C | 4 | uint32 LE | `immutable_hash_algorithm` | `[规范]` 0=NONE，1=SHA256；其他值结构无效 |
| 0x30 | 32 | bytes | `immutable_sha256` | `[规范]` NONE 时全零，SHA256 时覆盖 immutable `[0, mutable_offset)` 或 `[0, footer_offset)`；`[代码实现]` 可选验证 |
| 0x50 | 4 | uint32 LE | `footer_crc32` | `[规范]` ISO-HDLC CRC32，计算 128 字节且 0x50..0x53 置零；`[代码实现]` 失败即 footer 无效 |
| 0x54 | 44 | bytes | reserved | `[规范]` 必须全零；`[代码实现]` 非零拒绝 |

`[代码实现]` `src/footer.c` 还检查 footer/file 大小、mutable 起点、payload index 最小长度、hash algorithm/sha 一致性及 0xFFFF；`src/writer.c` 以同一布局生成 footer。`[已决][代码实现]` `src/registry.c` 与公共 ABI 通过 `romx_platform_status`、`romx_launch_format_status`、`romx_file_format_status` 明确返回 KNOWN、UNSPECIFIED/UNKNOWN、PRIVATE 或 PROHIBITED；未知非零 ID 保留数值可读但按 unsupported 处理。

### Footer registry（取值域）

`[规范]` platform registry：`0x0000` unspecified；`0x0001` GB；`0x0002` GBC；`0x0003` GBA；`0x0004` NES；`0x0005` SNES；`0x0006` N64；`0x0007` NDS；`0x0008` N3DS；`0x0010` Master System；`0x0011` Game Gear；`0x0012` Mega Drive；`0x0013` 32X；`0x0014` Sega CD；`0x0015` Saturn；`0x0016` Dreamcast；`0x0020` PC Engine；`0x0021` PC Engine CD；`0x0030` PlayStation；`0x0031` PS2；`0x0032` PSP；`0x0040` GameCube；`0x0041` Wii；`0x0050` Arcade；`0x0060` ScummVM；`0x0061` DOS；`0x0062` Amiga。`

`[规范]` launch format registry：`0x0000` unspecified；`0x0001` RAW_SINGLE_FILE；`0x0002` CUE；`0x0003` GDI；`0x0004` M3U；`0x0005` CCD；`0x0006` MDS；`0x0007` TOC；`0x0008` DIRECTORY；`0x0009` ROMSET；`0x000A` SPLIT_FILE_SET。`[代码实现]` `src/registry.c` 为上述已知值返回名称，未知值返回 NULL。

`[规范][代码实现]` 0x0001..0x7FFF 为标准注册，0x8000..0xFFFE 为私有/实验注册且必须共享定义，0xFFFF 禁止；RIDX `format_id=0` 仅可用于非 entrypoint 文件。未知值不臆测名称，而通过 registry status 明确分类。

## 4. Payload 与 RIDX

### 4.1 RIDX header（64 字节）

| Offset | Size | 类型 | 字段 | 约束与校验 |
|---:|---:|---|---|---|
| 0x00 | 4 | bytes | `magic="RIDX"` | `[规范]` 精确匹配 |
| 0x04 | 2 | uint16 LE | `index_version=1` | `[代码实现]` 仅接受 1 |
| 0x06 | 2 | uint16 LE | `header_size=64` | 固定 |
| 0x08 | 4 | uint32 LE | `entry_count` | `[规范]` ≥1；总大小不得溢出 |
| 0x0C | 4 | uint32 LE | `entry_size=512` | 固定 |
| 0x10 | 4 | uint32 LE | `flags=0` | 非零 `ROMX_E_INVALID_FLAGS`/index error |
| 0x14 | 4 | uint32 LE | `index_crc32` | 完整 index，字段自身置零；ISO-HDLC |
| 0x18 | 40 | bytes | reserved | 全零 |

### 4.2 RIDX entry（512 字节）

| Offset | Size | 类型 | 字段 | 约束与校验 |
|---:|---:|---|---|---|
| 0x00 | 4 | uint32 LE | flags | bit0 ENTRYPOINT，bit1 HAS_CRC32，其余保留位必须零 |
| 0x04 | 2 | uint16 LE | format_id | entrypoint 非零；0xFFFF 禁止；未知非零可报告 unsupported |
| 0x06 | 2 | uint16 LE | path_size | 1..480；不是 NUL 结尾字段 |
| 0x08 | 8 | uint64 LE | data_offset | 相对 payload 起点；entrypoint 必须为 0 |
| 0x10 | 8 | uint64 LE | data_size | >0；`offset+size` 不越过 payload |
| 0x18 | 4 | uint32 LE | crc32 | HAS_CRC32 时为数据精确 CRC；未设置时必须为 0 |
| 0x1C | 4 | uint32 LE | reserved | 必须为 0 |
| 0x20 | 480 | bytes | virtual path | 前 `path_size` 为严格 UTF-8/NFC 相对路径，余下必须为 0 |

`[规范]` 恰好一个 ENTRYPOINT；entrypoint data_size>0、format_id 非零；单文件是 `entry_count=1` 且 entry 覆盖整个 payload、无 padding，多文件由 entry_count>1 推导。entry ranges 不得重叠；未索引 gaps 必须全零。

`[代码实现]` `src/ridx.c` 验证 header/index CRC、entry flags/reserved/path padding、entry bounds、恰好一个 entrypoint、排序后的重叠、zero gaps、metadata/cover/mutable 的连续边界；`src/writer.c` 总是把 entrypoint 先写到 payload offset 0，再按输入顺序写其他 entry。

`[规范][代码实现]` virtual path 使用 `/` 分隔、相对、无 NUL/backslash/空组件`.`/`..`、无首尾 slash；ROMX 0.2.0 明确保留非 ASCII UTF-8 字节，只在冲突检查时折叠 ASCII `A`–`Z`。entrypoint 的 descriptor 路径只能引用规范化相对路径；完整 Unicode NFC/full case-fold 属于未来 profile，不是本版本隐式依赖。

`[测试证据]` `multi-cue.manifest.json` 有 3 entries：CUE entrypoint offset 0 size 135，两个 BIN 连续位于 135 和 2695，三者均带 CRC32；`tests/test_v2.c` 验证 entry lookup、VFS、payload IO、map、entry CRC。

## 5. Metadata

- `[规范]` metadata 是可选、严格 UTF-8、无 BOM、RFC 8259 JSON；所有深度对象 key 唯一；schema version 固定 `0.2.0`。
- `[代码实现]` `src/metadata.c` 先验证 UTF-8/BOM/JSON/重复 key，再按头文件中同等约束验证 schema；读取受 `max_metadata_size` 限制。
- `[规范][测试证据]` 必填字段：`schema_version`（const `0.2.0`）、`name`（1..512）。可选字段及边界来自 `schema/romx-metadata.schema.json`：`serial/origin/category`≤128，`developer/publisher/franchise/language/enhancement_hw`≤256，`media`≤64，`description`≤32768，`genre`≤32×64，`region`≤32×32，`users` 1..255，`coop/rumble/analog` bool，`release_date` YYYY / YYYY-MM / YYYY-MM-DD，`dump_status` 枚举，`crc32`/`origin_crc32` 小写 8 hex，`cover` 为 image/png 且 width/height 1..8192。
- `[规范]` metadata 不保存 payload/mutable offset、主机绝对路径、启动路径、外部 cover path、platform/launch 声明；footer/RIDX 才是物理布局、平台、启动格式和 entry identity 的权威来源，metadata `crc32` 仅为原始 ROM lookup identity。
- `[代码实现]` `romx_metadata_get_string/get_crc32/get_value_json/copy_json` 返回 borrowed/复制结果；缺失字段或类型不符返回 metadata schema error；`get_crc32` 读取后兼容大写 hex，但写入/规范要求小写。
- `[测试证据]` `single-complete` metadata size=68；`tests/test_v2.c` 读取 name；`tests/test_writer_mutable.c` 写入最小 schema 并验证；schema/validator 会拒绝未知字段和重复 key。
- `[已决][规范][代码实现][测试证据]` schema 仍由相邻标准仓库维护，但其 SHA-256、来源 commit 和 fixture README hash 已记录在 `ROMX-0.2.0-PROVENANCE.md`/`ROMX-0.2.0-来源冻结.md`，CMake 将两份 provenance/决议文档安装为发布资产。

## 6. Cover

- `[规范]` cover 是单个 PNG；IHDR 必须首个且唯一，IDAT 连续，IEND 必须是零长度且最后一个 chunk；chunk 边界、CRC、合法 color/depth、PLTE 规则和未知 critical chunk 均需校验；不能把尾随字节当 cover。
- `[代码实现]` `src/png.c` 流式读取 PNG，受 `max_cover_size`（默认 32 MiB）和 `max_cover_dimension`（默认 8192）限制，输出 width/height；`romx_validate_cover_io` 的错误不影响 payload 访问，除非调用者要求 cover 严格验证。
- `[规范]` cover invalid 时可报告/忽略，reader 仍可读取结构有效的 payload/RIDX；不能把无效 cover 当作容器结构有效的依据。
- `[测试证据]` `single-complete` 使用 70 字节单像素 PNG，`tests/test_v2.c` 验证 cover status；writer test 通过 memory/probe cover 路径；标准 fixtures 的 manifest 记录 cover size/dimension。
- `[规范][代码实现]` libromx 只 extract/validate/copy 原始 PNG；颜色转换、缩放和显示策略明确属于 consumer/image adapter，不属于 ROMX 0.2.0 wire/API 合约。

## 7. Mutable region

### 7.1 Header（4096 字节）

| Offset | Size | 类型 | 字段 | 约束与校验 |
|---:|---:|---|---|---|
| 0x00 | 4 | bytes | `magic="RMUT"` | 精确匹配 |
| 0x04 | 2 | uint16 LE | version=1 | 固定 |
| 0x06 | 2 | uint16 LE | header_size=4096 | 固定 |
| 0x08 | 4 | uint32 LE | entry_size=512 | 固定 |
| 0x0C | 4 | uint32 LE | entry_capacity | ≥8，且为 8 的倍数 |
| 0x10 | 8 | uint64 LE | directory_offset=4096 | 固定，相对 mutable 起点 |
| 0x18 | 8 | uint64 LE | directory_size | `entry_capacity*512` |
| 0x20 | 8 | uint64 LE | data_area_offset | `4096+directory_size` 且 4096 对齐 |
| 0x28 | 8 | uint64 LE | data_area_size | `mutable_capacity-data_area_offset`，>0 |
| 0x30 | 4 | uint32 LE | flags=0 | 非零无效 |
| 0x34 | 4 | uint32 LE | header_crc32 | 完整 4096 字节，字段自身置零 |
| 0x38 | 4040 | bytes | reserved | 全零 |

### 7.2 Directory entry（512 字节）

| Offset | Size | 类型 | 字段 | 约束与校验 |
|---:|---:|---|---|---|
| 0x00 | 4 | bytes | `magic="MENT"` | 空 slot 为全零；非空须匹配 |
| 0x04 | 2 | uint16 LE | state | ACTIVE=1、WRITING=2、DELETING=3；0 仅全零 slot |
| 0x06 | 2 | uint16 LE | namespace | SAVE=1、CHEAT=2、STATS=3、PRIVATE=4 |
| 0x08 | 4 | uint32 LE | flags=0 | 非零无效 |
| 0x0C | 4 | uint32 LE | key_size | 1..448 |
| 0x10 | 8 | uint64 LE | data_offset | 相对 mutable 起点，64 对齐，落在 data area |
| 0x18 | 8 | uint64 LE | data_capacity | >0，extent 不越界 |
| 0x20 | 8 | uint64 LE | data_size | ≤capacity；size=0 时 data CRC=0 |
| 0x28 | 8 | uint64 LE | generation | 从 1 开始，每次 replacement 递增 |
| 0x30 | 8 | uint64 LE | modified_unix_seconds | UTC 秒或 0 |
| 0x38 | 4 | uint32 LE | data_crc32 | 精确 data bytes 的 CRC32 |
| 0x3C | 4 | uint32 LE | entry_crc32 | 完整 entry，字段自身置零 |
| 0x40 | 448 | bytes | key | key_size 字节后全零；UTF-8 相对 key |

`[规范]` `(namespace,key)` 唯一（case-fold）；所有 ACTIVE/WRITING/DELETING extent 不重叠；无空间不得改变 immutable；SAVE/CHEAT/STATS/PRIVATE 是 namespace，PRIVATE key 必须带 producer identifier `/` 前缀。

`[代码实现]` `src/mutable.c` 先验证 header，再逐 slot 校验 key/extent/entry CRC；坏 slot 被标记不可用并把状态置 DEGRADED，坏 header 置 INVALID；ACTIVE object 在暴露前重新计算 data CRC；`src/mutable_write.c` 不 relocation/growth/compaction/repack，replacement 复用旧 extent。

`[规范][代码实现]` key 检查有效 UTF-8、首尾 slash、`.`/`..`、backslash、PRIVATE producer 分隔符；0.2.0 的冲突检查固定为 ASCII fold，非 ASCII 字节保持原样，完整 Unicode normalization/folding 不属于本版本。

## 8. Mutable 提交协议

`[规范][代码实现]` 单对象写入顺序：

1. 在目标 slot 写入带 entry CRC 的 `WRITING` entry，并 durable flush。
2. 按 chunk 写入 data extent，计算 data CRC，并 durable flush。
3. 写入带 entry/data CRC 的 `ACTIVE` entry，并 durable flush；此写入是 commit point。

`[规范][代码实现]` 删除顺序：写入 `DELETING` entry 并 flush，再把 slot 清零并 flush。中断后 reader 只暴露 CRC 正确的 ACTIVE；WRITING/DELETING 不暴露但保留 extent。断电最多使受影响 object unavailable，immutable payload/RIDX/footer 不被 mutable API 修改。

`[代码实现]` `src/mutable_write.c` 对 POSIX 使用 `fcntl(F_SETLKW)` 排它锁、`pwrite`/`fsync`，Windows 使用 LockFileEx/FlushFileBuffers；reader 本身不自动恢复、同步、写回或删除临时对象。

`[测试证据]` `tests/test_writer_mutable.c` 验证 generation 1→2、容量复用、文件大小不变、delete 后 object count=0；`tests/test_v2.c` 篡改 mutable data 得到 `ROMX_E_MUTABLE_DATA_CRC`，篡改 mutable header 只使 mutable INVALID，entrypoint 仍可读。

`[已决][规范][测试证据]` 断电协议已冻结为 WRITING → data → ACTIVE 与 DELETING → clear；`tests/test_v2.c` 覆盖结构有效的 WRITING 中间态隔离，`tests/test_writer_mutable.c` 覆盖损坏/中间状态。真实断电运行是平台发布验收，不增加 wire 规则。

## 9. SAVE/CHEAT bundle

### 9.1 SAVE 存档槽模型

- `[规范]` `SAVE` 是零个或多个逻辑存档槽的集合；每个 `ACTIVE` SAVE mutable object 独立提交，一个 object 可以投影多个槽。object key 是稳定的 consumer 标签，不是主机路径；槽顺序不由容器编码。
- `[规范]` 多文件槽使用一个无压缩 `RMBL` bundle；RMBL `entry_count` 统计整个 bundle 的文件数，不统计槽数。替换/删除一个槽时，consumer 必须保留未选中 entry 并原子替换同一 object。
- `[代码实现]` `src/mutable_bundle.c` 从 reader footer 的 `platform_id`/`launch_format_id` 选择投影：PSP 使用目录型 profile，其余（含 UNSPECIFIED）使用每文件一个槽；新 `romx_mutable_bundle_get_save_slot*` API 返回槽及其成员 entry。
- `[规范][代码实现]` PSP 只有含有效 `PARAM.SFO` 身份的目录才成槽：SFO 需有有效 `DISC_ID`，或有与目录 basename 匹配的 `SAVEDATA_DIRECTORY`；该目录下全部文件归入同一槽。目录层级本身不能创建槽。
- `[代码实现]` `romx_mutable_psp_savedata_inspect_sfo` 只解析 caller 提供的 SFO bytes，不扫描目录、不选择主机目标；实现校验 `PSF` header、bounded records、DISC_ID/SAVEDATA_DIRECTORY 字符集与可选 title。
- `[规范][代码实现]` 其他已注册平台每个 bundle 文件是一个槽，槽 key 为完整规范化相对 path；bundle 中出现目录不会隐式分组。
- `[已决][规范][代码实现][测试证据]` 未知非零与 `UNSPECIFIED` platform 均使用保守的一文件一槽；只有显式注册 profile 才能启用目录分组。PSP 每个有效 `PARAM.SFO` root 都是候选，文件归入最长有效 root，内层 root 拥有自己的子树。

#### 9.1.1 libromx 槽投影输出结构（内存 ABI，非 wire 字段）

`[代码实现]` 下列结构由 `include/romx/romx.h` 定义，调用方先把 `struct_size` 设为 `sizeof`；它们不写入 ROMX footer、RIDX 或 RMBL。

| 结构/字段 | 类型与含义 | 约束/生命周期 |
|---|---|---|
| `romx_mutable_save_slot_info_t.struct_size` | uint32，结构版本/输入大小 | 必须不小于当前结构；输出时重置为当前大小 |
| `index` / `entry_count` | uint32，槽索引及成员文件数 | 索引从 0 开始；`entry_count` 只统计该槽成员 |
| `key` / `key_size` | NUL 结尾 UTF-8 槽标签及字节数 | bundle-relative/stable label，不是主机路径；复制到调用方缓冲区 |
| `display_name` / `display_name_size` | NUL 结尾显示名称及字节数 | PSP 优先使用 SFO title，否则目录名；普通槽使用 path basename |
| `data_size` | uint64，槽成员文件数据总字节数 | 由投影累加，不能越过 bundle 数据；复制值 |
| `is_directory` | uint32，1 表示 PSP 目录槽，0 表示 per-file 槽 | 当前仅为投影提示，不改变 RMBL wire |
| `romx_mutable_psp_savedata_info_t.flags` | uint32 身份位图 | `HAS_DISC_ID=1`、`HAS_DIRECTORY=2`、`HAS_TITLE=4` |
| `disc_id` / `savedata_directory` / `title` | NUL 结尾解析出的 SFO 字符串 | 最大分别 64、1024、1024 字节；输出结构由调用方拥有 |

`[已决][代码实现]` 槽投影输出结构是内存 ABI，不进入 wire；调用方必须初始化 `struct_size`，实现按当前大小返回，新增字段通过结构大小门控，wire 兼容性不受影响。

### 9.2 RMBL header（64 字节）

| Offset | Size | 类型 | 字段 | 约束与校验 |
|---:|---:|---|---|---|
| 0x00 | 4 | bytes | `magic="RMBL"` | 精确匹配 |
| 0x04 | 2 | uint16 LE | version=1 | 固定 |
| 0x06 | 2 | uint16 LE | header_size=64 | 固定 |
| 0x08 | 2 | uint16 LE | namespace | 外层只能 SAVE/CHEAT |
| 0x0A | 2 | uint16 LE | flags=0 | 非零无效 |
| 0x0C | 4 | uint32 LE | entry_size=64 | 固定 |
| 0x10 | 4 | uint32 LE | entry_count | 受配置/标准上限 4096 |
| 0x14 | 4 | uint32 LE | reserved | 0 |
| 0x18 | 8 | uint64 LE | directory_offset=64 | 固定 |
| 0x20 | 8 | uint64 LE | path_table_offset | `64+entry_count*64` |
| 0x28 | 8 | uint64 LE | data_offset | path table 末尾向上 64 对齐 |
| 0x30 | 8 | uint64 LE | bundle_size | 必须等于外层 object data_size |
| 0x38 | 4 | uint32 LE | header_crc32 | 完整 64 字节，字段自身置零 |
| 0x3C | 4 | bytes | reserved | 全零 |

### 9.3 Bundle entry（64 字节）

| Offset | Size | 类型 | 字段 | 约束与校验 |
|---:|---:|---|---|---|
| 0x00 | 8 | uint64 LE | path_offset | 绝对 bundle offset，路径表 packed |
| 0x08 | 4 | uint32 LE | path_size | 1..1024 |
| 0x0C | 4 | uint32 LE | flags=0 | 非零无效 |
| 0x10 | 8 | uint64 LE | data_offset | 绝对 bundle offset，64 对齐，按目录顺序连续 |
| 0x18 | 8 | uint64 LE | data_size | 不越过 bundle |
| 0x20 | 4 | uint32 LE | data_crc32 | 文件数据 CRC32 |
| 0x24 | 28 | bytes | reserved | 全零 |

`[规范]` path table 无缝拼接，路径按 unsigned UTF-8 bytes 排序且不能 byte/ASCII-fold 冲突；padding 必须为零；bundle 可为空（只有 64 字节 header）；禁止 symlink、绝对路径、`.`/`..`、backslash、路径穿越和本地路径冲突。

`[代码实现]` `src/mutable_bundle.c` 写入前用 `lstat`/Windows reparse 检查 regular file，计算文件 CRC，排序、构造 path/data table；读取时先完整验证 header、目录、路径、padding、每个 file CRC，再暴露 entry；写入通过普通 mutable object 的 WRITING→ACTIVE 协议，因此具备对象级原子替换，并在 SAVE namespace 下建立 platform-aware slot projection。代码不替换共享主机根目录。

`[规范]` restore consumer 应在 staging 中验证全部路径/CRC，再执行原子替换；任一 symlink、绝对路径、路径冲突或本地目标已存在时跳过自动 restore。`[代码实现]` 当前 C API 生成/读取 bundle bytes、枚举槽，不扫描目录或执行 host restore；staging/目标目录策略属于 adapter 边界。

`[测试证据]` `tests/test_writer_mutable.c` 覆盖两个 SAVE objects、普通 per-file projection、PSP `PARAM.SFO` 检查、目录槽成员、slot entry lookup、排序和读取，以及 path collision 返回 `ROMX_E_MUTABLE_BUNDLE`；规范 fixture 由相邻仓库的 provenance 固定快照提供。

## 10. STATS JSON profile

- `[规范][代码实现]` object key 通常为 `default`；JSON ≤16384 字节、严格 UTF-8、无 BOM、无重复/未知字段、无浮点/注释/trailing non-whitespace；顶层必需 `{"schema":"romx.stats","version":1}`。
- `[规范][代码实现]` 可选字段：`play_time_seconds`、`launch_count`、`first_played_unix_seconds`、`last_played_unix_seconds`（非负安全整数 ≤9007199254740991）、`favorite`、`completed`、`completion_percent` 0..100、`achievements` 对象。
- `[规范][代码实现]` achievements 必须有 `unlocked`、`total`，可有 `hardcore_unlocked`；`unlocked≤total`，`hardcore_unlocked≤unlocked`；first≤last（两者同时存在时）。
- `[代码实现]` `romx_mutable_stats_parse_json/serialize_json/read/write_path` 提供 parse/serialize 和 mutable object 读写；序列化使用紧凑 JSON 和固定字段顺序，读取接受其他空白/顺序。
- `[规范]` session delta 合并为累计时间/启动次数；首次时间取最早、最后时间取最晚；布尔/完成度按 profile 规则合并；冲突必须由调用方按 generation/最新对象处理，不能覆盖 immutable。
- `[已决][代码实现]` `romx_mutable_stats_merge_session_delta` 已确定在库内执行 safe-integer counter 相加、first/last 时间戳取 min/max；session 提供的用户状态与成就摘要覆盖 baseline。调用方仍负责重新读取最新 generation 和用户确认。
- `[测试证据]` `tests/test_writer_mutable.c` 覆盖序列化/解析、unknown key/duplicate key 拒绝、STATS object 写入再读取、session delta 合并及溢出拒绝。

## 11. Reader 访问模型与 API

### 11.1 Reader 生命周期与线程边界

- `[代码实现]` `romx_reader_open_io/open_path` 取得 size、读取末尾 128 字节、解析 footer/RIDX/mutable layout，然后返回 reader；open 不自动做 immutable SHA、metadata、cover、entry CRC 全量扫描。
- `[代码实现]` `romx_reader_get_info` 返回 region、hash algorithm、footer CRC、platform/launch、entry count/entrypoint；`romx_reader_validate` 按 flags 执行 hash/metadata/cover/entry CRC；`ROMX_VALIDATE_ALL` 为全量请求。
- `[代码实现]` positional `romx_reader_read_entry`/`romx_reader_read_region` 使用 caller-provided `romx_io.read_at`，支持大文件和短读循环；`romx_reader_get_payload_io` 暴露 entrypoint 的 borrowed IO view。
- `[代码实现]` `ROMX_VALIDATE_HASHES` 可在 validation report 中计算 payload region 的 SHA-256；`ROMX_VALIDATE_IMMUTABLE_SHA256` 才是 footer 规定的容器完整性校验。payload SHA 没有 footer 字段，也不是 metadata schema 的必填项；extraction 还会对实际写出的 entrypoint 做内部 SHA-256 稳定性检查。
- `[代码实现]` `romx_reader_map_payload` 仅在底层提供 map callback 时可用；`romx_payload_mapping_*` 释放 mapping。`romx_payload_file_*` 是 path-backed entrypoint positional cursor；`romx_vfs_file_*` 是任意 RIDX entry cursor。
- `[代码实现]` metadata、VFS、payload IO、mapping、mutable file、bundle 都借用 reader/input；必须在 reader 关闭前关闭/销毁 borrowed object；`romx_reader_close` 释放 entries/mutable slots 并调用 owned IO close callback。
- `[规范][代码实现]` reader 可报告 metadata/cover invalid 而仍提供 payload；mutable header invalid 时 mutable API 错误，但 immutable payload 不受影响；entry CRC mismatch 只影响对应校验/访问。
- `[已决][代码实现]` 线程安全合约已写入 `romx.h`：在 IO callback 自身线程安全时，reader metadata 与独立 positional read 可并发；VFS、payload-file、mutable-file、bundle cursor 带可变 seek 状态，同一 handle 不得并发 seek/read，需每线程独立 handle 或串行化。

### 11.2 Public C API（输入、输出、生命周期、错误）

下表将头文件中所有公共函数按族列出。`reader`、`metadata`、`file`、`mapping`、`bundle`、`probe` 是 opaque handle；除 `*_close` 外都不转移所有权。所有函数通过 `romx_result_t` 返回，失败时可填 `romx_error_t`，不退出进程。

| 接口族/函数 | 输入与输出 | 生命周期/线程说明 | 主要错误行为 |
|---|---|---|---|
| `romx_reader_open_io`, `romx_reader_open_path` | 输入 `romx_io` 或 UTF-8 path/options；输出 `romx_reader_t*` | reader 拥有 path IO；custom IO 由调用方保持有效 | invalid argument、truncated、invalid footer/index、IO、OOM |
| `romx_reader_get_info`, `get_entry_count`, `get_entry`, `get_entrypoint`, `find_entry` | 输入 reader/index/path；输出 value struct | 输出为复制值，reader 需存活 | argument、entry not found、index/range |
| `romx_reader_read_entry`, `read_region` | reader、entry/region、offset、buffer；输出 bytes_read | positional，无 cursor；buffer 由调用方拥有 | range、truncated、IO、invalid region |
| `romx_reader_validate` | reader、validation flags；输出 validation report | 可重复调用；全量 hash 是 O(region size) | immutable hash、entry CRC、metadata/cover error，按 flags 返回 |
| `romx_reader_get_payload_io`, `romx_reader_map_payload` | reader；输出 borrowed IO/mapping | reader 必须晚于 IO/mapping 关闭；mapping close 释放 | unsupported map、argument、IO |
| `romx_vfs_file_open/open_entrypoint` + `get_size/tell/seek/read/close` | reader/path 或 entrypoint；输出 cursor/bytes | cursor 非线程安全；借用 reader | entry not found、range/argument、truncated、IO |
| `romx_payload_file_open_path` + `get_size/tell/seek/read/close` | path/options；输出 path-backed cursor | 内部 reader 与文件句柄在 close 释放 | IO、invalid footer/index、range、truncated |
| `romx_payload_mapping_data/size/close` | mapping；输出 borrowed pointer/size | pointer 仅至 close；map 失败返回 unsupported | null-safe query；close 无返回 |
| `romx_metadata_open/close/copy_json/get_string/get_crc32/get_value_json` | reader/key/buffer；输出 metadata handle/复制值 | metadata handle 借用 reader bytes，close 释放 | absent、too large、UTF8/JSON/schema、buffer too small |
| `romx_extract_payload_path/cache`, `romx_extract_cover_path` | reader/destination/options 或 cache dir；输出文件/缓存路径效果 | 使用 temp + publish；destination 由调用方管理 | required integrity、extract hash、write、exists、atomic rename、cover errors |
| `romx_reader_get_mutable_status/count/object/find` | reader、namespace/key/index；输出 object/status | 每个 object 暴露前 CRC 校验；输出复制值 | mutable absent/header/entry/data CRC、not found、OOM |
| `romx_mutable_file_open` + `get_size/tell/seek/read/close` | reader/ns/key；输出 cursor/bytes | cursor 借用 reader；不可写 | mutable errors、range、truncated |
| `romx_mutable_write_io_path`, `romx_mutable_write_path`, `romx_mutable_delete_path` | ROMX path、namespace/key、source/options；输出 written object | 文件级锁；WRITING/data/ACTIVE 或 DELETING/zero commit | absent/header/no space/data CRC/IO/write |
| `romx_mutable_bundle_write_path_entries` | SAVE/CHEAT key、path entries/options；输出 object | 源文件仅在调用期间读取；外层原子对象写入 | invalid path/regular file、bundle limit/CRC/no space |
| `romx_mutable_bundle_open/get_entry_count/get_entry/read_entry/close` | reader/ns/key/options/index/offset；输出 bundle handle/entry/bytes | bundle 验证完成后才暴露；close 释放 | bundle header/path/padding/CRC/truncated |
| `romx_mutable_bundle_get_save_slot_count/get_save_slot/get_save_slot_entry` | SAVE bundle、slot index、slot entry index；输出 logical slot/member entry | 只接受 SAVE namespace；输出为复制值，bundle 需存活 | invalid argument、slot/entry index out of range |
| `romx_mutable_psp_savedata_inspect_sfo` | caller SFO bytes、size、可选 expected directory basename；输出 PSP identity/title flags | 纯内存、不扫描 host directory；输出 struct 由调用方提供 | invalid argument、malformed/missing identity -> `ROMX_E_MUTABLE_BUNDLE` |
| `romx_mutable_stats_parse_json/serialize_json` | bytes/stats/buffer；输出 parsed/JSON required_size | 无 reader；纯内存、可并行但 stats 对象由调用方管理 | mutable stats、invalid values、buffer too small |
| `romx_mutable_stats_read/write_path` | reader/key 或 path/key/stats；输出 stats/object | 读写走 mutable object 协议 | stats/mutable/IO/no space |
| `romx_probe_open_io/open_path/get_info/copy_metadata_json/copy_cover_png/close` | payload IO/path、format；输出 probe info/复制 bytes | 可选识别器；probe handle close 释放缓存 | unsupported、IO、buffer too small、cover invalid 被降级为 absent |
| `romx_writer_write_io_entries`, `romx_writer_write_path_entries` | destination、entry array、metadata/cover/options；输出 report | writer 流式读取 source；用 temporary + atomic publish | invalid input、metadata/cover/index/range、write/exists/rename |
| `romx_platform_name`, `romx_launch_format_name`, `romx_file_format_name`, `romx_result_string` | numeric code | 返回静态字符串或 NULL；无状态 | 未知 code 返回 NULL/unknown string，不抛出 |

`[已决][代码实现]` C++ wrapper 的 `romx::reader` 以 `romx::error` 抛异常，move-only，析构自动 close；覆盖常用 reader、writer、mutable object 和 STATS merge 操作。它不是第二套 ABI，也不承诺完整 VFS/mapping/bundle/probe/report API；这些继续使用 C ABI。

## 12. Writer 行为

- `[代码实现]` 参数校验：platform/launch 非零且不为 0xFFFF；entry path/IO/format 合法；恰好一个 entrypoint 且非空；ASCII-fold path 不冲突；metadata/cover 先完整验证。
- `[规范][代码实现]` payload 写入顺序：entrypoint first at offset 0，其余输入按 writer entry array 顺序；不压缩、不加密、不重写；累计 payload size，按 flags 计算 entry CRC32。
- `[代码实现]` 生成 RIDX header/entries/index CRC；metadata 和 cover 追加；有 mutable 时写零 padding、RMUT header、零填充容量；最后计算 immutable SHA256（如启用）并写 footer。
- `[代码实现]` 使用 destination 同目录临时文件（`mkstemp`/Windows exclusive open）；完成后 durable flush（可选）、关闭，再 `rename`/`link` 或 Windows `MoveFileEx` 原子发布；replace flag 决定覆盖。
- `[代码实现]` `ROMX_WRITER_PROBE_PAYLOAD` 可从 entrypoint probe 生成 metadata/cover；probe 失败 unsupported 时不阻断普通写入。
- `[规范]` mutable capacity 是固定预分配，不在 writer 后续自动增长；推荐容量是非规范 guidance，不能写成必须值。
- `[测试证据]` `tests/test_writer_mutable.c` 覆盖多 entry CUE、entry CRC、immutable SHA、metadata、12288 mutable、写/替换/delete；probe 测试生成 NDS metadata/cover。
- `[已决][代码实现][测试证据]` `ROMX_*_DURABLE` 先 flush 临时文件；POSIX 在 atomic rename/link 后通过 `src/durability.c` fsync 父目录，Windows 使用 FlushFileBuffers 与 write-through move，文档明确其为 file-level durability 边界。

## 13. 错误码、状态与降级

`[代码实现]` 主要 `romx_result_t`：

| 类别 | 错误码 |
|---|---|
| 参数/资源/IO | `ROMX_E_INVALID_ARGUMENT`, `ROMX_E_OUT_OF_MEMORY`, `ROMX_E_IO`, `ROMX_E_TRUNCATED`, `ROMX_E_RANGE`, `ROMX_E_WRITE`, `ROMX_E_ATOMIC_RENAME`, `ROMX_E_EXISTS`, `ROMX_E_BUFFER_TOO_SMALL` |
| Footer/结构 | `ROMX_E_INVALID_FOOTER`, `ROMX_E_INVALID_FLAGS`, `ROMX_E_OVERLAP`, `ROMX_E_INDEX`, `ROMX_E_UNSUPPORTED` |
| Integrity | `ROMX_E_IMMUTABLE_HASH`, `ROMX_E_ENTRY_CRC`, `ROMX_E_EXTRACT_HASH` |
| Metadata/Cover | `ROMX_E_METADATA_ABSENT`, `ROMX_E_METADATA_TOO_LARGE`, `ROMX_E_METADATA_UTF8`, `ROMX_E_METADATA_JSON`, `ROMX_E_METADATA_SCHEMA`, `ROMX_E_COVER_ABSENT`, `ROMX_E_COVER_TOO_LARGE`, `ROMX_E_COVER_PNG` |
| Path/entry | `ROMX_E_VIRTUAL_PATH`, `ROMX_E_ENTRY_NOT_FOUND` |
| Mutable | `ROMX_E_MUTABLE_ABSENT`, `ROMX_E_MUTABLE_HEADER`, `ROMX_E_MUTABLE_ENTRY`, `ROMX_E_MUTABLE_DATA_CRC`, `ROMX_E_MUTABLE_NO_SPACE`, `ROMX_E_MUTABLE_BUNDLE`, `ROMX_E_MUTABLE_STATS` |

- `[规范]` footer/region/RIDX 结构错误时不得信任容器；metadata/cover 错误可独立降级；mutable layout 错误只使 mutable unavailable；某个 object CRC 错误不得影响 immutable payload。
- `[代码实现]` `romx_error_t` 包含 code、system_code、byte_offset、256 字节 message；所有公开 API 返回错误而不进程退出。
- `[已决][规范][代码实现]` salvage 明确排除 0.2.0；普通 reader 对缺失/无效 footer fail-closed，C ABI 不返回 salvaged handle。独立恢复工具必须显式标记 unverified 且不得写回 mutable。

## 14. 安全与健壮性

- `[规范][代码实现]` 所有 offset+size、entry_count×entry_size、mutable capacity/directory、bundle path/data 计算均需先做整数溢出检查；越过 footer、区域重叠、非零 reserved/padding 都拒绝。
- `[规范][代码实现]` 路径禁止绝对路径、`..`、`.`、空组件、反斜杠、NUL；bundle source 必须 regular file，拒绝 symlink/reparse point；ASCII-fold collision 会拒绝。
- `[代码实现]` metadata 默认 1 MiB、cover 默认 32 MiB、cover dimension 默认 8192、I/O chunk 默认 64 KiB；writer/reader 可传上限；STATS 16384、bundle 默认 128 MiB、entry 上限 4096。
- `[代码实现]` malformed container 不会信任未知 footer；mutable header/slot/data CRC 分层降级；payload positional read 保证短读/截断显式错误。
- `[代码实现]` extraction/writer 使用随机临时文件名、exclusive create、临时完成后 atomic publish；cache 只在验证成功后发布，replace 由显式 flag 控制。
- `[已决][代码实现]` 公共头文件已表达并发边界：无状态的 reader metadata/positional read 可在安全 IO callback 下并发；具有 seek 状态的 cursor/bundle handle 不能跨线程共享。

## 15. 版本与兼容性

- `[规范]` ROMX 0.2.0 ↔ footer wire version 2；RIDX version 1；metadata schema `0.2.0`；mutable header version 1；RMBL version 1；STATS version 1。
- `[规范]` reserved 必须为零；未知 platform/format 非零（非 0xFFFF）不代表结构损坏，但可报告 unsupported；0x8000..0xFFFE 是私有/实验注册，需共享定义。
- `[代码实现]` `src/registry.c` 为已知 platform/launch/file format 提供静态名称，未知返回 NULL；`src/footer.c`/`src/ridx.c` 拒绝 0xFFFF，但不拒绝所有未知值。
- `[规范]` reader 可在未知 launch/platform 时保留结构信息但不能假装可启动；writer 需要非零合法 platform/launch；payload entry format_id 是 entry-level hint，RIDX/entrypoint 边界更权威。
- `[已决][规范][代码实现]` Reader 对未知 wire version fail-closed；registry status API 明确 KNOWN、UNSPECIFIED/UNKNOWN、PRIVATE、PROHIBITED；未知非零值可保留数值读取但不得假装可启动，0xFFFF 结构无效。

## 16. 测试与 conformance matrix

| 能力/夹具 | 规范预期 | 现有证据 | 状态 |
|---|---|---|---|
| 最小单文件 | payload offset 0、entrypoint、无可选区 | `minimal-single.manifest.json`、`tests/test_v2.c` | `[测试证据]` |
| metadata + cover + mutable + immutable SHA | 连续 region、PNG/JSON/sha valid | `single-complete.manifest.json`、`tests/test_v2.c` | `[测试证据]` |
| 多文件 CUE/BIN | entrypoint + virtual paths + entry CRC | `multi-cue.manifest.json`、`tests/test_v2.c` | `[测试证据]` |
| writer payload/RIDX | entrypoint first、CRC/index、atomic publish | `tests/test_writer_mutable.c` | `[测试证据]` |
| mutable object | write/replace/generation/delete、data CRC | `tests/test_writer_mutable.c`、`tests/test_v2.c` | `[测试证据]` |
| SAVE/CHEAT bundle | header/path table/file CRC/path collision | `tests/test_writer_mutable.c` | `[测试证据]` |
| SAVE slot projection | 多 object、非 PSP per-file slot、PSP PARAM.SFO 目录 slot、slot member lookup | `tests/test_writer_mutable.c`；`src/mutable_bundle.c` | `[测试证据][代码实现]` |
| PSP SFO inspection | DISC_ID identity、expected directory label、bounded SFO input | `tests/test_writer_mutable.c`；`src/mutable_bundle.c` | `[测试证据][代码实现]` |
| STATS JSON | schema/version/duplicate/unknown/round-trip | `tests/test_writer_mutable.c` | `[测试证据]` |
| probe | 可选 metadata/cover 生成 | `tests/test_writer_mutable.c` | `[测试证据]` |
| 错误 magic/截断/footer CRC | fail closed | source unit paths and pinned adjacent reference fixtures | `[测试证据]`：fixture 来源与 hash 见 provenance |
| RIDX CRC/entry CRC/重叠/gap | reject or report according to validation layer | `src/ridx.c`, `src/validate.c`、固定标准 fixtures | `[测试证据][代码实现]` |
| 路径穿越/大小写冲突/symlink | reject | writer/bundle source validators and conformance fixtures | `[测试证据][代码实现]` |
| immutable hash mismatch | immutable validation error，mutable independent | `tests/test_v2.c` | `[测试证据]` |
| mutable data corruption/header corruption | object CRC vs mutable unavailable 分层 | `tests/test_v2.c` | `[测试证据]` |
| 中断写入/断电恢复 | WRITING/DELETING 不暴露，immutable 不变 | 协议文字、commit code、intermediate-state tests | `[测试证据]`；真实断电属于平台发布验收 |
| 多平台 gcc/clang/MSVC | C ABI/大文件/无编译警告 | CI 与 clean build | `[测试证据]`；SFO helper warning 已消除 |

`[规范][测试证据]` 当前工作树不复制游戏字节；CMake/conformance 从 provenance 固定的相邻标准仓库取得 fixtures，并记录 manifest SHA-256 与来源 commit。

## 17. 当前实现与标准一致性登记

| 项目 | 证据位置 | 当前状态 | 决议 |
|---|---|---|---|
| 规范文件归属 | `/Volumes/Repositories/romx/docs/*`、`docs/ROMX-0.2.0-PROVENANCE.md` | 来源 commit 与文档/schema/fixture README hash 已冻结 | `[已决]` 以 provenance 为唯一版本边界 |
| Unicode path/key | `src/ridx.c`、`src/mutable.c`、`src/mutable_bundle.c`、冻结规范 | 非 ASCII UTF-8 字节保持不变，仅 ASCII A–Z fold | `[已决]` 完成 0.2.0 互操作边界 |
| Unknown IDs | `src/registry.c`、`include/romx/romx.h`、`tests/test_writer_mutable.c` | status API 区分 KNOWN/UNSPECIFIED/UNKNOWN/PRIVATE/PROHIBITED | `[已决]` 未知非零可读但 unsupported，0xFFFF 无效 |
| Salvage | 标准 salvage 段、`reader.c`、公共头文件 | 普通 reader fail-closed，0.2.0 不提供 salvage handle | `[已决]` 明确排除本版本 |
| STATS merge | `src/mutable_stats.c`、`include/romx/romx.h`、C++ wrapper、测试 | baseline+session delta、safe-integer、min/max、session 状态覆盖 | `[已决]` library helper 实现，调用方负责最新 generation/确认 |
| Bundle restore | `docs/INTEGRATION.md`、`src/mutable_bundle.c`、冻结规范 | libromx 只验证/投影/读取 bytes；host staging/原子替换为 adapter 职责 | `[已决]` 不扩展核心 ABI |
| SAVE slot projection | `src/mutable_bundle.c:611-638,1400-1520`、冻结规范、测试 | PSP 有效 SFO root、最长前缀；未知/UNSPECIFIED per-file | `[已决]` 只允许显式 platform policy 分组 |
| PSP PARAM.SFO grammar | `src/mutable_bundle.c:305-530`、冻结规范、测试 | bounded PSF v1.01，校验 table/record、integer/string/binary、NUL string、identity | `[已决]` malformed table 直接拒绝 |
| Durable publish | `src/durability.c`、`src/writer.c`、`src/extract.c`、集成文档 | POSIX rename/link 后 fsync parent；Windows file-level write-through | `[已决]` durable 边界固定 |
| Power-loss behavior | `src/mutable_write.c`、`tests/test_v2.c`、`tests/test_writer_mutable.c` | WRITING/DELETING 中间态隔离，immutable 不受影响 | `[已决]` 真实断电为平台验收，不改 wire |
| Payload SHA | `src/extract.c`、`src/validate.c`、`include/romx/romx.h` | 仅派生/提取自检，不进入 footer/metadata | `[已决]` immutable SHA 是唯一容器级 hash |
| Thread safety | `include/romx/romx.h`、`src/vfs_file.c`、`src/mutable.c` | metadata/positional read 在安全 IO callback 下可并发；cursor 不可共享 | `[已决]` 合约写入公共头文件 |
| C++ wrapper | `include/romx/romx.hpp` | move-only RAII convenience；覆盖常用 reader/writer/mutable/STATS merge | `[已决]` 完整面仍以 C ABI 为准 |
| Compiler warning | `src/mutable_bundle.c:305-307`、clean build | 小端 helper 使用显式 cast，项目警告选项下通过 | `[已决]` 已消除 Wconversion warning |

### 已决事项来源索引

| 已决事项 | 源码/文档位置 | 决议依据 |
|---|---|---|
| 来源与冻结快照 | `/Volumes/Repositories/romx/docs/ROMX-SPEC*.md`、`schema/`、`tests/fixtures/`、`docs/ROMX-0.2.0-PROVENANCE.md` | provenance 记录 commit 与 hash，规范状态为 frozen snapshot |
| Unicode fold | `src/ridx.c`、`src/mutable.c`、`src/mutable_bundle.c`、规范 4.5/6.4.1 | 0.2.0 明确 ASCII-only fold；非 ASCII 字节保持原样 |
| Registry status | `src/registry.c`、`include/romx/romx.h`、`tests/test_writer_mutable.c` | status API 与 0xFFFF 拒绝规则已覆盖 |
| Salvage | 规范 4.6、`src/reader.c`、公共 API | 只作为独立恢复工具边界，普通 reader 不提供 salvage |
| STATS merge | `src/mutable_stats.c`、`include/romx/romx.h`、`include/romx/romx.hpp`、测试 | merge helper 与 safe-integer overflow 测试已加入 |
| Bundle restore | 规范 6.4.2、`docs/INTEGRATION.md`、`src/mutable_bundle.c` | 适配层负责 host staging/replace，核心只处理 bundle bytes |
| PSP slot ownership | 规范 6.4.1、`src/mutable_bundle.c`、测试 | longest valid root 与 per-file fallback 已覆盖 |
| SFO grammar | 规范 6.4.1、`src/mutable_bundle.c`、测试 | PSF v1.01 table/record/encoding/identity 检查已冻结 |
| Durable publish | `src/durability.c`、`src/writer.c`、`src/extract.c`、测试/文档 | POSIX parent fsync 与 Windows file-level 语义已固定 |
| Thread safety | `include/romx/romx.h`、集成文档 | cursor ownership 与 callback 并发要求已成为 API 合约 |
| C++ wrapper | `include/romx/romx.hpp`、C ABI 对照 | wrapper 范围已固定，不替代完整 C ABI |
| Compiler warning | `src/mutable_bundle.c:305-307` | explicit cast 修复后 clean build 通过 |

## 18. 审阅结论

- `[代码实现][测试证据]` 当前核心已覆盖 0.2.0 的 footer、RIDX、metadata、cover、payload view/VFS、writer、mutable object、SAVE/CHEAT bundle、platform-aware SAVE slot projection、PSP SFO identity inspection、STATS、probe 主要路径。
- `[规范]` immutable 与 mutable 的边界、CRC/SHA 计算范围、原子提交顺序和路径安全规则已经足够形成实现审查基线。
- `[已决][规范][代码实现][测试证据]` 发布门禁项目已全部关闭：标准来源与 hash 已冻结；ASCII-only fold、registry status、unknown-platform per-file projection、nested PSP ownership、PSF v1.01 grammar、salvage 范围、STATS merge、bundle restore adapter 边界、durable publish、cursor 并发合约和编译器 warning 均有对应决议与实现/测试证据。
