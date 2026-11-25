# Tray Phase 0：当前仓库核验报告

## 1. 核验信息

| 项目 | 结果 |
|---|---|
| 核验时间 | 2026-11-25 |
| 当前分支 | `main` |
| 当前 HEAD | `51d7df24e0e4baef18535866a3e94636e91e219f` |
| 任务书审计基线 | `51d7df24e0e4baef18535866a3e94636e91e219f` |
| HEAD 是否一致 | 是 |
| 当前系统 | Windows `10.0.26320` |
| C++ 标准 | C++20 |
| 编译器约束 | MSVC `/W4`、`/WX`、`/permissive-`、UTF-8 |
| CRT | 静态 CRT：Debug `/MTd`，Release `/MT` |

当前未跟踪项：

```text
$null
docs/
美化方案.md
```

本阶段不修改生产代码，也不处理上述未跟踪项。

## 2. 当前构建与测试入口

项目实际入口：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ".\IslandBar\scripts\build.ps1" -Config Debug
powershell -NoProfile -ExecutionPolicy Bypass -File ".\IslandBar\scripts\build.ps1" -Config Release
.\IslandBar\build\Debug\src\Debug\island_bar.exe --self-test
.\IslandBar\build\Release\src\Release\island_bar.exe --self-test
.\IslandBar\build\Release\tools\smoke\Release\island_smoke.exe
```

构建脚本每次都会运行：

```text
scripts/check-layers.ps1
```

架构依赖方向仍为：

```text
domain -> core -> platform -> media -> services -> shell -> render -> app -> cli
```

`island_domain` 仍不允许包含 `<windows.h>`，也不链接系统库。

## 3. 模块存在性核验

以下任务书列出的模块全部存在：

| 模块 | 当前状态 |
|---|---|
| `src/shell/TrayIcons.cpp/.h` | 存在 |
| `src/shell/TrayLearn.cpp/.h` | 存在 |
| `src/shell/TrayPress.cpp/.h` | 存在 |
| `src/shell/TrayUia.cpp/.h` | 存在 |
| `src/shell/TrayMenu.cpp/.h` | 存在 |
| `src/domain/TrayLedger.cpp/.h` | 存在 |
| `src/domain/TrayEncoding.h` | 存在 |
| `src/domain/TrayRegistration.h` | 存在 |
| `src/platform/TrayRing.h` | 存在 |
| `src/trayhook/TrayHook.cpp` | 存在 |
| `src/platform/NativeTaskbar.cpp/.h` | 存在 |
| `src/app/BarLoop.cpp` | 存在 |
| `src/render/TextRenderer.cpp` | 存在 |
| `src/shell/SystemStatus.cpp/.h` | 存在 |

没有发现模块被重命名或删除。

## 4. 与基线一致的实现事实

### 4.1 图标来源

[TrayIcons.cpp](../../IslandBar/src/shell/TrayIcons.cpp) 的 `TrayIcons::Refresh` 仍执行以下流程：

1. 打开 `HKCU\Control Panel\NotifyIconSettings`。
2. 枚举注册表子键。
3. 读取 `ExecutablePath`、`IconSnapshot`、`InitialTooltip`、`IsPromoted`、`UID`、`IconGuid`。
4. 只接受有效 PNG。
5. 根据 live ledger 与 registry identity 合并。
6. 将多个 registry entry 折叠为一个 Shell 注册。
7. 仅在内容变化时递增 revision。

结论：**仍成立。**

### 4.2 Explorer 观察器

[TrayHook.cpp](../../IslandBar/src/trayhook/TrayHook.cpp) 仍通过 `WH_CALLWNDPROC` 观察发给 `Shell_TrayWnd` 的 `WM_COPYDATA`。

[TrayRegistration.h](../../IslandBar/src/domain/TrayRegistration.h) 仍使用实测 tag、payload 最小长度和固定字段偏移解码：

```text
+0x004 command
+0x00C tray window
+0x010 uid
+0x014 flags
+0x018 callback message
+0x020 tooltip
+0x328 version
+0x3B0 guid
```

结论：**仍成立。**

### 4.3 共享内存 ring

[TrayRing.h](../../IslandBar/src/platform/TrayRing.h) 仍满足：

- 固定容量 64。
- ABI 版本 2。
- 固定 `TrayRingRecord` 布局。
- registration 与 hook state 使用 kind 区分。
- ABI、magic、record size 不匹配时拒绝消费。

结论：**仍成立。**

### 4.4 实时 ledger

[TrayLedger.cpp](../../IslandBar/src/domain/TrayLedger.cpp) 仍维护：

- GUID 或 `(window, uid)` 身份。
- ADD/MODIFY 更新。
- DELETE 删除。
- 窗口死亡删除。
- 固定容量 128。

结论：**仍成立。**

### 4.5 点击协议

[TrayEncoding.h](../../IslandBar/src/domain/TrayEncoding.h) 仍是 v0/v3/v4 参数编码的唯一入口。

[TrayPress.cpp](../../IslandBar/src/shell/TrayPress.cpp) 仍支持：

- 应用自己的 callback message。
- v0/v3 左键 down/up。
- v4 `NIN_SELECT`。
- v0/v3 右键 pair。
- v4 `WM_CONTEXTMENU`。
- 失败后 IslandBar 菜单兜底。

结论：**仍成立。**

### 4.6 UIA

[TrayUia.cpp](../../IslandBar/src/shell/TrayUia.cpp) 仍是可选左键 fallback：

- 默认关闭。
- `ISLANDBAR_TRAY_UIA=1` 才开启。
- 不作为绘制来源。
- 不支持右键菜单。

结论：**仍成立。**

### 4.7 Explorer 重启恢复

[NativeTaskbar.cpp](../../IslandBar/src/platform/NativeTaskbar.cpp) 仍会在旧 `Shell_TrayWnd` 失效时重新定位。

[BarLoop.cpp](../../IslandBar/src/app/BarLoop.cpp) 的 `TickTray` 仍会在 Explorer PID 改变时：

1. 重置 live tray。
2. 启动后台学习 pass。
3. 向新 shell 安装 observer。
4. 广播 `TaskbarCreated`。

结论：**代码路径仍成立，真实连续重启尚未验证。**

## 5. 任务书列出的缺陷仍成立

### P0：验证缺失

| 缺陷 | 当前状态 |
|---|---|
| 五应用完整交互矩阵 | `NOT TESTED` |
| 连续三次 Explorer 重启 | `NOT TESTED` |
| 动态图标和 Tooltip | `NOT TESTED` |
| 隐藏任务栏下的完整端到端交互 | 部分证据存在，矩阵未完成 |

### P1：数据正确性和并发安全

| 缺陷 | 当前状态 |
|---|---|
| callback cache 多 worker 竞争 | 仍存在 |
| callback cache 原地重写 | 仍存在 |
| cache 只保存 window class，不保存精确 HWND | 仍存在 |
| `IsWindow` 未与 PID 联合校验 | 仍存在 |
| wire window 字段为 32 位 | 仍存在 |
| stable label 与 current tooltip 未完全分离 | 仍存在 |
| 双击产品路径缺失 | 仍存在 |
| balloon/keyboard notification 未完整实现 | 仍存在 |

### P2：容量与完整性

| 缺陷 | 当前状态 |
|---|---|
| `upgradedIcons_` 没有淘汰 | 仍存在 |
| `IsPromoted` 未参与布局策略 | 仍存在 |
| 多显示器托盘模型不完整 | 仍存在 |
| 注释与实现漂移 | 仍存在 |
| 右键响应检测可能误判 | 仍存在 |
| 右键等待可能带来看得见的延迟 | 仍存在 |
| 高频 registry 更新成本未测量 | 仍存在 |

## 6. 当前运行环境证据

任务书记录的当前环境仍可复现：

- `Shell_TrayWnd` 存在但隐藏。
- `TrayNotifyWnd` 存在。
- `ToolbarWindow32` 不存在。
- 隐藏任务栏时 `SystemTray_Main` 的 UIA descendants 为 `0`。
- 当前日志可以观察到 observer 启动和 ledger 增长。
- 当前日志最终达到：

```text
tray: 9 icon(s) -- 9 matched of 29 registry entr(ies)
       (the shell holds 10 registration(s));
       0 duplicate(s) collapsed
```

这些结果只证明发现链在当前样本下工作，不代表五个应用的交互矩阵已经通过。

## 7. Phase 0 结论

### 仍成立

- 混合数据路线。
- Explorer 内观察器。
- registry + ledger merge。
- v0/v3/v4 集中编码。
- 默认无鼠标模拟。
- UIA 默认关闭。
- Explorer 重启恢复代码路径。

### 已修复

无。当前 HEAD 与任务书基线一致。

### 已变化

无实现级变化。

### 新增问题

本轮确认当前未跟踪项中存在任务书之外的文件，但不属于托盘实现，不纳入本任务。

### 可以进入下一阶段

可以。

下一阶段只新增独立 `tray_probe` 和测试脚本，不修改生产托盘行为。
