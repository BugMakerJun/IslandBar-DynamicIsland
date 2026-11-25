# IslandBar 全面审查整改记录（2026-11-26）

基线：`e86d3a675109c3ec312a2593ae1e4514555faa6c`

范围：P0 → P3。ChatGPT 的内部浏览器审查已执行；Gemini 按用户要求跳过。
本文件记录实际代码依据、根因、修复、验证结果和仍未覆盖的环境项，不把推断写成已验证。

## 1. P0：任务栏异常退出恢复

### P0-01 硬杀后任务栏可能永久隐藏

- 源码依据：原 `NativeTaskbar::Hide/Restore` 只在 bar 进程内执行恢复；`TrayHook.cpp` 没有跨进程恢复所有者。
- 触发条件：任务管理器结束进程、崩溃、调试器终止、注销前异常退出。
- 根因：改变 Explorer 状态的进程同时是唯一负责恢复的进程；该进程消失后没有任何存活实体回滚状态。
- 修复：新增 `platform/TaskbarRecovery.*`。bar 在改任务栏前原子写入 owner PID、creation time、session、
  Shell PID、主/副任务栏可见性、窗口区域和工作区；已 pinned 的 tray hook 在 Explorer 内运行 guardian，
  等待 owner 进程句柄并只在其异常终止时恢复。
- 防 PID 复用：owner 和 guardian 都同时校验 PID + process creation time；不把 PID 单值当身份。
- 防半写文件：恢复快照带 magic、version、size、FNV-1a checksum，并用同目录临时文件 +
  `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH` 原子替换。
- 验收：`taskbar-recovery-test.ps1` 强杀循环。最终 Release `20/20`，最慢 `76 ms`；Debug `20/20`，最慢 `123 ms`。

### P0-02 Guardian 未就绪时仍可能改任务栏

- 源码依据：`NativeTaskbar::Hide()`。
- 根因：原路径只检查任务栏窗口是否存在，没有检查“谁来恢复”。
- 修复：`Hide()` 先要求 `TaskbarGuardianHealthy()`；失败则 fail closed，保持原生任务栏可见并记录原因。
- 验收：自检项 `the native taskbar refuses to hide without a recovery guardian`。

## 2. P1：托盘跨进程通道

### P1-01 Ring 首次初始化竞争

- 源码依据：原 `TrayRing.h::EnsureRingHeader()`。
- 根因：多个进程可同时看到 `magic == 0`，随后并行填写头部；读者也可能看到半初始化字段。
- 修复：ABI 6 的所有共享字节访问统一进入命名 Mutex。首次初始化在锁内完成，最后才发布 magic；
  ABI、record size、capacity、version 全部校验后才开放 header/records。

### P1-02 多写者覆盖、撕裂和类型混淆

- 源码依据：原 per-slot sequence + 无锁普通结构体复制。
- 根因：槽位回绕时写者可覆盖未读槽；“前后序号相同”不能证明 C++ 非原子读合法。
- 修复：64 槽固定有界队列；队列满时拒绝新记录并累计 `droppedFull`，永不覆盖未消费记录。
  Publisher 与 Reader 都在 Mutex 内复制完整 record，记录 kind 与 payload 一并写入。
- 验收：4 线程 + 2 子进程压力无撕裂/错型；满 Ring 只丢新记录；不兼容 ABI 一次拒绝后持续拒绝。

### P1-03 32 位 cursor 回绕

- 源码依据：原 `TrayRingReader::Drain(unsigned& cursor, ...)`，而 header counter 已是 64 位。
- 触发条件：累计记录数超过 `2^32`。
- 修复：cursor 改为 `std::uint64_t`；生产、probe、自测调用点全部同步。
- 验收：测试把计数置于 `0xFFFFFFF0`，读 32 条后要求 cursor 精确为 `0x100000010`。

### P1-04 Mutex 无限等待会阻塞 Explorer

- 根因：Publisher 在 Shell 消息路径内，持锁者挂起时无限等待会冻结注册应用和 Explorer。
- 修复：Publisher 最多等 `1 ms`，Reader 最多等 `20 ms`；超时分别计为 `droppedLock` 或保留 cursor 下轮重试。
- `WAIT_ABANDONED`：检查 `writeInFlight/pendingIndex`，清除半写槽，再修正 `read <= written` 后才继续使用。

### P1-05 Hook 阻塞、磁盘 I/O 和导入表重写

- 源码依据：原 `TrayHook.cpp` 的 IAT/`GetProcAddress` detour 和报告文件路径。
- 风险：在 Explorer 消息路径内做字符串格式化、文件打开/写入或改写第三方导入。
- 修复：生产模块改为 observer-only：只安装一个 `WH_CALLWNDPROC`，只观察 `WM_COPYDATA`，
  解码后发布到 Ring 并立即返回。删除生产 detour 和注册路径磁盘 I/O。
- 保留：模块自行 PIN；guardian thread 只在 Explorer 内存在；不兼容进程直接退出观察路径。
- 验收：真实 observation 捕获 `13/17` 条注册，卡片覆盖 `9/13`；应用启动后 Ring 继续增长。

### P1-06 TrayLearn 数据丢失、无界增长和竞态

- 源码依据：`TrayLearn.cpp::HarvestReport/DrainTrayRing/ReportReader`。
- 修复：
  - Report reader 上限 2 MiB，按实际读取字节转换，不再固定 64 KiB 截断。
  - Ring cursor 只在记录已进入内存 pending 队列后推进。
  - pending 队列有上限；报告文件到 2 MiB 时截断。
  - observer 状态读写由 Mutex 保护。
  - PID 缓存按 `(PID, creation time)` 键，最多 256 项 LRU；空结果不缓存。
  - `tray_callbacks.txt` 和 `tray_right.txt` 走同一原子替换 primitive。
- 验收：8 线程并发更新零丢失；独占文件时更新失败且旧 cache 完整保留。

## 3. P1：线程与生命周期

### P1-07 detached 工作线程和 Stop 失败被忽略

- 源码依据：Apps list、tray learning、tray press、system toggles 等多处 `std::thread(...).detach()`。
- 根因：进程退出时没有所有者等待；超时被当作成功；对象可能在线程仍访问 `this` 时析构。
- 修复：统一到 `WorkerThread`。会访问宿主成员的任务用 `shared_ptr` owner；纯进程静态任务才用
  `StartUnmanaged`。`Stop()` 返回 `Stopped/Timeout/Failed`，失败时保留线程、stop event 和 owner。
- 覆盖：AudioVolume、Lyrics、MusicSession、SoundOutput、NotificationServer、ToastReader、
  WindowsFileSearch、SystemStatusReader、WeatherReader、PlayerWindowWatch、WeChatWatcher、
  StartApps list/icons、TrayPress、SystemToggles、TrayWake、tray learning。
- 正常退出：`BarLoop` 通过 `ReportWorkerStop` 逐项报告；timeout 不再伪装成已停止。

### P1-08 Icon worker timeout 后关闭仍被使用的 event

- 源码依据：`StartApps::StopIconWorker/StartIconWorker`。
- 根因：超时后把 `iconWorkerRunning_` 清零，重开时关闭 `iconWakeEvent_`；旧线程可能仍在等待该句柄。
- 修复：timeout 时保留 running 状态；线程自己的 scope-exit 在真正返回时清标志。停止成功后才关闭 event。

### P1-09 锁文件被占用时诊断完全消失

- 源码依据：`core/Log.cpp::OpenLog`。
- 触发条件：程序旁日志和 `%TEMP%\island_bar.log` 都短暂不可打开。
- 根因：只有两个固定路径；两者都失败后 `g_file` 为空，运行仍继续但所有诊断丢失。
- 修复：最终回退到 `%TEMP%\island_bar-<pid>.log`；恢复测试按 PID 查找实际日志。

## 4. P2：帧调度与渲染资源

### P2-01 帧统计混入空闲等待

- 源码依据：原 `FrameStats::Record` 把“两次绘制之间存在数秒空闲”当成动画帧间隔。
- 修复：新增 `RecordAtTicks`；只有连续 animation/fullscreen 绘制才进入帧间隔序列。
  一次性时钟/通知绘制不成为下一次动画的基准；idle wait 单独统计。
- 验收：确定性自测覆盖活跃、空闲、单次绘制、全屏和动画序列。

### P2-02 每帧重复创建 brush / clip geometry

- 修复：
  - 颜色量化 solid-brush cache，按帧维护，容量 512；超预算 brush 在本帧保留，在下一帧开始时清理。
  - card reveal 的矩形 geometry 由 Renderer 持有，只在矩形边缘变化时重建。
  - morph brush 复用一个 renderer-owned brush，并只改颜色。
- 避免新引入的 UAF：不在一次绘制内部清 cache，避免同帧早先取得的 brush 指针被后续 lookup 释放。
- 验收：正常每 2 秒 `brush_misses=0`、`brush_clears=0`；动画循环只在预算触发时清理。

### P2-03 Explorer 重启后任务栏多显示约 14 秒

- 实测：旧实现把 guardian 重装绑在托盘学习 15 秒节流上。
- 修复：任务栏检查发现 claim 仍在但 guardian 已失联时，先对新 Shell 执行
  `EnsureTrayObserver(kObserverWaitMs)`，成功后再 `EnsureHidden`。托盘学习仍在后台独立进行。
- 验收：最终 Release 从强杀旧 Explorer 到新任务栏重新隐藏 `323 ms`；日志有
  `observer is up in the shell` 与 `native taskbar: re-hidden`。

## 5. P3：架构、可维护性和 UI/UX 验证

### P3-01 新 domain 头可能逃过 Win32 纯度检查

- 源码依据：原 `PurityCheck.cpp` 手写头文件列表。
- 修复：CMake 枚举所有 `domain/*.h`，生成 include 清单；`PurityCheck.cpp` 只消费生成清单。
- 结果：新增 domain 头不需要人工记得更新检查器。

### P3-02 托盘右键学习文件非原子

- 修复：`tray_right.txt` 改走 `TrayCallbackCache::Update`；独占/替换失败时保留旧文件并记录。

### P3-03 交互与布局

- 未发现必须改布局的真实源码缺陷；现有 1273 项自测覆盖 pill/card 命中、
  Start/System/Tray/Music/Weather/Calendar/Niri 区域、滚动、DPI 比例、文字裁剪和鼠标离开状态。
- 自测同时验证绘制区域与命中区域共用 layout，不通过“看起来差不多”代替。

## 6. 构建与测试结果

构建：

- Debug x64：PASS，`/WX`，配置与编译输出零 warning。
- Release x64：PASS，`/WX`，配置与编译输出零 warning。
- `build.ps1` 自动发现 `WindowsSdkDir`，消除了 6 条 C++/WinRT projection configure warning。

自测与探针：

- Debug `--self-test`：`1273 OK / 0 FAIL`，exit 0。
- Release `--self-test`：`1273 OK / 0 FAIL`，exit 0。
- Debug/Release `island_smoke`：PASS。
- `tray_protocol_test`：PASS；含 4 线程 + 2 进程、100000 条持续记录、满 Ring、32 位边界。
- `tray_cache_test`：PASS；8 线程更新、旧 schema 迁移、替换失败保留旧文件。

稳定性：

- Debug hard-kill recovery：`20/20`，max `123 ms`。
- Release hard-kill recovery：`20/20`，max `76 ms`。
- Explorer 替换：observer 重新安装，任务栏 `323 ms` 内重新隐藏。
- 真实 tray learn：捕获 `17` 条注册，卡片覆盖 `9/13`，无 ABI mismatch。

性能：

- 165 Hz 动画：286 个样本；单段最差 `p95 15.4 ms / p99 19.6 ms / max 19.6 ms`，
  `hitches 0`。
- 静止 40 秒：CPU `0.13-0.47%`；private `80.61-83.13 MB`（35 秒后平）；
  handles `1006-1016`，GDI `44` 不变，USER `19-20`。
- 正常音乐运行 5 秒采样：CPU `0.62-1.89%`；GPU engine sum `0.5-2.61%`。
- 正常 render log：`brush_hits` 持续增长，`brush_misses=0`、`brush_clears=0`（稳定后）。

## 7. 未验证 / 剩余风险

以下不是代码失败，而是本轮机器条件无法诚实回答的项：

- 多显示器热插拔、不同刷新率、跨屏 DPI；本机只有单显示器 96 DPI。
- 真实睡眠/唤醒、锁屏/解锁、用户注销。
- 真实独占全屏游戏切换；本轮验证了全屏判据、比例和退场逻辑，没有启动游戏冒充实测。
- 数小时 private-bytes 趋势；本轮做了 40 秒分段和稳定性测试，不把分钟级结果写成小时级。
- 第三方托盘应用的完整交互矩阵仍应由用户实际点验；协议/观察路径已验证，真实左键/右键菜单属于 UI 手测项。

没有执行 `git push`。
