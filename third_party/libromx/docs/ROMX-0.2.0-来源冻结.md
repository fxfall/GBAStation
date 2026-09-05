# ROMX 0.2.0 发布门禁来源冻结

本文件关闭 `libretro` 分支的版本归属问题。以下字节快照是本 libromx 分支唯一消费的
ROMX 0.2.0 wire 合约：

| 项目 | 固定值 |
|---|---|
| 容器格式 | ROMX 0.2.0 |
| Footer wire | `2` |
| RIDX wire | `1` |
| Metadata schema | `0.2.0` |
| Mutable header | `RMUT` v1 |
| Bundle profile | `RMBL` v1 |
| STATS profile | `romx.stats` v1 |
| ROMX 规范来源 commit | `d0f7bb7d979ea4a0189e6d4d8b2cbf3536eded82` |
| 英文规范 SHA-256 | `a9b67cea76c5c44cf68c124c4d4a7c848eae28dcc3f9bfa4213816e16ef06e67` |
| 中文规范 SHA-256 | `61c8535fca58f0880f978c214506d378b6a9b7e4271c818599ced4ed57eda7e2` |
| Metadata schema SHA-256 | `edb162ef3107081eeaf78ba20150327c63aac460de5f483b1eb30ef08b8ebb88` |
| 英文 fixture README SHA-256 | `95df77a3158d898b004d6e063d4b0e7ef2c97a93a893fbe9ccfc28e818fc6097` |
| 中文 fixture README SHA-256 | `cfc18ebd3a35828bd2918a8fe2866f51514f2f42357b18a04d047790c642c7c1` |

来源 commit 标识 `romx` `libretro` 基线；文档与 schema hash 标识本次决议后的冻结工作树
快照。commit 与这些 hash 共同构成 0.2.0 的不可变 provenance 边界；未来 wire 变化必须
发布新的来源冻结记录。

规范仓库是独立依赖；libromx 不把受版权保护的游戏 fixture 复制进源码。CI 或发布构建
必须从固定来源取得 fixture 目录，并记录生成 fixture 文件的 hash。序列化语义变化必须
使用新的 wire version 或新的来源冻结记录；不改变上述值的实现修复仍兼容 0.2.0。

## 本快照固定的兼容决策

- 路径与 mutable key 要求有效 UTF-8、`/` 分隔符，并使用 libromx 已实现的 ASCII-only
  fold。完整 Unicode NFC/大小写折叠属于未来 profile，不是 0.2.0 的隐式依赖。
- platform 或 launch-format 的零值是 `UNSPECIFIED`；RIDX file-format 的零值是
  `UNKNOWN`，只允许用于非 entrypoint 文件。
- 未知且非零的 registry ID 仍按数字可读，但会报告 unsupported。未知 platform ID 使用
  保守的 per-file SAVE-slot projection，绝不自动启用 PSP 分组。
- Reader 对未知 wire version fail-closed；0.2.0 C ABI 没有 salvage handle。
- PSP slot profile 使用经过校验的 v1.01 PARAM.SFO record，并按最长有效路径前缀归属
  嵌套 root。
