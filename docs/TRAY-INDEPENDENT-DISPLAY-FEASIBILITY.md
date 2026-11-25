# IslandBar 独立托盘显示技术路线与可行性报告

> 项目：IslandBar-DynamicIsland
>
> 平台：Windows 11 x64
>
> 语言与 UI：C++20、Win32、Direct2D、DirectComposition
>
> 审计基线：`51d7df24e0e4baef18535866a3e94636e91e219f`
>
> 报告日期：2026-11-25
>
> 本阶段状态：只完成源码审计、Windows 机制调查、原型设计和技术决策；不修改生产代码

---

## 0. 结论摘要

IslandBar 可以继续采用“独立显示”路线，但应把它定义为**高覆盖的兼容层**，不能承诺对所有第三方应用都实现 100% 原生等价体验。

当前项目已经实现了一条可工作的混合路线：

1. 用 `HKCU\Control Panel\NotifyIconSettings` 中的图标快照负责绘制。
2. 用 Explorer 进程内的观察器获取实时注册身份、回调窗口、回调消息、版本和 Tooltip。
3. 用共享内存 ring 把观察结果送回 IslandBar。
4. 用固定容量 ledger 判断某个注册是否仍被 Shell 持有。
5. 用应用自己的回调消息完成主要点击交互。
6. UIA 只作为无法学习回调时的可选左键兜底。

本机在 Windows `10.0.26320` 上实测：

- Explorer PID：`11992`
- IslandBar PID：`23404`
- `Shell_TrayWnd`：存在，但已隐藏
- `TrayNotifyWnd`：存在
- `ToolbarWindow32`：不存在
- 隐藏任务栏状态下，`SystemTray_Main` 的 UIA descendants：`0`
- 当前运行日志最终达到：`9 icon(s) -- 9 matched ...; 0 duplicate(s) collapsed`

因此：

- **独立绘制可行。**
- **第三方应用的左键交互有较高可行性。**
- **应用原生右键菜单只能尽力兼容。**
- **UI Automation 不能作为独立绘制的数据源。**
- **公开 API 不存在“枚举全部托盘图标”的完整方案。**
- **必须保留注册表兜底和 IslandBar 自己的菜单兜底。**

---

## 1. 审计范围与方法

本次审计以当前仓库 HEAD 为准，重点读取了以下实现：

- [TrayIcons.cpp](../IslandBar/src/shell/TrayIcons.cpp)
- [TrayIcons.h](../IslandBar/src/shell/TrayIcons.h)
- [TrayLearn.cpp](../IslandBar/src/shell/TrayLearn.cpp)
- [TrayLearn.h](../IslandBar/src/shell/TrayLearn.h)
- [TrayPress.cpp](../IslandBar/src/shell/TrayPress.cpp)
- [TrayPress.h](../IslandBar/src/shell/TrayPress.h)
- [TrayUia.cpp](../IslandBar/src/shell/TrayUia.cpp)
- [TrayUia.h](../IslandBar/src/shell/TrayUia.h)
- [TrayMenu.cpp](../IslandBar/src/shell/TrayMenu.cpp)
- [TrayMenu.h](../IslandBar/src/shell/TrayMenu.h)
- [TrayLedger.cpp](../IslandBar/src/domain/TrayLedger.cpp)
- [TrayLedger.h](../IslandBar/src/domain/TrayLedger.h)
- [TrayEncoding.h](../IslandBar/src/domain/TrayEncoding.h)
- [TrayRegistration.h](../IslandBar/src/domain/TrayRegistration.h)
- [TrayRing.h](../IslandBar/src/platform/TrayRing.h)
- [TrayHook.cpp](../IslandBar/src/trayhook/TrayHook.cpp)
- [NativeTaskbar.cpp](../IslandBar/src/platform/NativeTaskbar.cpp)
- [BarLoop.cpp](../IslandBar/src/app/BarLoop.cpp)
- [TextRenderer.cpp](../IslandBar/src/render/TextRenderer.cpp)
- [SystemStatus.cpp](../IslandBar/src/shell/SystemStatus.cpp)

审计方法：

- 静态阅读实际函数，不根据文件名推断。
- 搜索生产代码中的 Win32 API、窗口类名和托盘数据结构。
- 对当前交互桌面做只读窗口树探测。
- 读取当前运行日志与 callback cache。
- 对照 Microsoft 官方文档区分公开 API 与非公开实现。

本次没有执行以下操作：

- 没有修改生产代码。
- 没有重启 Explorer。
- 没有重新构建 Debug 或 Release。
- 没有修改、删除或注入第三方应用。
- 没有重新执行五个应用的完整交互矩阵。

---

## 2. 交付 A：现有源码审计

### 2.1 当前实现架构

```text
第三方应用
    |
    | Shell_NotifyIcon
    v
Windows Shell / Explorer
    |                         HKCU\Control Panel\NotifyIconSettings
    |                         |
    | WM_COPYDATA             | IconSnapshot / Tooltip / UID / GUID
    v                         v
island_tray_hook.dll      TrayIcons::Refresh
    |                         |
    | shared-memory ring      | registry adapter
    v                         |
TrayLearn / TrayLedger <------+
    |
    +------ TrayIcons 快照 ------+
    |                            |
    v                            v
TrayPress / TrayUia          TextRenderer / CardLayout
    |                            |
    v                            v
应用自己的回调消息             IslandBar Liquid Glass 托盘页
```

当前模块职责：

| 模块 | 当前职责 |
|---|---|
| `TrayHook.cpp` | 进入 `Shell_TrayWnd` 所在进程，拦截发给 Shell 的托盘注册 `WM_COPYDATA`，解码后写入共享内存 ring |
| `TrayRing.h` | 定义模块与主程序之间的固定大小共享内存记录、控制记录和 ABI 检查 |
| `TrayRegistration.h` | 定义注册 payload 的纯逻辑解码，不依赖 Win32 |
| `TrayLearn.cpp` | 安装观察器、等待 observer ready、广播 `TaskbarCreated`、drain ring、维护 ledger 与 callback cache |
| `TrayLedger.cpp` | 判断 Shell 当前是否持有某个注册，处理 ADD、MODIFY、DELETE、窗口死亡和容量上限 |
| `TrayIcons.cpp` | 读取注册表图标快照，与 ledger 求交，生成绘制列表和 revision |
| `TrayEncoding.h` | 唯一负责 v0/v3/v4 点击消息编码的纯逻辑入口 |
| `TrayPress.cpp` | 向应用自己的托盘窗口投递回调，判断应用是否响应，维护左键配方和右键菜单策略 |
| `TrayUia.cpp` | 在显式启用时，通过 UIA Invoke 请求 Shell 替 IslandBar 按图标 |
| `TrayMenu.cpp` | IslandBar 自己的兜底菜单、应用激活和结束进程操作 |
| `NativeTaskbar.cpp` | 隐藏或裁剪原生任务栏、重新隐藏、Explorer 重启恢复、工作区处理 |
| `BarLoop.cpp` | 启动托盘模块、周期刷新、Explorer 重启检测、点击分发和状态更新 |
| `TextRenderer.cpp` | 按 revision 缓存托盘 PNG 解码后的 D2D bitmap |

### 2.2 任务书十五问逐项回答

#### 1. 当前 IslandBar 如何发现托盘图标？

- 文件：[TrayIcons.cpp](../IslandBar/src/shell/TrayIcons.cpp)
- 函数：`TrayIcons::Refresh`
- 已确认逻辑：
  - 打开 `HKCU\Control Panel\NotifyIconSettings`。
  - 枚举每个子键。
  - 读取 `ExecutablePath`、`IconSnapshot`、`InitialTooltip`、`IsPromoted`、`UID`、`IconGuid`。
  - 只有 PNG 有效的条目才进入候选列表。
  - 当 ledger 已初始化且观察器常驻时，以 ledger 中的实时注册为准。
  - 当 ledger 尚未初始化或观察器不常驻时，退回“进程正在运行”的注册表列表。
  - 多个注册表条目指向同一个 Shell 注册时，通过 `SameTrayIcon` 折叠成一个图标。
- 已确认事实：
  - 发现主路线不是 `EnumWindows`，也不是 `ToolbarWindow32`。
  - Explorer 内观察器提供实时注册；注册表提供图标和稳定标签。
- 尚未验证的假设：
  - Windows 后续版本是否继续使用 `NotifyIconSettings` 和相同的值名。
  - 所有应用的 registry snapshot 是否都能及时反映最新 HICON。
- 潜在问题：
  - 该注册表布局是 Shell 私有实现，不属于公开稳定 API。
  - `upgradedIcons_` 目前没有容量淘汰策略。

#### 2. 当前是否读取 Explorer 的 `ToolbarWindow32`？

- 文件：生产源码中未找到读取实现
- 已确认事实：
  - `rg` 搜索仅在历史说明、README 和历史交接文档中出现该窗口类。
  - 当前 Windows `10.0.26320` 的 `Shell_TrayWnd` 子窗口中不存在 `ToolbarWindow32`。
- 当前本机实测窗口树：

```text
Shell_TrayWnd
  TrayDummySearchControl
  TrayNotifyWnd
  ReBarWindow32
    MSTaskSwWClass
      MSTaskListWClass
```

- 尚未验证的假设：
  - Windows 10 或部分早期 Windows 11 是否仍存在经典 `ToolbarWindow32` 路线。
- 潜在问题：
  - 不应把经典 `TB_GETBUTTON + TRAYDATA` 方案放回正式架构。

#### 3. 当前是否读取 `TrayNotifyWnd`？

- 文件：[NativeTaskbar.cpp](../IslandBar/src/platform/NativeTaskbar.cpp)
- 函数：`NativeTaskbar::Locate`、`NativeTaskbar::Hide`、`NativeTaskbar::ApplyTrayRegion`
- 已确认逻辑：
  - `TrayNotifyWnd` 用于识别原生通知角落。
  - `Hide` 会显式隐藏它。
  - `--native-tray` 模式下，可把任务栏裁剪到 `TrayNotifyWnd` 的矩形。
- 已确认事实：
  - `TrayNotifyWnd` 不参与第三方托盘图标枚举。
- 尚未验证的假设：
  - 某些 Windows 构建是否会改变它的父子关系。
- 潜在问题：
  - 完全隐藏 `TrayNotifyWnd` 可能改变部分系统通知和 Shell 行为的呈现路径，需要单独回归测试。

#### 4. 当前是否使用 `Shell_NotifyIconGetRect`？

- 文件：生产源码中没有实际调用
- 已确认事实：
  - [TrayIcons.h](../IslandBar/src/shell/TrayIcons.h) 的注释仍声称按压需要它。
  - 当前按压路径没有调用该 API。
- 官方边界：
  - `Shell_NotifyIconGetRect` 只能根据已知的 `NOTIFYICONIDENTIFIER` 查询一个图标的屏幕矩形。
  - 它不能枚举全部托盘图标。
- 尚未验证的假设：
  - 是否还需要它作为未来几何诊断工具。
- 潜在问题：
  - 文档与实现漂移，容易让后续维护者误以为当前依赖该 API。

#### 5. 当前图标 HICON 从哪里获得？

- 文件：[TrayIcons.cpp](../IslandBar/src/shell/TrayIcons.cpp)
- 函数：`TrayIcons::Refresh`、`PngSide`、`UseExecutableIcon`
- 已确认逻辑：
  - 主图标来源是注册表 `IconSnapshot` 的 PNG 字节。
  - 当 PNG 边长小于 40 且 exe 不是 Explorer 或系统 host 时，尝试从 exe 提取 64px 图标并编码为 PNG。
  - 当前没有持有托盘 HICON。
- 已确认事实：
  - 托盘显示没有走 `GetIconInfo`、`CopyIcon` 或 `DestroyIcon`。
  - 这消除了托盘 HICON 跨进程生命周期问题。
- 尚未验证的假设：
  - registry PNG 对头像闪烁、动态通知图标和不同 DPI 是否始终足够。
- 潜在问题：
  - HICON 更新若没有同步到 `IconSnapshot`，独立托盘会显示旧图。

#### 6. 图标更新通过什么机制发现？

- 文件：
  - [TrayLearn.cpp](../IslandBar/src/shell/TrayLearn.cpp)
  - [BarLoop.cpp](../IslandBar/src/app/BarLoop.cpp)
  - [TextRenderer.cpp](../IslandBar/src/render/TextRenderer.cpp)
- 函数：
  - `TrayWakeThreadEntry`
  - `TickTray`
  - `TrayIcons::Refresh`
  - `TextRenderer::TrayIconBitmap`
- 已确认逻辑：
  - 后台线程等待 ring named event 和 `RegNotifyChangeKeyValue`。
  - 任一事件只发送合并后的 `kUiWakeTray`。
  - 主循环在收到 wake 或 5 秒兜底时间到达时刷新托盘。
  - 名称或 PNG 内容变化时递增 `TrayIcons::Revision`。
  - 渲染层按 revision 重新解码 bitmap。
- 已确认事实：
  - 没有每帧扫描注册表。
  - 没有每帧跨进程读取图标。
- 尚未验证的假设：
  - registry 通知在高频动态图标下是否足够及时。
- 潜在问题：
  - registry 事件仍可能触发完整注册表枚举；高频更新时需要测量。

#### 7. Tooltip 从哪里读取？

- 文件：
  - [TrayIcons.cpp](../IslandBar/src/shell/TrayIcons.cpp)
  - [TrayLedger.cpp](../IslandBar/src/domain/TrayLedger.cpp)
  - [TrayHook.cpp](../IslandBar/src/trayhook/TrayHook.cpp)
- 已确认逻辑：
  - 注册表提供 `InitialTooltip`。
  - `WM_COPYDATA` payload 提供注册时的 `szTip`。
  - ledger 在非空 MODIFY 时更新 tip。
  - 当前单元格名称优先使用稳定的 `InitialTooltip`；为空时才使用注册 tip。
- 已确认事实：
  - 当前没有完整复刻原生 hover tooltip。
- 尚未验证的假设：
  - 是否所有应用都会把最新 tooltip 写入 registry snapshot。
- 潜在问题：
  - QQ 音乐、WeGame、Clash Verge 等会动态改变 tooltip；当前数据结构没有把“稳定标签”和“最新动态 tooltip”完全分开。

#### 8. 是否能获取应用原始回调 HWND？

- 文件：
  - [TrayHook.cpp](../IslandBar/src/trayhook/TrayHook.cpp)
  - [TrayRegistration.h](../IslandBar/src/domain/TrayRegistration.h)
  - [TrayLedger.cpp](../IslandBar/src/domain/TrayLedger.cpp)
- 已确认逻辑：
  - payload `+0x00C` 处的 `trayWindow` 是应用注册的回调窗口。
  - `TrayLiveRegistration` 保存该窗口数值。
- 已确认事实：
  - 注册观察阶段可以获取原始 HWND。
- 尚未验证的假设：
  - 某些应用是否会在一次进程生命周期中使用多个同 class 回调窗口。
- 潜在问题：
  - 当前 callback cache 不保存 HWND，而是保存窗口类。
  - 按压时通过 exe 查找该 class 的第一个窗口；同名 class 多窗口时可能选错。

#### 9. 是否能获取原始 `callbackMessage`？

- 文件：
  - [TrayRegistration.h](../IslandBar/src/domain/TrayRegistration.h)
  - [TrayHook.cpp](../IslandBar/src/trayhook/TrayHook.cpp)
  - [TrayLearn.cpp](../IslandBar/src/shell/TrayLearn.cpp)
- 已确认逻辑：
  - payload `+0x018` 为 `uCallbackMessage`。
  - 解码后写成 `tray_callbacks.txt` 的 `message` 字段。
  - 按压时从 cache 读取。
- 已确认事实：
  - callback message 可以自动学习，不再依赖人工 Win32k trace。
- 尚未验证的假设：
  - 注册后应用是否会改变 callback message 而只发送 MODIFY。
- 潜在问题：
  - cache 写入与按压读取之间没有统一的原子替换机制。

#### 10. 是否正确处理 `NOTIFYICON_VERSION_4`？

- 文件：
  - [TrayEncoding.h](../IslandBar/src/domain/TrayEncoding.h)
  - [TrayPress.cpp](../IslandBar/src/shell/TrayPress.cpp)
  - [TrayLearn.cpp](../IslandBar/src/shell/TrayLearn.cpp)
- 已确认逻辑：
  - v4 左键使用 packed `NIN_SELECT`。
  - v4 右键使用 packed `WM_CONTEXTMENU`。
  - v0/v3 使用 `wParam = uid`、`lParam = mouse message`。
  - `NIM_SETVERSION` 的值会覆盖 `NIM_ADD` 中与 timeout 共用存储的 `uVersion`。
- 已确认事实：
  - 参数编码集中在一个纯逻辑入口，避免发送端各自拼消息。
- 尚未验证的假设：
  - 应用声称 v4 时是否总接受文档形态。
- 潜在问题：
  - 当前实现覆盖点击和上下文菜单，不覆盖 `NIN_BALLOON*` 等完整通知族。
  - 真实应用可能要求与版本字段不一致的实测配方。

#### 11. 当前左键和右键如何实现？

- 文件：[TrayPress.cpp](../IslandBar/src/shell/TrayPress.cpp)
- 函数：`SendTrayCallback`、`PressTrayIcon`
- 左键：
  - 查找应用 callback HWND。
  - 授予目标进程前台权限。
  - 根据学习到的配方发送 `NIN_SELECT` 或 `WM_LBUTTONDOWN + WM_LBUTTONUP`。
  - 观察应用是否出现新窗口、关闭窗口或切到前台。
  - 必要时尝试激活应用主窗口。
- 右键：
  - v0/v3 优先发送 Shell 实测的右键 down/up pair。
  - 对失败过的图标尝试 `WM_CONTEXTMENU`。
  - v4 直接使用 packed `WM_CONTEXTMENU`。
  - 两种形态都失败后，后续按压不再重复猜测，改由 IslandBar 自己菜单兜底。
- 已确认事实：
  - 右键不是统一的单消息实现，已有按图标记录历史结果。
- 尚未验证的假设：
  - “检测到应用新窗口”是否足以证明所有应用的上下文菜单都已出现。
- 潜在问题：
  - 复用一个已有菜单窗口的应用可能被误判为“没有响应”。
  - 检测窗口需要轮询最多 1.5 秒，增加按压延迟。

#### 12. 是否存在鼠标模拟？

- 文件：[TrayPress.cpp](../IslandBar/src/shell/TrayPress.cpp)
- 已确认事实：
  - 当前生产路径没有 `SendInput`、`SetCursorPos` 或 `mouse_event`。
  - 只读取当前指针位置，用作菜单锚点。
- 尚未验证的假设：
  - 特殊应用是否会拒绝所有消息路线，最终必须使用受控输入模拟。
- 潜在问题：
  - 如果以后恢复鼠标模拟，必须增加前台许可、可中断等待、指针恢复和故障回滚。

#### 13. 是否存在 UI Automation 备用激活？

- 文件：
  - [TrayUia.cpp](../IslandBar/src/shell/TrayUia.cpp)
  - [TrayPress.cpp](../IslandBar/src/shell/TrayPress.cpp)
- 函数：`PressTrayIconWithUia`
- 已确认事实：
  - UIA 路径存在，但默认关闭。
  - `ISLANDBAR_TRAY_UIA=1` 才启用。
  - 只支持左键 Invoke，不支持右键菜单。
- 当前实测：
  - 隐藏任务栏时，`SystemTray_Main` 的 UIA descendants 为 `0`。
- 尚未验证的假设：
  - 令原生任务栏短暂出现后，当前新 build 是否仍能稳定找到全部图标。
- 潜在问题：
  - 显示原生任务栏会造成可见闪烁，与独立显示目标冲突。

#### 14. Explorer 重启后当前代码如何恢复？

- 原生任务栏：
  - [NativeTaskbar.cpp](../IslandBar/src/platform/NativeTaskbar.cpp)
  - `NativeTaskbar::EnsureHidden`
  - 检测旧 HWND 是否失效。
  - `HandleExplorerRestart` 清空窗口句柄和 region。
  - 重新查找新的 `Shell_TrayWnd` 并隐藏。
  - [BarLoop.cpp](../IslandBar/src/app/BarLoop.cpp) 以约 50 帧为周期重新检查。
- 托盘数据：
  - `TickTray` 比较 `TrayShellProcessId()`。
  - 新 PID 出现时调用 `ResetLiveTray()`。
  - 启动新的后台学习 pass。
  - 再次向新 Explorer 进程提供 `island_tray_hook.dll`。
  - 广播 `TaskbarCreated`，使应用重新注册。
- 已确认事实：
  - 代码包含这两条恢复链。
- 尚未验证的假设：
  - 快速连续重启、Explorer 启动很慢、登录时 IslandBar 先启动等情况是否都能恢复。
- 潜在问题：
  - 重新学习最少有 15 秒节流。
  - 复杂环境下恢复时间需要实测。

#### 15. 当前是否存在图标句柄泄漏、重复图标或失效 HWND 问题？

- 已确认事实：
  - 当前不持有托盘 HICON，因此没有 HICON 双重释放问题。
  - Renderer 的 D2D bitmap 在替换和析构时释放。
  - Ledger 有固定 128 条容量上限。
  - 当前日志实测重复数为 `0`。
  - 每轮 tray tick 会调用 `ForgetDeadTrayWindows`。
- 潜在问题：
  - `upgradedIcons_` 没有容量和过期淘汰。
  - wire format 中窗口字段是 32 位，虽然当前 HWND 都能表示，但 Win64 HWND 本身不是 32 位契约。
  - 失效判断只调用 `IsWindow`，没有同时核对 PID，理论上可能接受被复用的 HWND。
  - `ReadTrayCallbacks`、`HarvestReport`、`MarkCallbackAsIgnored`、`MarkCallbackLeftSpelling` 之间存在多 worker 文件读写风险。
  - 当前 cache 文件是原地重写，不是临时文件 + 原子替换。

### 2.3 当前已经具备的能力

- 默认隐藏 `Shell_TrayWnd` 和所有 `Shell_SecondaryTrayWnd`。
- 从注册表显示第三方托盘图标。
- 用实时 ledger 排除历史残留。
- 把多个注册表条目折叠为同一个托盘图标。
- 自动学习 callback HWND 类型、callback message 和 version。
- 发送应用自己的左键/右键回调。
- 通过每应用历史结果选择右键形态。
- 对无法处理的图标打开 IslandBar 自己的兜底菜单。
- Explorer 重启后重新隐藏原生任务栏。
- Explorer 重启后重新安装托盘观察器。
- 按 revision 缓存 PNG 解码结果。

### 2.4 必须修改或验证的部分

| 优先级 | 项目 | 原因 |
|---|---|---|
| P0 | 五应用真实交互矩阵 | 当前没有本轮实测微信、QQ、WeGame、Steam、NVIDIA 的 5/5 左键结果 |
| P0 | Explorer 连续重启测试 | 验证不崩溃、不重复、无永久失效 HWND |
| P0 | 动态图标和动态 tooltip 测试 | 验证 registry snapshot 与观察器数据是否同步 |
| P1 | callback cache 原子写 | 避免学习 worker 与按压 worker 读到半个文件 |
| P1 | callback HWND 与 PID 联动校验 | 避免同 class 多窗口和 HWND 复用 |
| P1 | 双击协议 | 当前没有 `WM_LBUTTONDBLCLK` 产品路径 |
| P1 | Tooltip 数据模型 | 分离稳定名称和最新动态 tooltip |
| P2 | `IsPromoted` 布局策略 | 当前只读取，不决定显示/隐藏 |
| P2 | 多显示器数据模型 | 目前只以主 `Shell_TrayWnd` 作为观察入口 |
| P2 | 文档漂移清理 | 注释中仍有过早失效的 API 和模块描述 |

---

## 3. 交付 B：Windows 技术可行性

### 3.1 `Shell_NotifyIcon` 是什么

[`Shell_NotifyIcon`](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shell_notifyiconw)
是应用向 Windows Shell 注册、更新、删除通知区图标并设置版本的接口。

主要命令：

| 命令 | 含义 |
|---|---|
| `NIM_ADD` | 添加通知区图标 |
| `NIM_MODIFY` | 修改已有图标 |
| `NIM_DELETE` | 删除图标 |
| `NIM_SETFOCUS` | 把焦点还给通知区 |
| `NIM_SETVERSION` | 选择通知消息协议版本 |

它不是“枚举系统中全部托盘图标”的公开 API。

### 3.2 `NOTIFYICONDATA` 的身份

[`NOTIFYICONDATAW`](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ns-shellapi-notifyicondataw)
使用以下两种身份之一：

- `hWnd + uID`
- `guidItem`

当使用 `guidItem` 时，`uID` 被忽略。

关键字段：

| 字段 | 用途 |
|---|---|
| `hWnd` | 接收托盘通知的窗口 |
| `uID` | 应用定义的图标标识 |
| `guidItem` | GUID 形式的图标身份 |
| `uFlags` | 表明字段是否有效，如 `NIF_MESSAGE`、`NIF_ICON`、`NIF_TIP`、`NIF_GUID` |
| `uCallbackMessage` | Shell 用来通知应用的应用定义消息 |
| `hIcon` | 图标句柄 |
| `szTip` | Tooltip |
| `uVersion` | 回调协议版本 |

`hIcon` 只对应用和接收注册的 Shell 有意义。跨进程直接保存另一个进程的 HICON 不是安全稳定的做法。

### 3.3 v0/v3 与 v4 回调格式

v0/v3：

```text
wParam = uid
lParam = mouse message / notification code
```

v4：

```text
wParam = MAKELONG(x, y)
lParam = MAKELONG(notification, uid)
```

任务书要求覆盖：

- `WM_LBUTTONDOWN`
- `WM_LBUTTONUP`
- `WM_RBUTTONDOWN`
- `WM_RBUTTONUP`
- `WM_CONTEXTMENU`
- `NIN_SELECT`
- `NIN_KEYSELECT`
- `NIN_BALLOONSHOW`
- `NIN_BALLOONHIDE`
- `NIN_BALLOONTIMEOUT`
- `NIN_BALLOONUSERCLICK`

当前 IslandBar 只完整实现普通左键、右键和上下文菜单所需的消息编码。键盘选择与 balloon 通知族尚未形成产品路径。

### 3.4 公开 API 能做什么

| API | 能做 | 不能做 |
|---|---|---|
| `Shell_NotifyIcon` | 注册、修改、删除、设置单个图标版本 | 枚举全部图标 |
| `Shell_NotifyIconGetRect` | 按已知身份查询图标矩形 | 枚举、读取图标本体、读取 callback message |
| `GetWindowThreadProcessId` | 从 HWND 解析 PID/线程 | 证明托盘注册仍然有效 |
| `GetIconInfo` | 获取已持有 HICON 的 bitmap mask | 从 Shell 枚举未公开的 HICON |
| `CopyIcon` | 复制句柄 | 跨进程安全读取图标 |
| `DestroyIcon` | 释放本进程拥有的图标 | 释放 Shell 或应用拥有的图标 |
| WinEvent | 接收窗口创建、销毁、前台变化等事件 | 提供托盘注册集合 |
| UI Automation | 浏览可访问性树并调用控件 Pattern | 作为稳定完整托盘枚举 API |

### 3.5 当前项目依赖的非公开部分

1. 发给 `Shell_TrayWnd` 的 `WM_COPYDATA` 注册副本。
2. `WM_COPYDATA` payload 的具体字段偏移和 tag。
3. `HKCU\Control Panel\NotifyIconSettings` 子键与值名。
4. `IconSnapshot` 为 PNG。
5. `UID`、`IconGuid`、`IsPromoted` 的含义。
6. `SystemTray_Main`、`TopLevelWindowForOverflowXamlIsland` 等内部窗口类。
7. Explorer 内的 XAML 托盘结构和 UIA 表现。

这些行为只能标记为“当前 Windows 构建上的实测适配”，不能写成稳定官方 API。

### 3.6 为什么经典 `ToolbarWindow32` 路线不能作为主方案

经典路线大致是：

1. 找到 `Shell_TrayWnd`。
2. 找到 `ToolbarWindow32`。
3. 跨进程读取 `TBBUTTON`。
4. 从内部 `TRAYDATA` 取 HWND、UID 和 callback message。

风险：

- `ToolbarWindow32` 在 Windows 11 新构建上可能不存在。
- 内部结构、对齐和堆布局可能随更新改变。
- 需要跨进程内存读取。
- 不覆盖 XAML 溢出的全部实现。
- 不适合作为唯一正式架构。

本机 `10.0.26320` 实测确认该窗口不存在。

### 3.7 注册表快照与 Shell 内部数据的关系

当前实现把两者组合：

- 注册表负责“怎么画”：PNG、稳定名称、exe 路径和候选身份。
- 观察器负责“现在是否存在”：实时注册集合、callback 信息、version 和最新 tip。

只使用注册表的问题：

- 注册表保留应用历史上出现过的所有图标。
- 进程仍运行不等于图标仍注册。
- 应用退出后可能留下旧记录。

只使用观察器的问题：

- 观察器只看到它启动后发生的注册。
- Explorer 刚重启时需要广播 `TaskbarCreated` 才会重新收齐应用注册。
- payload 没有直接给出可跨进程长期保存的 HICON。

因此“注册表 ∩ 实时 ledger”是当前最合理的组合。

### 3.8 动态图标

建议的数据流：

```text
NIM_MODIFY
    |
    v
Shell 更新内部图标
    |
    +----> Shell 更新 IconSnapshot
    |
    +----> Observer 发布新的 tip / identity / version
    |
    v
TrayProbe 合并同一身份的新旧记录
    |
    v
PNG hash 变化
    |
    v
revision++
    |
    v
Renderer 重解码一次
```

关键限制：

- `NIM_MODIFY` 只证明应用请求更新，不保证 `IconSnapshot` 已经同步完成。
- 同一 HICON 是否被修改并进行 MODIFY，取决于应用行为。
- 头像闪烁可能使用窗口绘制或通知气泡，不一定改变托盘 HICON。
- 必须在微信/QQL/Steam 等真实应用上测量，而不是用一个测试 registrar 推断。

### 3.9 左键交互

可行的分级顺序：

1. 应用自己的 callback message。
2. 正确的 v0/v3 或 v4 参数编码。
3. 同步发送与异步发送两种已实测形态。
4. 授予前台权限并尝试激活应用主窗口。
5. UIA Invoke。
6. 受控输入模拟。
7. IslandBar 自己的菜单兜底。

当前项目已实现 1 到 5 的主要路径。受控输入模拟已从默认路径移除。

关键风险：

- 应用可能把托盘窗口和主窗口放在不同进程。
- 消息可能被自定义过滤或忽略。
- 应用可能要求真实前台点击资格。
- 同一个消息可能既打开菜单又切换状态，因此不能未经判断重试多种形态。

### 3.10 右键菜单

右键是兼容性最差的部分。

当前形态：

- v0/v3：同步发送 `WM_RBUTTONDOWN`、短等待、`WM_RBUTTONUP`。
- 失败后在下一次按压使用 `WM_CONTEXTMENU`。
- v4：使用 packed `WM_CONTEXTMENU`。
- 两种都失败后，IslandBar 打开自己的菜单。

不能保证：

- 菜单窗口一定由应用进程新建。
- 菜单一定可通过窗口出现来检测。
- UIA 一定提供 ContextMenuPattern。
- 自绘菜单一定响应原始鼠标消息。

因此“尽可能显示应用原生菜单”可行，“所有应用必定显示原生菜单”不可行。

### 3.11 Explorer 重启

Explorer 重启后：

1. 旧 `Shell_TrayWnd` 销毁。
2. 旧 Shell 内的模块随进程退出。
3. 旧 ledger 不再代表新 Shell。
4. 应用通常响应 `TaskbarCreated` 并重新注册。
5. IslandBar 必须重新进入新 Explorer 进程。
6. NativeTaskbar 必须重新隐藏新任务栏。

当前代码已经有对应逻辑，但第一阶段原型必须连续验证至少三次。

### 3.12 多显示器

公开行为中，`Shell_SecondaryTrayWnd` 表示其他显示器上的任务栏。

当前项目：

- 会隐藏主任务栏和所有 secondary taskbar。
- 托盘数据观察入口仍主要围绕主 `Shell_TrayWnd`。
- 没有完整的“图标属于哪块显示器”“主托盘/溢出/副托盘”状态模型。

若产品要支持多显示器独立托盘，后续需要增加：

- monitor id
- taskbar/source monitor
- promoted/overflow 状态
- 每个显示器自己的布局和缓存

### 3.13 系统图标

当前 `SystemStatus` 已经独立读取：

- WLAN 接口、SSID、信号强度
- Bluetooth radio
- Core Audio 默认输出音量和静音
- `GetSystemPowerStatus` 电池和 AC 状态
- 电池节能状态
- 前台窗口键盘布局

这是正确方向。网络、音量、蓝牙、电池和输入法不应依赖复制 Explorer 的全部系统托盘菜单。

边界：

- 输入法只能读到语言布局，当前不能稳定读取中文/英文转换模式。
- 电池 API 在台式机上可能成功但返回“未知”。
- WLAN radio 的读写在不同 Windows build 上可能需要不同 API。

---

## 4. 交付 C：最小技术验证原型

### 4.1 原型原则

不立即修改 IslandBar 主程序。

不复制一整套产品代码。原型应复用已有纯逻辑和平台通道：

- `domain/TrayRegistration.h`
- `domain/TrayLedger.*`
- `domain/TrayEncoding.h`
- `platform/TrayRing.h`

原型只负责验证和输出证据。

### 4.2 建议新增文件

```text
IslandBar/
  tools/
    tray_probe/
      CMakeLists.txt
      main.cpp
  scripts/
    tray-probe-matrix.ps1
```

不修改：

- `island_bar.exe` 启动流程
- `app/BarLoop.cpp`
- `render/TextRenderer.cpp`
- 当前默认托盘页

若原型必须使用独立 hook ring，再单独增加：

```text
IslandBar/src/trayhook/CMakeLists.txt
```

中的 probe 目标，或复用现有 hook 但通过编译定义使用独立 ring 名称。不得让原型和正在运行的产品同时竞争同一个 ring cursor。

### 4.3 核心数据结构

```cpp
struct TrayProbeIcon {
    uint64_t generation;
    uint32_t pid;
    uint64_t hwnd;
    uint32_t uid;
    bool hasGuid;
    GUID guid;
    uint32_t callbackMessage;
    uint32_t version;
    std::wstring exe;
    std::wstring label;
    std::wstring tooltip;
    bool promoted;
    std::vector<uint8_t> png;
    uint64_t updatedTick;
};
```

身份键：

1. GUID。
2. `(PID, HWND, UID)`。
3. `(exe, UID)` 加 tooltip，仅作为受控兜底。

不允许仅凭图标 PNG 相同来合并两个图标，因为两个不同应用可能使用同一张状态图。

### 4.4 数据获取层

原型包含三个只读来源：

| 来源 | 用途 |
|---|---|
| Registry adapter | PNG、exe、InitialTooltip、promoted、候选身份 |
| Ring adapter | 实时 ADD/MODIFY/DELETE、callback、version、tip |
| Window validation | 根据 HWND 查询 PID/class，并检查窗口是否仍存在 |

UIA 只作为诊断和按压实验模块，不进入绘制快照。

### 4.5 消息处理流程

```text
Shell_TrayWnd
    |
    | WM_COPYDATA
    v
Probe hook
    |
    | decode
    | fixed-size ring
    v
Probe ring reader
    |
    | identity merge
    v
TrayProbeState
    |
    +--> registry PNG
    |
    +--> current callback info
    |
    +--> visible snapshot
```

每条记录处理顺序：

1. 校验 tag、payload 大小和 command。
2. 校验 GUID 或 `(HWND, UID)`。
3. 检查 HWND 当前存在。
4. `GetWindowThreadProcessId` 查询 owner PID。
5. 更新 identity map。
6. 对 DELETE 移除记录。
7. 对 ADD/MODIFY 更新非空字段。
8. 只在内容变化时生成新 revision。

### 4.6 图标缓存方案

```cpp
struct IconCacheEntry {
    uint64_t pngHash;
    std::vector<uint8_t> png;
    uint64_t lastUsedTick;
};
```

规则：

- 以 PNG 内容 hash 作为位图缓存键。
- 同一个 hash 不重复解码。
- cache 设固定容量，例如 128。
- 超过容量时淘汰最久未使用项。
- 不允许每帧读取 registry。
- 不允许每帧调用跨进程窗口查询。

### 4.7 线程模型

| 线程 | 职责 |
|---|---|
| UI thread | 消息循环、快照读取、绘制、命中测试 |
| ring thread | 等待 named event，批量 drain |
| registry thread | 监听注册表变化，按需读取快照 |
| press worker | 有界发送 callback、观察响应、超时可取消 |

约束：

- UI thread 不等待 ring 写满。
- UI thread 不等待另一个应用消息处理完成。
- press worker 单次发送最多 1.5 秒。
- 所有 worker 使用 `std::atomic<bool>` 或现有 `WorkerThread` 停止协议。

### 4.8 原型测试计划

#### 测试一：静态格式断言

- known payload 能解码。
- 错误 tag 被拒绝。
- 过短 payload 被拒绝。
- GUID、UID、callback、version 和 tip 位置正确。
- v0/v3/v4 编码结果符合断言。

#### 测试二：真实图标发现

至少覆盖：

- 微信
- QQ
- WeGame
- Steam
- NVIDIA
- OneDrive
- Windows 安全中心
- 输入法/系统状态

连续采样十次，记录：

- registry entries
- live registrations
- visible icons
- duplicate count
- stale count

#### 测试三：动态更新

- QQ 音乐切换歌曲，观察 tooltip 变化。
- WeGame 状态变化，观察 tip 变化。
- 微信头像闪烁，观察 PNG 是否变化。
- Task Manager 刷新 tooltip。
- 应用撤销图标后，确认 ledger 删除。

记录从 MODIFY 到新图可绘制的延迟。

#### 测试四：左键

每个应用验证：

- 窗口最小化时能否恢复。
- 已在后台时能否到前台。
- 已在前台时是否被误判。
- 消息是否只执行一次。
- 应用退出后是否显示“窗口不存在”而不是旧 HWND。

#### 测试五：右键

每个图标记录：

- 发送的 shape
- 是否出现原生菜单
- 菜单位置
- 菜单关闭后焦点
- 是否阻塞消息循环
- 失败后是否打开 IslandBar 菜单

#### 测试六：Explorer 重启

连续执行三次：

- 杀掉并重启 Explorer。
- 记录新 PID。
- 记录 observer ready 时间。
- 记录图标重新出现时间。
- 检查重复图标。
- 检查旧 HWND。
- 检查原生任务栏是否再次隐藏。

#### 测试七：隐藏原生任务栏

在 `Shell_TrayWnd` 隐藏状态下验证：

- IslandBar 仍显示图标。
- 左键仍能操作应用。
- 右键仍能显示应用菜单。
- UIA 不可用时行为正确降级。
- 系统状态图标不依赖原生托盘 UI。

### 4.9 验收标准

第一阶段原型建议使用以下门槛：

| 指标 | 门槛 |
|---|---|
| 图标发现一致性 | 连续 10 次 `visible == live` |
| 重复图标 | `duplicates=0` |
| 失效窗口 | `stale=0` |
| 动态图标延迟 | P95 不超过 1500 ms |
| 五应用左键 | 至少 5/5 得到明确可用结果 |
| 原生右键菜单 | 至少 4/5 显示真实应用菜单 |
| Explorer 重启 | 连续 3/3 在 15 秒内恢复 |
| 空闲额外 CPU | 平均不超过 0.5% |
| 10 分钟内存增长 | 不超过 1 MiB |

必须包含以下固定日志：

```text
tray-probe: registry=29 live=9 visible=9 duplicates=0 stale=0
tray-probe: modify-to-paint p95=####ms samples=##
tray-probe: left native=5/5 right-native=4/5 fallback=1
tray-probe: explorer-restart ready_ms=#### icons=##
```

其中主验收数字是：

```text
duplicates=0
```

该数字必须在连续十次完整采样中成立。

### 4.10 回退方式

原型只新增独立工具和脚本，不进入 IslandBar 启动链。

失败时：

1. 停止并删除 `tools/tray_probe`。
2. 删除 `scripts/tray-probe-matrix.ps1`。
3. 不修改主程序配置。
4. 不迁移用户数据。
5. 不需要回退 registry、cache 或任务栏状态。

如果使用独立 probe ring，则删除独立 ring 名称和 probe hook 目标即可。

### 4.11 预计影响

CPU：

- 正常空闲由 registry/ring 事件驱动。
- 5 秒 fallback 只做增量检查。
- 不做每帧跨进程查询。
- 学习 pass 是一次性的启动尖峰，不在 UI 线程执行。

内存：

- ledger 固定上限。
- ring 固定大小。
- PNG cache 需要容量上限。
- D2D bitmap 每个图标只保留一份。

观感：

- 原型不改变 IslandBar UI。
- UIA 只按需开启。
- 默认不显示原生任务栏。
- 后续主项目集成仍保留 Liquid Glass 托盘页。

---

## 5. 建议的生产架构

不建议为了“看起来分层”而新建大量模块。当前命名已经接近目标，应优先收紧职责：

```text
TrayHook / TrayRing
        |
        v
TrayLearn + TrayLedger
        |
        +----------+
        |          |
        v          v
TrayIcons      TrayPress / TrayUia
        |          |
        +----+-----+
             |
             v
CardLayout + TextRenderer + IslandWindow
```

建议职责：

| 模块 | 建议 |
|---|---|
| `TrayHook` | 只做 payload 观测、解码、ring 发布；不解析 UI、不写业务决策 |
| `TrayRing` | 固定 ABI、固定容量、可拒绝不匹配版本 |
| `TrayLearn` | 安装/重装观察器、广播注册、合并 cache |
| `TrayLedger` | 纯逻辑实时注册集合 |
| `TrayIcons` | 保持注册表 adapter 与绘制快照职责，不增加 Explorer 注入逻辑 |
| `TrayPress` | 只负责点击协议和响应判定 |
| `TrayUia` | 保持显式 opt-in，不成为默认绘制依赖 |
| `TrayMenu` | 只做 IslandBar 兜底菜单 |
| `CardLayout` | 只做布局和命中测试 |
| `TextRenderer` | 只消费不可变图标快照和 revision |

依赖方向必须保持：

```text
domain -> core -> platform -> media -> services -> shell -> render -> app -> cli
```

`domain/TrayRegistration.h`、`TrayLedger` 和 `TrayEncoding.h` 必须继续不包含 Win32。

---

## 6. 风险清单

| 风险 | 可能性 | 影响 | 应对 |
|---|---:|---|---|
| Windows build 改变 WM_COPYDATA payload | 中 | 观察器失明 | tag/size 校验、ABI 拒绝、日志和 registry fallback |
| registry schema 改变 | 中 | 无法绘制快照 | adapter 隔离；失败时显示空列表而不是伪造图标 |
| 应用不响应 `TaskbarCreated` | 中 | Explorer 重启后少图标 | 保留 registry fallback，明确标注不完整 |
| 右键菜单检测误判 | 高 | 打开错误菜单或错误 fallback | 记录 shape、窗口证据、失败缓存 |
| 动态图标 registry 更新延迟 | 中 | 图标闪烁滞后 | P95 测量；必要时增加 MODIFY 后的小延迟重读 |
| HWND 复用 | 低到中 | 使用失效 callback 窗口 | 同时校验 HWND、PID 和窗口类 |
| cache 并发写损坏 | 中 | 一次按压暂时失败 | 单 writer 或 temp + atomic replace |
| 高分辨率/DPI 图标不一致 | 中 | 清晰度差 | icon hash + DPI source side 缓存 |
| 多显示器状态丢失 | 中 | 图标顺序/归属错误 | 增加 monitor/tray source 字段 |
| 注入导致 Explorer 不稳定 | 低到中 | Shell 崩溃 | 只进 Shell 单进程、PIN、DllMain 最小化、不全局注入 |

---

## 7. 交付 D：最终决策

### 7.1 该方案是否可行？

可行。

更准确的定义是：

> 在不重写 Shell、不替换 Explorer、不依赖第三方 UI 框架的前提下，IslandBar 可以实现高覆盖的独立托盘显示和主要交互。

但不能承诺：

- 所有应用的原生右键菜单。
- 所有应用的动态头像实时一致。
- 所有 Windows build 的内部结构不变。
- 所有提权、受保护、自绘和异常应用都支持。

### 7.2 哪些功能可以通过公开 API 实现？

- 单个托盘图标的注册、修改、删除和版本设置。
- 按已知身份获取图标矩形。
- 通过 UIA Invoke 操作支持 InvokePattern 的图标。
- 获取窗口所属进程。
- 复制本进程拥有的 HICON。
- 网络、音量、蓝牙、电池和键盘布局等系统状态。
- 通过 WinEvent 接收部分窗口生命周期事件。

### 7.3 哪些功能必须依赖 Explorer 内部机制？

- 获取全量实时托盘注册。
- 获取原始 callback HWND、callback message 和 version。
- 获取 Shell 存储的 PNG 图标快照。
- 识别当前 XAML 托盘和溢出窗口。
- 区分 promoted 和 overflow。
- 对不可学习图标请求 Shell 自己 Invoke。

### 7.4 哪些功能可能无法完整实现？

- 100% 第三方应用的右键原生菜单。
- 双击兼容。
- 所有动态 HICON 与 hover tooltip 的像素级同步。
- 不被观察器捕获的历史注册。
- 自保护、提权和异常框架的完整自动化。
- 多显示器下完整恢复每个原生托盘状态。
- Windows 未来版本改变内部布局后的无缝兼容。

### 7.5 是否值得集成进 IslandBar？

值得保留和继续完善。

理由：

- 当前代码已经在真实桌面达到 9/9 图标和 0 重复。
- 注册表 + ledger 的组合有明确数据职责。
- callback 自动学习显著减少人工维护。
- 当前默认隐藏原生任务栏，符合产品方向。

但不建议在完成原型矩阵前继续扩大主项目改动。

### 7.6 是否存在比当前方案更稳定的技术路线？

没有一条路线能够同时满足：

- 原生任务栏托盘不显示。
- IslandBar 自己绘制全部图标。
- 原生应用行为兼容。
- 只依赖公开 API。

可选路线比较：

| 路线 | 优点 | 缺点 | 结论 |
|---|---|---|---|
| 只读注册表 | 简单、无注入 | 历史残留、动态信息不足 | 只能作为绘制来源 |
| 经典 ToolbarWindow32 | 旧系统数据完整 | 新系统不存在、私有内存 | 淘汰 |
| 全量注入所有应用 | 可截获应用调用 | 签名、沙箱、黑屏和崩溃风险 | 淘汰 |
| 只进 Explorer 观察 WM_COPYDATA | 覆盖面好、应用不感知 | 私有格式、Explorer 崩溃风险 | 当前主路线 |
| UI Automation | 可让 Shell 执行操作 | 不能绘制；隐藏时不可用；无右键 Pattern | 仅按压兜底 |
| Registry + Shell observer + optional UIA | 覆盖面与产品体验最平衡 | 多数据源、维护复杂 | 推荐 |

---

## 8. 下一步执行方案

本轮已经完成研究，不修改主项目。

用户确认后，下一轮只做独立原型：

### 修改文件

新增：

- `IslandBar/tools/tray_probe/CMakeLists.txt`
- `IslandBar/tools/tray_probe/main.cpp`
- `IslandBar/scripts/tray-probe-matrix.ps1`

可能按需新增独立 probe hook 目标，但不接入主程序。

不修改：

- `IslandBar/src/app/BarLoop.cpp`
- `IslandBar/src/app/IslandWindow.cpp`
- `IslandBar/src/render/TextRenderer.cpp`
- `IslandBar/src/shell/TrayPress.cpp`
- `IslandBar/src/trayhook/TrayHook.cpp`

### 为什么这样做

- 先证明数据发现、动态更新、交互和 Explorer 恢复。
- 避免在未验证前继续扩张主项目。
- 复用现有纯逻辑和共享内存格式。
- 保持主项目 Debug/Release 和现有托盘行为不变。

### 验收

最小验收数字：

```text
duplicates=0
```

并必须在十次连续采样中成立。

固定日志：

```text
tray-probe: registry=29 live=9 visible=9 duplicates=0 stale=0
```

### 回退

删除新增的 probe 工具和脚本即可；主项目无需恢复数据或配置。

### 预计影响

- CPU：事件驱动，空闲平均额外占用目标不超过 0.5%。
- 内存：固定 ring、固定 ledger、容量受限的 PNG cache；10 分钟增长不超过 1 MiB。
- 观感：原型不改变 IslandBar；生产集成继续保留 Liquid Glass 托盘页。

---

## 9. 当前运行证据

### 9.1 本机窗口探测

```text
Shell_TrayWnd=131382
visible=False
pid=11992
rect=0,0-2560,32

TrayDummySearchControl
TrayNotifyWnd
ReBarWindow32
MSTaskSwWClass
MSTaskListWClass
```

未发现：

```text
ToolbarWindow32
```

### 9.2 UIA

隐藏任务栏状态下：

```text
SystemTray_Main
name=电池指示器
class=SystemTray_Main
type=ControlType.Pane
descendants=0
```

该结果证明 UIA 不能作为隐藏任务栏时的绘制数据源。

### 9.3 当前托盘日志

运行日志中确认：

```text
native taskbar: hidden
tray hook: the observer is up in the shell
tray learn: 4 registration(s) captured
tray: 4 icon(s) -- 4 matched ...; 0 duplicate(s) collapsed
tray: 5 icon(s) -- 5 matched ...; 0 duplicate(s) collapsed
tray learn: 1 registration(s) arrived ... -- 6 of 6 icon(s) have a callback entry now
tray: 7 icon(s) -- 7 matched ...; 0 duplicate(s) collapsed
tray: 8 icon(s) -- 8 matched ...; 0 duplicate(s) collapsed
tray: 9 icon(s) -- 9 matched ...; 0 duplicate(s) collapsed
```

这证明：

- 观察器实际工作。
- ledger 会随应用注册增长。
- 注册表记录与实际 Shell 注册能够配对。
- 当前样本没有重复单元格。

### 9.4 当前 callback cache

当前缓存覆盖了包括 ChatGPT、WeGame、Clash Verge、QQ 音乐、Steam、微信、Explorer Bluetooth、Windows 安全中心等应用。

这只能证明注册信息可观察到，不能单独证明每个应用的左键和右键都正常。

---

## 10. 官方资料

- [Shell_NotifyIconW](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shell_notifyiconw)
- [NOTIFYICONDATAW](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ns-shellapi-notifyicondataw)
- [Shell_NotifyIconGetRect](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shell_notifyicongetrect)
- [NOTIFYICONIDENTIFIER](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ns-shellapi-notifyiconidentifier)
- [UI Automation](https://learn.microsoft.com/en-us/windows/win32/winauto/entry-uiauto-win32)

---

## 11. 外部交叉评审状态

仓库中已有 ChatGPT 与 Gemini 的历史评审记录，目录包括：

- `reviews/review-a32-chatgpt.txt`
- `reviews/review-a32-gemini.txt`
- `reviews/review-a33-chatgpt.txt`
- `reviews/review-a33-gemini.txt`
- `reviews/review-q33.txt`
- `reviews/review-q36.txt`
- `reviews/review-q37.txt`
- `reviews/review-q39.txt`
- `reviews/review-q40.txt`

本轮尝试再次打开现有 ChatGPT/Gemini 页面进行交叉确认时，Computer Use 因无法验证当前浏览器 URL 安全状态而终止。

没有向 ChatGPT 或 Gemini 提交新的外部消息，也没有把本机日志、用户数据或源码发送给外部服务。

本报告中的当前实现结论以本地源码、当前运行日志、当前窗口树探测和上述仓库内历史评审为依据。

---

## 12. 最终判断

推荐继续推进，但严格分阶段：

1. 保持当前生产实现不变。
2. 先建立独立 `tray_probe`。
3. 用真实应用完成发现、动态、交互和 Explorer 重启矩阵。
4. 在 `duplicates=0`、五应用左键和 Explorer 恢复门槛通过后，再决定主项目修改。
5. 不在缺少实测的情况下新增大量数据源抽象或恢复全局注入。

当前路线的核心原则应保持不变：

> 独立显示，兼容原生应用行为；不复制原生托盘界面，不盲目替换 Windows Shell。
