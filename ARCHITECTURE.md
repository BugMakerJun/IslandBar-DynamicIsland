# IslandBar 架构与开发方式

写这份文件的目的只有一个：让第一次打开这个仓库的人**读完就知道每块代码住在哪儿、谁可以依赖谁、加一个功能该动哪几个文件、怎么验证自己没改坏。**

历史（每一次改动、每一个实测数字、每一次失败的原因）在 `HANDOFF.md`，那是档案不是入口。

## 1. 这东西是什么

Windows 11 上替代系统任务栏的一条横条（"岛"）：自己画 pill 与卡片，接管媒体、通知、开始菜单、任务条，并把原生任务栏藏起来。C++20 + Win32 + D3D11 + Direct2D + DirectComposition，单 exe、静态 CRT、无运行时依赖，私有内存 75-95MB。

二进制有两个：`island_bar.exe` 与 **`island_tray_hook.dll`**。模块**只**进 `Shell_TrayWnd` 所在的那一个进程
（当前是 explorer），在那里读应用发给 shell 的托盘注册（`WH_CALLWNDPROC` + 导入槽/`GetProcAddress` 两个
detour），经共享内存环回传；它钉住自己，安装器的载荷里也带它。见第 7 节。

## 2. 目录与职责

```
src/
  main.cpp          入口。解析命令行 → 要么跑一条命令，要么跑这条岛
  cli/              那些"代替岛运行"的命令（--toast-dump / --window-list / --icon-dump / --music-preview …）
  domain/           纯逻辑，独立静态库目标 island_domain：几何、布局、动画、状态策略、JSON 读取。
                    这一层不许包含 <windows.h>，也不许链接任何库（见第 3 节）
  core/             所有模块共用的基础设施：日志、进程指标
  app/              窗口与生命周期：IslandWindow（画与命中）、AppBar（占位）、PlayerWindow、
                    BarState.h（bar 自己的状态；正在一片一片从 RunBar 里搬进来，见第 7 节）
  render/           D2D/DComp 渲染：Renderer、TextRenderer、玻璃材质
  media/            GSMTC 会话、歌词、音量、声音输出、图片解码
  shell/            与 shell 打交道：开始菜单、天气、系统开关、原生任务栏
  platform/         平台能力：显示器、窗口列表、缩略图、原生任务栏（三种模式：隐藏（默认）/ 裁到托盘
                    （`--native-tray`）/ 完全不动（`--keep-taskbar`））、托盘注册的共享内存环、Win32 薄封装
  services/         Toast 通知读取与通知管道
  selftest/         诊断与 900+ 条断言（--self-test）
tools/              独立探针可执行文件（geo_probe / thumb_probe / jsonprobe / island_smoke）
setup/              安装器（payload 以 RCDATA 内嵌）
scripts/            build.ps1 / capture-*.ps1 / snapshot.ps1
```

**包含写法是硬规则**：一律写 `#include "core/Geometry.h"` 这种"模块/文件"形式，包含根是 `src/`。文件换目录就立刻编译失败，而不是被搜索路径悄悄解析到别处。

## 3. 依赖方向（唯一一条不许违反的规则）

**一句话：一层只能包含它下面的层。** 真正是一条链，不是一张网：

```
      island_bar.exe（main.cpp + selftest：只负责组装）
                    |
                  cli/
                    |
                  app/
                    |
                 render/
                    |
   services/ ---- shell/          （shell 在 services 之上、render 之下）
                    |
                 media/
                    |
                platform/
                    |
                  core/           共用基础设施：日志、进程指标、纯度检查
                    |
                 domain/          纯逻辑：布局、动画、命中、状态策略、JSON、观察账本
```

顺序：`domain → core → platform → media → services → shell → render → app → cli`。
每一层是 `src/CMakeLists.txt` 里**一个静态库目标**，只向下链接。另外两个不在链上的：
`trayhook/`（要进别的进程的模块，只许用 domain/core/platform）、`selftest/`（属于 exe，可以包含全部）。

- **`src/domain/` 不许包含 `<windows.h>`**。它只做几何、布局、动画、状态策略这些能脱离桌面运行的东西——`--self-test` 能在不开窗口的情况下跑完 900+ 条断言，靠的就是这条规则真的成立。
- 纯度检查在 **`src/core/PurityCheck.cpp`**（2026-10-16 从 `domain/` 挪过来的：它要包含 core 的头去证明那个头也是纯的，而"只能包含下面的层"不允许 domain 反向包含 core；检查本身一个字没改）。
- 这条规则**不靠自觉**，现在有三层：① `core/PurityCheck.cpp` 是一个只做一件事的翻译单元——包含 `domain/` 下所有头文件，然后 `#ifdef _WINDOWS_ #error`；`island_domain` 仍然**不链接任何库**，所以纯逻辑里出现 Win32 调用会先在编译期被拦住。② **链接图**：某一层调用了它没有链接的层，链接器会报未定义符号。③ **include 那一半由构建系统每一步检查**：`scripts/check-layers.ps1` 在每次构建时跑，按一张"每层允许包含哪些层"的表扫 `src/`，指向上面就构建失败。**为什么需要 ③**（实测 2026-10-16）：include 根是 `src/`，所以 `shell/` 里写一句 `#include "render/Renderer.h"` 照样编得过、链得过——把检查器从默认构建里临时摘掉验证过，编译器和链接器都不会说话。
- 需要 Win32 类型的地方（例如 `LONG`/`UINT` 的像素换算）放在 `platform/` 或各模块自己的头里，不放 `domain/`。
- 反向依赖（`domain/` 去依赖 `shell/`、`platform/`、`render/`，或 `island_domain` 去链接它们）在评审里直接拒绝。

## 4. 一条功能住在哪儿

| 想改什么 | 主要落点 | 顺带要看 |
|---|---|---|
| pill 的形状、边距、命中区域 | `core/Geometry.h`（唯一一份圆角矩形判定）、`core/CardLayout.h` | 渲染与命中测试用同一个函数，改一处两边一起变 |
| 展开/收起的动画手感 | `core/Motion.h`（spring，可中断、继承速度） | `core/StatePolicy.h` 决定状态迁移 |
| 卡片里某个控件的位置 | `core/CardLayout.h` | `selftest/Diagnostics.cpp` 里通常已有对它的断言 |
| **托盘页**（岛里那一页：画什么、怎么按、我们自己的菜单） | 画什么：`shell/TrayIcons.*` —— 注册表给图标与身份，`domain/TrayLedger.*` 给「现在真的注册着」，两边都有才画一格；怎么知道：`shell/TrayLearn.*`（模块 + 共享内存环 + 回调缓存）；怎么按：`shell/TrayPress.*`（编码在 `domain/TrayEncoding.h`）；我们自己的四行菜单：`shell/TrayMenu.*` | 模块在 `src/trayhook/TrayHook.cpp`，注册的线格式在 `domain/TrayRegistration.h`，通道在 `platform/TrayRing.h`；历史记录留在 `docs/TRAY-RESEARCH-RETIRED.md`，探针在 `scripts/retired/tray/` |
| 媒体卡片 | `media/MusicSession.cpp`、`media/Lyrics.cpp`、`render/` | 延迟初始化：没音乐时不该起线程 |
| 通知纸条 | `services/`（Toast 读取）加 `app/IslandWindow` 的 NoticeStrip 状态 | |
| 与 Win32 对话的细节 | `platform/` | 不要在 `core/` 里加 `<windows.h>` |
| 一条诊断命令（"这个图标到底有没有回调"这类） | `cli/Cli.cpp` | 参数解析留在 `main.cpp`；命令只负责干活并返回退出码 |
| **帧循环里的一段逻辑（相位）** | `src/main.cpp` 里 `RunBar` **上方**的自由静态函数（名字就是它干的事：`FillPanelFrame`、`PollPointerState`…），按调用顺序排列 | 跨帧存活的东西进 `app/BarState.h` 的 `BarRuntime`，**不**留在函数里；搬动必须过 `scripts/check-phase-move.ps1`（见第 7 节） |

## 5. 构建、自检、冒烟、部署

```powershell
# 构建（Debug 与 Release 都要过；/WX 现在是打开的，告警即错误）
powershell -NoProfile -ExecutionPolicy Bypass -File ".\IslandBar\scripts\build.ps1" -Config Release
powershell -NoProfile -ExecutionPolicy Bypass -File ".\IslandBar\scripts\build.ps1" -Config Debug

# 自检（当前 1233 项；退出码 0 == 全过）
.\IslandBar\build\Release\src\Release\island_bar.exe --self-test

# 冒烟（编译、链接、运行时）
.\IslandBar\build\Release\tools\smoke\Release\island_smoke.exe

# 这一轮的网（都在 scripts\ 下，各自被一次实测"假绿"换来的）：
powershell -NoProfile -ExecutionPolicy Bypass -File ".\IslandBar\scripts\check-layers.ps1"       # include 有没有指向上层；每次 build 都跑
powershell -NoProfile -ExecutionPolicy Bypass -File ".\IslandBar\scripts\check-phase-move.ps1"   # 一次"只是搬动"的证明（-Function -RegionStart -RegionEnd -Reference）
powershell -NoProfile -ExecutionPolicy Bypass -File ".\IslandBar\scripts\check-duplicate-code.ps1" # 同一段代码写了两遍的地方（抽组件的清单由它念出来，不是靠记）
powershell -NoProfile -ExecutionPolicy Bypass -File ".\IslandBar\scripts\regression\run-demo.ps1" -Name startup -Seconds 18 -Deploy -StopRunning
powershell -NoProfile -ExecutionPolicy Bypass -File ".\IslandBar\scripts\regression\normalize-log.ps1" -Name startup
```

**四条环境事实**（都是实测踩出来的，不是传闻）：

1. **exe 要从项目树外运行**。镜像在 `C:\PROJECT\Taskbar` 下的进程会被降成 LOW 完整性，UIPI 会静默丢弃所有发往 Explorer 的消息；GUI 二进制在受限沙箱里起不来（0xC0000142）。正式部署位置是 `%USERPROFILE%\IslandBar`。
2. **有单实例互斥锁**（`IslandBar.SingleInstance`），不能并行跑第二个副本做对照。要对照就先优雅关掉正在运行的那个（它会恢复原生任务栏），测完再启动。
3. **只有 PowerShell 5.1**，所有 `.ps1` 必须是纯 ASCII。
4. **注入到别人进程里的模块必须自己 PIN 住**（`GET_MODULE_HANDLE_EX_FLAG_PIN`），否则那个进程退出时导入表里会留下悬空指针——这件事已经崩过 Explorer 6 次。**现在有一个这样的模块**：`island_tray_hook.dll` 只进 `Shell_TrayWnd` 所在进程，`trayhook/TrayHook.cpp` 里钉住自己。**为什么不是全局钩子**（那才是 MyDockFinder 的做法）：未签名的模块进不了 Chromium 的渲染进程——实测装上全局钩子后 Clash Verge 的窗口整块变黑（`TrayLearn.h` 有这次实验的记录）。

## 6. 一次改动要走完的流程

1. **研究**：带着本机实测数据问外部评审（ChatGPT / Gemini），要求对方明确表态同意或反对，并记下反对点。
2. **方案**：写清改哪些文件、为什么（依据是实测数字或代码位置）、验收指标、回退方式、CPU / 内存 / 观感影响。
3. **等用户确认**，然后才动代码。
4. **实现并部署**：Debug 与 Release 都要能编过，跑 `--self-test` 与 `island_smoke`，部署到 `%USERPROFILE%\IslandBar`。
5. **等用户测**：用户说"没问题"之前不 push（本地提交可以）。
6. **推送**：用户确认后才推。本机 443 到 github.com 不通（`git push` 跑不了），走 `IslandBar\captures\push-via-api.ps1`——它按对象逐个上传 blob/tree/commit，每个哈希都与本地断言相等、分支最后才移动；文件字节取自 `git cat-file blob` 而不是工作区（`core.autocrlf` 开着，磁盘上是 CRLF、提交里是 LF）。

## 6a. 并发与系统边界的硬规则（2026-11-23）

- **线程状态不用 `volatile bool`**。后台线程与所有者之间共享的 `running_` 必须是 `std::atomic<bool>`：读用 acquire，写用 release；同一判断需要多次使用状态时先取一次快照。`Start/Stop/析构` 仍由对象所有者串行调用，不做并发启动协议。
- **停止不能假定成功，owner 必须被共享持有**。`WorkerThread::Stop()` 返回 `Stopped / Timeout / Failed`；超时后线程与停止事件保留，`Prepare()` 拒绝新旧 worker 重叠。所有会访问宿主成员的 worker 必须用 `shared_ptr` 宿主启动；后台入口返回前，最后一份 UI 引用可以释放，但对象不会析构。只有静态生命周期或完全无 context 的线程使用 `StartUnmanaged()`。
- **命名管道的响应不能被断开丢掉**。通知服务器保留 MESSAGE 管道兼容性，同时使用 `FILE_FLAG_OVERLAPPED`；每次 Connect/Read/Write 都有独立 operation context。取消失败时线程继续持有 operation、buffer、event 和 pipe，直到内核完成，再允许析构。客户端响应读取最多 5 秒。写完响应后要等客户端读取并关闭（1 秒上限），再 `DisconnectNamedPipe`。
- **窗口图标失败有界重试，并校验 HWND+PID**。同一 HWND 最多提取 4 次，等待为 500 / 2000 / 5000 ms；PID 改变视为新窗口并重建。离场仍由 `PruneWindowIconsForFrame` 释放。不要改成每帧重试。
- **图标提取暂不后台化**。`ID2D1DeviceContext2::CreateBitmap` 必须在渲染设备上下文执行；真正后台化还需要像素结果队列、HWND+PID 代次校验、设备丢失后重传和退出排空。现有有界 `WM_GETICON`（100 ms）+ 有限退避先控制最坏路径；若以后实测 P95/P99 仍超预算，再按完整后台流水线单独立项。
- **`island_tray_hook.dll` 的 `DllMain` 只做 `DisableThreadLibraryCalls`**。日志锁使用静态初始化的 `SRWLOCK`；“attached”身份日志与全部文件 I/O 延迟到第一次 `IslandTrayHookProc` 内的 `EnsureHooked()`，不在加载器锁下打开文件。

## 7. 已知的架构债（写在明处，不是等人发现）

- **层的边界已经锁住（2026-10-16）**：`domain/` 的纯度是编译期护栏，其余各层是"一个静态库 + 只向下链接"，include 那一半由 `scripts/check-layers.ps1` 在每次构建时检查（见第 3 节）。**还欠的是"层内"**：一层内部的文件之间没有约定——`render/TextRenderer.*` 想拿 `shell/` 的什么就拿、`shell/` 内部各文件互相拿类型。要收紧得靠更小的目标或前置声明，目前没做。
- **重复代码有一个念得出来的清单（2026-10-16）**：`scripts/check-duplicate-code.ps1` 现在报 **90 组** 6 行重复（扫描 104 个文件）。已抽掉的是最长、且证据说明**已经在害人**的那两条：七个读取器抄了七遍的线程收尾（抄的过程中已经漂移，`SoundOutput::Start` 失败会把线程留在跑、重试会覆盖停止事件句柄）与两份 WinHTTP 取回（见 HANDOFF 8.154）。**剩下的没有计划**——抽不抽由用户决定，因为这批里不少是"同一件事在两种上下文里各写一遍"，抽掉未必更好。
- **`main.cpp` 已经不大了，而且不再是"岛本身"**（2026-10-16 实测：`main.cpp` **588 行**，`main()` 从第 257 行开始；岛在 `app/BarLoop.cpp` —— **6,016 行**，其中 `RunBar` **1,347 行**、它上方 **43 个相位**，帧循环 **835 行**）。搬出去过的东西：托盘的行为 `shell/TrayMenu.*`、代替岛运行的那些命令 `cli/Cli.cpp`（约 950 行）、`RunBar` 里跨帧状态 `app/BarState.h`（`BarRuntime`）、自检从 8,968 行拆成 `selftest/` 里 11 个文件（`Diagnostics.cpp` 只剩两个入口，**567 行**）。
  **帧循环现在是一串具名相位**：相位 = 自由静态函数 + 显式参数（`FillPanelFrame` / `PollPointerState` / `FillDecodedPictures`…），从循环里搬出去时**逐字移动**，只有缩进和"跨帧状态加前缀"这两处变化。**跨帧存活的东西一律进 `BarRuntime`**，因为编译器抓不住"静默用了外层状态"这件事。每片都要走完：两套配置零告警、`--self-test`、`island_smoke`、固定场景日志对照（`scripts/regression/`）、部署跑一遍。
  **搬动由 `scripts/check-phase-move.ps1` 校验**，它是文件而不是几行临时命令：它拿参考提交里的行区间与函数体现在的内容逐条规范化后比较（去空白、去 `runtime.` 前缀），**空区域算错误**——手写版本在同一个会话里给出过三次"假绿"。边界要单独断言（起始行、结束行、紧跟其后的是什么），因为"比较结果为相同"也可能是两边都空。
  **循环里现在只剩 53 条自己的语句**（2026-10-16 实测；按"缩进 12 空格、非注释、非花括号"数），其中 **19 条是相位调用**，其余是它自己的事：取消息、诊断注入的那四个 `if`、状态日志行、`if (needDraw)` 这个框、节流/内存/区间三行报告、`PaceTheLoop` 与两个工作位。**"202 条顶层语句"那个旧口径复现不出来**（同一段代码按上面这个口径当时是 70 条），所以以后只写这个口径的数字。
  **第 4 节第 4 条现在成立**（2026-10-16，提交 `51756a1`）：`main.cpp` 是"解析参数 → 跑命令或跑岛"，岛在 `app/BarLoop.*` 里。**欠的只剩形状**：`app/BarLoop.cpp` 一个翻译单元装着 43 个相位 + `RunBar`（6,016 行），与当年 `main.cpp` 是同一个形状，只是主题变成了"岛"；要再分成"一个主题一个文件"是可行的，但**没有计划**（没有实测证据说它值得）。
- **已还的债**（留着这几行是为了说明它们不再是问题）：`core/` 已抽成 `island_domain` 独立静态库（编译期护栏见第 3 节）；模块↔主程序的通道是**共享内存环 + ABI 版本号**（`platform/TrayRing.h`），同步路径里没有磁盘 I/O；安装器的载荷里带 `island_tray_hook.dll`（构建输出那行 `Copying island_bar.exe and island_tray_hook.dll into the setup payload`）。
- **托盘是"岛里我们自己的那一页"**（2026-10-16，HANDOFF 8.144/8.145/8.146/8.147），**默认整条隐藏原生任务栏**；`--native-tray` 才是 2026-10-15 那套"把任务栏裁到只剩通知区域、屏幕上留系统自己的托盘"，`--keep-taskbar` 是完全不动它。按压的唯一编码入口是 `domain/TrayEncoding.h`（`PlanTrayClick`）：把应用自己的 `uCallbackMessage` 投递到它自己的托盘窗口；学不到回调的格子（有应用窗口活着但不应答、或从来没注册过回调的）开我们自己的菜单，并如实标注"应用菜单（本机拿不到）"。
- **卡片画什么 = 注册表 ∩ shell 里真的注册着的**（2026-10-16，HANDOFF 8.151）。注册表 `HKCU\Control Panel\NotifyIconSettings` 给图标 PNG 与身份（uid / guid），`domain/TrayLedger.h` 的账本给"现在是否真的活着"——账本由模块在 shell 里看到的 `NIM_ADD`/`NIM_MODIFY`/`NIM_DELETE` 记出来，窗口没了就剔除；第一趟学习跑完之前用注册表列表兜底。**旧口径**（只按"这条记录的 exe 有进程在跑"过滤）会把 NVIDIA 的三个常驻条目和一条死掉的 explorer 记录也画出来——实测 **13 格 vs 系统托盘的 9 个**。
- **`--native-tray` 那条路为什么成立、边界在哪**：读 HANDOFF 8.133 的三个判别实验（占名当托盘宿主**失败**；搬托盘窗口**被系统接受但像素不跟**；`SetWindowPos` 搬整条任务栏**被忽略**；只有**裁剪**好看又零成本）。它的代价：那 144 DIP 里**包含时钟**，做不到"只留图标不留时钟"，托盘也只能待在 shell 放它的地方。
