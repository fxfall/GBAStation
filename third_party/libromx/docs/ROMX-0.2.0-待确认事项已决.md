# ROMX 0.2.0 待确认事项决议

这是 `libretro` 分支的发布门禁记录，取代早期技术审查草案中的待确认标记。
wire 快照固定在 `ROMX-0.2.0-来源冻结.md`。

| # | 原待确认项 | 决议与当前适配 |
|---:|---|---|
| 1 | 标准版本与 fixture 归属 | **已确定。** Footer wire `2`、RIDX `1`、metadata `0.2.0`、RMUT/RMBL `1`、STATS `1` 已冻结。规范/fixture 来源 commit 与文档 hash 记录在来源冻结文件；不把游戏字节放入 libromx。 |
| 2 | Unicode NFC/full case-fold | **0.2.0 已确定边界。** 路径与 key 要求有效 UTF-8；非 ASCII 字节保持不变，冲突检查只折叠 ASCII `A`–`Z`。完整 Unicode 规范化属于未来 profile，不是未声明的依赖；中文标题仍可正常使用。 |
| 3 | 未知 platform/launch/file ID | **已确定。** `romx_*_status()` 返回 `KNOWN`、`UNSPECIFIED`、`UNKNOWN`、`PRIVATE` 或 `PROHIBITED`；platform/launch 的零值是 `UNSPECIFIED`，RIDX file format 的零值是 `UNKNOWN`。未知非零 ID 仍可按数字读取但不支持；`0xFFFF` 结构无效。RetroArch 必须提示兼容性问题，不能静默自动探测。 |
| 4 | Salvage API | **确定不属于 0.2.0。** Footer 缺失/无效时普通 reader fail-closed；libromx 0.2.0 不提供 salvage handle。Consumer 可以提供单独且明确标记的恢复工具，但不能写回 mutable。 |
| 5 | STATS session merge/冲突 | **已在库中确定。** `romx_mutable_stats_merge_session_delta()` 在 safe-integer 范围内相加 counter，时间戳取 min/max，session 提供的用户状态/成就汇总覆盖 baseline。前端仍负责重新读取最新 generation 和用户确认。 |
| 6 | Bundle host restore | **已确定为适配层职责。** libromx 只校验、投影并流式读取 bundle；RetroArch 负责 staging、目标路径、symlink 检查、冲突提示和只替换选中 content path 的原子操作，绝不替换共享 save root。 |
| 7 | 中断写入/断电 fixture | **协议已确定，真实断电为显式平台验收。** `WRITING → data → ACTIVE` 与 `DELETING → clear` 顺序属于规范，reader 会隔离未完成 entry；单元测试覆盖损坏/中间状态。真实断电运行属于发布测试，不新增 wire 规则。 |
| 8 | durable publish 边界 | **已确定。** `ROMX_*_DURABLE` 先 flush 临时文件；POSIX 在 atomic rename/link 后再 fsync 父目录。Windows 通过 `FlushFileBuffers`/write-through move 提供 file-level durability，因为没有可移植的目录句柄 flush 合约。 |
| 9 | Payload SHA 规范地位 | **确定为派生数据。** Payload SHA 只用于 extraction 内部自检，不是 footer 字段、metadata 要求或信任信号；唯一存储的容器级 hash 仍是 immutable SHA。 |
| 10 | 同一 cursor 线程安全 | **已确定为 API 合约。** reader 元数据与 positional read 在调用方 IO callback 安全时可并发；VFS、payload-file、mutable-file 与 bundle cursor 有 seek 状态，不能在多个线程共享同一 handle，需每个消费者独立打开或串行化访问。 |
| 11 | C++ wrapper 覆盖面 | **已确定范围。** Header-only wrapper 是可选的 move-only RAII 便利层，覆盖 reader、writer、mutable object 与 STATS merge；不是 ABI，也不承诺完整 VFS/mapping/bundle/probe/report API，后者使用 C ABI。 |
| 12 | 未知平台 SAVE projection | **已确定。** 未知非零与 `UNSPECIFIED` 均使用保守的“一文件一槽”；只有显式注册的平台 policy 才能启用目录分组，禁止按目录数量猜测。 |
| 13 | 嵌套 PSP root | **已确定。** 每个有效 `PARAM.SFO` root 都是候选；文件归入最长匹配的有效 root，内层 savedata root 拥有自己的子树，外层只保留其余成员。 |
| 14 | PSP PARAM.SFO grammar | **已确定。** 使用有界 PSF v1.01，校验 table/record 范围，接受标准 integer/string/binary 格式，String 值必须在 `data_length` 内有 NUL，并要求有效 `DISC_ID` 或与 basename 匹配的 `SAVEDATA_DIRECTORY`。表损坏直接拒绝，不做 salvage。 |
| 15 | 编译器 warning | **已确定。** SFO 小端读取 helper 已显式 cast；当前 macOS 工具链使用项目的 `-Wall -Wextra -Wpedantic -Wconversion -Wshadow` 编译干净。 |

## 验证证据

- 修改后 `ctest --test-dir build --output-on-failure`：libromx 2/2 通过（reader/VFS/payload
  与 writer/mutable/bundle/PSP/STATS/probe）。
- writer-mutable 测试新增嵌套 PSP root、严格 SFO 版本拒绝、未知 platform 的 per-file
  fallback、STATS delta/溢出和 registry status 分类覆盖。
- 主机目录 restore 与前端路径测试仍由现有 RetroArch ROMX 适配层负责；本轮不改任何
  core ABI 或模拟器核心源码。
