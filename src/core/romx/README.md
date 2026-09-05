# GBAStation ROMX 模块

本目录是 GBAStation 的 ROMX 接入层，通过 `GBAStation::ROMX` 接入构建。
`third_party/libromx` 是 `/Volumes/Repositories/libromx` 的通用 API 源码副本，
不放 GBAStation 路径、核心协议、页面逻辑或自动导入策略。

## 职责

- `RomxFrontend`：把容器信息映射为前端字段，管理封面缓存、载荷映射及释放。
- `RomxGameEntryAdapter`：GameEntry、实际核心存档目录、命名槽选择、手动覆盖、
  存档/金手指/统计写回。存档格式识别和相对布局仍调用 libromx 公共 API。
- `LibretroRomxSession`：一个启动会话统一持有映射、VFS 和缓存回退，核心卸载后释放。
- `RomxVfs`：libretro VFS 桥接；不属于 libromx。
- `CMakeLists.txt`：源码清单和 Switch 缺失系统能力的编译配置。

页面仅调用模块接口，不直接引用 libromx；核心加载器只传递已协商的能力和
启动参数。INTERFACE 构建目标让模块使用前端原有的 SDK/平台编译选项，避免
为 Switch 或 macOS 另设一套容易偏离的参数。

## 必须保持的行为

- PSP/3DS 首次启动不自动恢复存档，用户手动选择并确认覆盖。
- ROMX 管理只处理已有容器，不在前端创建普通 ROM 的 ROMX 副本。
- FBNeo 接收完整 ZIP 载荷，由核心解压；前端不展开 ZIP 内容。
- need_fullpath 核心可使用 VFS；不支持或拒绝 VFS 的核心使用缓存文件回退。
- 实际 3DS Title Save/ExtData 路径和 PSP 的活动存档目录属于本模块；
  libromx 只返回格式、ID、相对布局及内容。

## 同步与验证

先修改独立 libromx 并运行其测试，再同步 vendored 的 `src`、`include`、构建文件
和配套文档/测试。不要在 vendored 源码里长期保留前端专用补丁。
前端平台配置只改本目录的构建文件。变更后至少运行 libromx 回归测试、
`tests` 下的模块测试和前端编译；NS 真机验证不能由 macOS 编译替代。

模块测试可独立构建，无需编译模拟器核心或运行界面：

```sh
cmake -S src/core/romx/tests -B /tmp/gbastation-romx-tests
cmake --build /tmp/gbastation-romx-tests
ctest --test-dir /tmp/gbastation-romx-tests --output-on-failure
```
