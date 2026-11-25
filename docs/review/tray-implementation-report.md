# Tray 生产整改与验证报告

## 0. 基线与环境

| 项目 | 值 |
|---|---|
| 源码基线 | `51d7df24e0e4baef18535866a3e94636e91e219f` |
| 操作系统 | Windows `10.0.26320` |
| 架构 | x64 |
| CPU 逻辑处理器 | 12 |
| IslanderBar 最终部署 | `%USERPROFILE%\IslandBar` |
| 最终 probe 时间 | 2026-11-25 19:57:10 |

本轮按 Phase 0-6 顺序执行。Phase 0 报告见
[tray-phase0-current-state.md](tray-phase0-current-state.md)。

## 1. 交付状态总表

| 类别 | 结果 | 证据 |
|---|---|---|
| Debug 构建 | PASS | `/WX` 下构建成功 |
| Release 构建 | PASS | `/WX` 下构建成功 |
| `--self-test` Debug | PASS | 退出码 0 |
| `--self-test` Release | PASS | 退出码 0 |
| `island_smoke` Debug | PASS | `RESULT: PASS` |
| `island_smoke` Release | PASS | `RESULT: PASS` |
| callback cache 并发/原子写 | PASS | `tray_cache_test` |
| cache v1 -> v2 迁移 | PASS | `tray_cache_test` |
| 64 位窗口与边界解码 | PASS | `tray_protocol_test` |
| ABI 不匹配安全拒绝 | PASS | ring header/record size checks |
| ring 并发撕裂测试 | PASS | `tray_protocol_test` |
| 独立 probe 连续 10 次 | PASS | `visible=9`、`duplicates=0`、`stale=0` |
| ABI 5 Explorer 重启 | PASS | 3/3，最慢 14.71 秒 |
| 左键应用矩阵 | 部分 PASS | 6 个明确结果，5 个应用可形成测试门槛 |
| 右键原生菜单矩阵 | 部分 PASS | 6 个中 5 个 `probable` |
| 动态 Tooltip | PASS（单样本） | observer 发布到可见变化 `360 ms` |
| 动态 PNG/头像 | NOT TESTED | 微信停在登录态 |
| 双击 | NOT TESTED | 没有足够应用证据，按任务书保持 unknown |
| 多显示器独立托盘 | NOT TESTED | 仍为已知边界 |
| 10 分钟 CPU 门槛 | FAIL | `2.021% > 0.5%` |
| 10 分钟内存增长门槛 | FAIL | `2312 KB > 1024 KB` |

## 2. 源码变更

### 2.1 独立 probe

新增：

```text
IslandBar/tools/tray_probe/main.cpp
IslandBar/tools/tray_probe/cache_test.cpp
IslandBar/tools/tray_probe/protocol_test.cpp
IslandBar/tools/tray_probe/CMakeLists.txt
IslandBar/scripts/tray-probe-matrix.ps1
```

能力：

- 独立 probe ring，不与产品共享 cursor。
- observer-only hook，不接管运行中产品的 `Shell_NotifyIcon` 导入槽。
- 输出 registry/live/visible/duplicates/stale。
- 输出精确 identity、PID、HWND、callback、version、稳定标签、动态 Tooltip、PNG hash。
- 记录 publish tick 并计算 modify-to-visible 延迟。
- 连续采样脚本将 `duplicates != 0`、`stale != 0` 或 P95 > 1500 ms 判为失败。

### 2.2 Callback cache v2

新增：

```text
IslandBar/src/shell/TrayCallbackCache.h
IslandBar/src/shell/TrayCallbackCache.cpp
```

改动：

- 所有读取和更新经过进程内单锁。
- 更新采用读取 -> 修改 -> 同目录临时文件 -> flush -> atomic replace。
- 写失败时保留完整旧文件。
- UTF-8、大小、NUL 和 schema 校验。
- 自动读取 v1，下一次更新迁移到 v2。
- v2 记录 `hwnd`、`pid`、`tid`、`shell_pid`。

原始测试：

- [tray_cache_test.out](tray-logs/tray_cache_test.out)
- [tray_cache_test_debug.out](tray-logs/tray_cache_test_debug.out)

### 2.3 精确 callback 绑定

[TrayPress.cpp](../../IslandBar/src/shell/TrayPress.cpp) 现在：

- 优先使用 cache v2 的精确 HWND。
- 校验 `IsWindow`、owner PID、owner thread、window class 和 Shell PID。
- ledger 已 seed 时，还要求身份仍在 live ledger。
- legacy cache 只有在同 exe/class 窗口唯一时才允许低置信度查找。
- 失败原因分别为：
  - `callback_hwnd_missing`
  - `callback_pid_mismatch`
  - `callback_thread_mismatch`
  - `callback_generation_stale`
  - `callback_class_mismatch`
  - `callback_identity_ambiguous`
  - `callback_identity_not_live`

### 2.4 ABI 5

[TrayRing.h](../../IslandBar/src/platform/TrayRing.h)：

- `TrayRegistration::trayWindow` 解码为 `uint64_t`。
- `TrayRingRecord` 增加 `publishedTick`。
- 每个槽位增加 `publishSequence`。
- 写入顺序为 odd sequence -> record -> memory barrier -> even sequence。
- reader 在复制前后校验 sequence，只返回稳定记录。
- ABI 从 2 升到 5，ring/event 名称同步变为 v5。

原始测试：

- `tray_protocol_test`：最大 32 位 wire HWND、GUID/tip/version、错误 tag、短 payload、未知 command、并发撕裂。

### 2.5 Tooltip 与 PNG hash

[TrayIcons.cpp](../../IslandBar/src/shell/TrayIcons.cpp)：

- `TrayIcon::name`：稳定单元格标签。
- `TrayIcon::tooltip`：最新实时 Tooltip。
- `TrayIcon::pngHash`：PNG 内容 hash。

[TextRenderer.cpp](../../IslandBar/src/render/TextRenderer.cpp)：

- D2D bitmap 按 PNG hash + size 缓存。
- Tooltip-only revision 不再重新解码图标。
- 托盘页 hover 时在卡片内绘制当前 Tooltip，限制在卡片边界内。

### 2.6 点击可靠性

- 左键优先应用 callback；callback 不可用时才使用 activate/launch。
- 每次按压只发送一种协议 shape。
- 不再把“没有观察到动作”永久写成 `none`。
- 新旧 cache 中的 `none/click` 不再阻止后续真实尝试。
- 新结果 `SentUnconfirmed` 表示消息已送达但未证实动作。
- 成功左键通过 worker-to-UI 请求收起托盘页。
- 明确失败才请求 IslandBar 菜单，避免菜单叠加。
- 右键 native-menu evidence 分类为 `confirmed / probable / unknown / failed`。
- 当前检测器从不会声称 `confirmed`，最多 `probable`。

## 3. Probe 基线

最终 ABI 5 probe 连续 10 次：

```text
tray-probe: registry=29 live=10 visible=9 duplicates=0 stale=0
```

原始日志：

- [tray-probe-release-20261125-195710.log](tray-probe-release-20261125-195710.log)

历史阶段日志：

- [tray-probe-release-20261125-193402.log](tray-probe-release-20261125-193402.log)
- [tray-probe-release-20261125-194553.log](tray-probe-release-20261125-194553.log)
- [tray-probe-release-20261125-195035.log](tray-probe-release-20261125-195035.log)

主要样本：

| 应用 | PID | callback HWND | callback | version | 备注 |
|---|---:|---|---:|---:|---|
| ChatGPT | 18616 | `0x7083C` | `0x8001` | 0 | GUID |
| QQ 音乐 | 3716 | `0x10828` | `0x0BD0` | 0 | UID 256 |
| WeGame | 11032 | `0x20716` | `0x0922` | 0 | UID 0 |
| Clash Verge | 12340 | `0x2087A` | `0x1772` | 0 | UID 2 |
| Steam | 10992 | `0x105FE` | `0x0401` | 0 | UID 1 |
| Wallpaper Engine | 11472 | 动态 | `0x0400` | 4 | UID 1000 |
| Windows 安全中心 | 20300 | `0x10442` | 未在本次广播捕获 | 4 | GUID |
| Explorer 蓝牙 | Explorer PID | 动态 | `0x0403` | 0 | GUID |

`callback=0x0000` 只表示本轮广播没有捕获到可用 callback，不表示图标不存在。

## 4. 应用交互矩阵

### 4.1 左键

| 应用 | 结果 | 证据 |
|---|---|---|
| ChatGPT | PASS | 应用 callback 被确认，1578 ms |
| Clash Verge | PASS | 应用 callback 被确认，46 ms |
| QQ 音乐 | PASS | 应用 callback 被确认，125 ms |
| Wallpaper Engine | PARTIAL | 消息已送达，1500 ms 内未观察到动作，结果 `SentUnconfirmed` |
| Steam | PARTIAL | 消息已送达，1500 ms 内未观察到动作，结果 `SentUnconfirmed` |
| WeGame | PARTIAL | 消息已送达，1500 ms 内未观察到动作，结果 `SentUnconfirmed` |
| 微信 | BLOCKED | 当前停在登录窗口，未注册可操作托盘 |
| NVIDIA | BLOCKED for callback | 没有可学习 callback；产品走 activate/launch 或菜单 fallback |

左键原始日志：

- [tray_press_chatgpt.out](tray-logs/tray_press_chatgpt.out)
- [tray_press_2.out](tray-logs/tray_press_2.out)
- [tray_press_3.out](tray-logs/tray_press_3.out)
- [tray_press_4.out](tray-logs/tray_press_4.out)
- [tray_press_5.out](tray-logs/tray_press_5.out)
- [tray_press_6.out](tray-logs/tray_press_6.out)
- [tray_press_abi5.out](tray-logs/tray_press_abi5.out)

### 4.2 右键

| 应用 | evidence | 结果 |
|---|---|---|
| ChatGPT | probable | 出现 `Chrome_WidgetWin_1`，47 ms |
| Wallpaper Engine | probable | foreground 进入 `WPETrayWindow`，62 ms |
| Clash Verge | probable | 出现 `#32768` 菜单，1547 ms |
| WeGame | probable | 出现 `TWINCONTROL`，93 ms |
| QQ 音乐 | probable | 出现 `TXGuiFoundation`，109 ms |
| Steam | unknown | 消息送达但 1500 ms 内未看到菜单 |

结果：5/6 有 probable 原生菜单证据，达到至少 4/5 的阶段目标；没有声称 confirmed。

原始日志：

- [tray_rpress_0.out](tray-logs/tray_rpress_0.out)
- [tray_rpress_3.out](tray-logs/tray_rpress_3.out)
- [tray_rpress_4.out](tray-logs/tray_rpress_4.out)
- [tray_rpress_5.out](tray-logs/tray_rpress_5.out)
- [tray_rpress_7.out](tray-logs/tray_rpress_7.out)
- [tray_rpress_8.out](tray-logs/tray_rpress_8.out)
- [tray_rpress_abi5.out](tray-logs/tray_rpress_abi5.out)

## 5. 动态更新

通过系统 `APPCOMMAND_MEDIA_NEXTTRACK` 触发 QQ 音乐 Tooltip 变化。

样本：

```text
tray-probe: modify-to-paint p50=360ms p95=360ms samples=1
```

结论：

- 观察链能够看到真实 Tooltip 更新并输出延迟。
- 只有一个样本，P95 没有统计代表性，不写成稳定的 P95 <= 1500 ms 通过。
- 微信头像/PNG 动态变化为 `NOT TESTED`，因为微信停在登录态。

## 6. Explorer 重启

最终 ABI 5 结果：

```text
round=1 total_ready_s=13.84 taskbar_hidden=True bar_alive=True
round=2 total_ready_s=14.45 taskbar_hidden=True bar_alive=True
round=3 total_ready_s=14.71 taskbar_hidden=True bar_alive=True
```

结果：PASS，3/3 最慢 14.71 秒。

原始文件：

- [explorer-restart-abi5.log](tray-logs/explorer-restart-abi5.log)

重启后再次按 ChatGPT 左键成功，证明 callback cache 的 `shell_pid` 已更新到新 Explorer 代际。

## 7. 性能

10 分钟整程序测量：

```text
duration_s=601.7
cpu_avg_pct=2.021
private_memory_start_mb=79.37
private_memory_end_mb=81.62
private_memory_delta_kb=2312
```

判定：

- CPU：FAIL（目标 <= 0.5%）
- 内存：FAIL（目标 <= 1024 KB）

说明：

- 这是整个 `island_bar.exe`，包含持续动画渲染、媒体、天气和托盘。
- 本轮没有把托盘链路拆成独立进程，因此不能把全部增量归因于托盘。
- 也没有通过调低 UI 帧率或改无关模块来“做出”一个好看的数字。

原始文件：

- [performance-soak-abi5.txt](tray-logs/performance-soak-abi5.txt)

## 8. 构建与自检

实际命令：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ".\IslandBar\scripts\build.ps1" -Config Debug
powershell -NoProfile -ExecutionPolicy Bypass -File ".\IslandBar\scripts\build.ps1" -Config Release
.\IslandBar\build\Debug\src\Debug\island_bar.exe --self-test
.\IslandBar\build\Release\src\Release\island_bar.exe --self-test
.\IslandBar\build\Debug\tools\smoke\Debug\island_smoke.exe
.\IslandBar\build\Release\tools\smoke\Release\island_smoke.exe
```

结果：

- 两套配置零告警。
- `--self-test` 两套退出码 0。
- `island_smoke` 两套 PASS。
- CMake 仍打印既有 C++/WinRT 投影头路径 warning；不是编译告警，也不由本轮代码产生。

## 9. 已知限制

1. 微信当前处于登录窗口，动态头像和回调未验证。
2. NVIDIA 没有可学习 callback，只能 activate/launch 或菜单 fallback。
3. Steam 左键和右键都未观察到动作，标为 `SentUnconfirmed/unknown`。
4. 右键最多证明 `probable`，无法从当前观察器确认所有自定义菜单。
5. 双击没有真实应用证据，保持 `NOT TESTED/unsupported`。
6. 多显示器仍以主 `Shell_TrayWnd` 为主要观察入口，没有每个显示器独立注册集合。
7. `IsPromoted` 仍只作为候选元数据，不静默隐藏未 promoted 图标。
8. 性能总门槛 FAIL；需要独立测量托盘链开销后再决定优化对象。
9. 私有 Windows 行为仍可能随 build 改变；ABI/schema 不匹配会安全失败并回退。

## 10. 回退

本轮仍在本地，尚未 push。

- 独立 probe 可直接删除，不影响产品。
- cache v2 可被 v1 客户端忽略；迁移不会删除旧内容。
- ABI 5 与旧 hook 按不同 ring 名称隔离，不会误读。
- 生产程序退出会恢复原生任务栏。
- 若需回退整个最终实现，回到本轮前的 `51d7df2` 即可。

## 11. 最终结论

实现层面：

- 数据身份、生命周期、ring 完整性、cache 原子性和 Explorer 恢复达到本轮目标。
- 左键 callback 优先和高置信度 HWND 校验已完成。
- 右键原生菜单达到“尽力兼容”，没有虚假承诺。

验收层面：

- 功能与稳定性测试大部分 PASS。
- CPU 和内存门槛 FAIL，必须如实保留。
- 微信/NVIDIA/双击/多显示器和完整动态 PNG 仍应保持 `BLOCKED` 或 `NOT TESTED`。
