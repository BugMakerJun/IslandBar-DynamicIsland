# IslandBar · 灵动岛

Windows 11 的任务栏替代品：一个灵动岛风格的栏，固定在屏幕顶部，承载**开始按钮**、**已打开窗口**和**快捷设置面板**，并隐藏系统自带的任务栏。

技术栈：C++20 · Direct2D · DirectComposition。

## 长什么样

```
![Uploading dfcb78fd-1a8a-40bd-988a-51fd53a6eb54.png…]()


```

快捷设置面板从**栏的下方**、被点击的按钮下面展开：

```
[ v ][ # ]   ( 灵动岛 )   [ 应用 ] [ 应用 ] [ 应用 ]
  +---------------------------------------------------+
  |  Wi-Fi      | 蓝牙         | 飞行模式               |
  |  辅助功能    | 节能模式     | 实时字幕               |
  |  (音量)============o                               |
  |                                            (齿轮)  |
  +---------------------------------------------------+
```

## 当前状态

栏是一个无边框、始终置顶、不抢焦点的窗口，横跨整个显示器宽度，通过合成交换链绘制；**除了它实际画出的形状之外，所有区域对点击透明**。

**灵动岛只有一个形状。** `Collapsed`、`Hover`、`MusicStrip` 三者都是 220 × 36 ——
"灵动岛不需要再左右胀了" 去掉了每个状态各自的宽度，所以**鼠标移到上面不会改变它的大小**。
只有 `Expanded`（卡片）是不同的形状。

| 状态 | 尺寸 (DIP) | 内容 |
|---|---|---|
| Collapsed / Hover / MusicStrip | 220 × 36 | `时:分`，播音乐时显示当前歌词 |
| Expanded | 520 × 250 | 系统面板、通知卡片，或音乐卡片 |
| FullBar | 显示器宽度 × 32 | 有全屏窗口时，填充那条预留带 |

窗口**只创建一次**，尺寸为 显示器宽度 × 320 DIP，**之后永不缩放**：所有过渡都是在窗口**内部**移动和放大形状。
缩放一个基于合成交换链的窗口会闪烁，所以替代方案就是在固定画布内做动画。

灵动岛两侧挂着两个"舱"——左侧是开始和系统按钮，右侧是应用图标。**两者都按【静止时】的胶囊来定位，而不是按动画中的形状。**
这个区别很关键：按卡片大小的盒子去排版，会把图标行放进面板自己的地盘里；而把 520 DIP 的舱居中在 220 DIP 的胶囊上，会把按钮压到卡片底下。

## 试一下

```powershell
# 构建
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Config Debug -RunSmoke

# 运行（会隐藏系统任务栏；加 --keep-taskbar 可以保留它）
& .\build\Debug\src\Debug\island_bar.exe --keep-taskbar
```

然后：按左舱的 **chevron（^）** 打开快捷设置面板，按 **开始**，或点任意**应用图标**。
点灵动岛或面板外的任何地方，面板会收起。

## 装一个发行包

`IslandBarSetup.exe` 是**一个文件**（安装器把栏作为资源带在自己身上），双击即装：

* 装**当前用户**、**不需要管理员**（只写 `%USERPROFILE%\IslandBar` 和 `HKCU`）；
* 开始菜单两个入口（`IslandBar` / `IslandBar (safe mode)`，后者不隐藏系统任务栏）+ 开机自启 +
  `设置 → 应用` 里的卸载项；
* 运行库是**静态链接**的（`/MT`），所以目标机器只要有 Windows 11 就行。

出包：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\package-release.ps1
```

它会构建 Release、按**安装器自己的版本资源**给文件命名（`dist\IslandBarSetup-<版本>.exe`），
并用 `dumpbin /dependents` 确认包里**没有** MSVC 运行库（有就报错退出 —— 那种包装得上、在别的电脑上起不来）。

## 自检

`island_bar.exe --self-test` 运行**数百项运行时断言**，覆盖：显示器度量、圆角矩形命中测试、
JSON 线格式（含恶意与畸形输入）、通知存储、状态机与缓动曲线、两个舱和卡片的排版算术、
窗口扩展样式与 DPI 感知、合成表面、**从 GPU 回读的渲染像素**，以及区域级的点击穿透。
**退出码就是失败数量，所以 0 表示全通过。**

几何计算都放在**纯函数**里（`Motion.cpp`、`TaskStripLayout.h`、`CardLayout.h`、
`StatePolicy.h`、`WindowList.h`），正是为了**不需要窗口、GPU 或显示器就能断言它们**。

## 系统面板（快捷设置）

六块磁贴、一个音量滑块和一个设置齿轮，按系统自带快捷设置的样子排布。
由 chevron 打开，位置在被点击的按钮下方，点别处关闭。

磁贴会打开对应的 `ms-settings:` 页面。它们**不假装能切换**：
Windows 没有为切换 WLAN 射频提供受支持的 API；而能读到状态的磁贴（节能模式）会**如实反映**状态，
而不是显示一个它改变不了的状态。

图标来自 `Segoe Fluent Icons`，**每个码位都是渲染出来确认过的** ——
缺字形只会画一个空方框且不报错，所以最初猜的三个码位**全是错的**。

## 通知

栏在一个命名管道上监听，把收到的内容显示成一张展开的卡片。
推送通道已经完整；读取 Windows 自己的通知数据库（`wpndatabase.db`）是另一半，**尚未完成**。

从另一个进程发一条 —— 栏必须已经在运行：

```bat
"build\Debug\src\Debug\island_bar.exe" --notify "构建完成" --body "0 个错误，3 个警告" --app DSH
```

可选：`--urgency low|normal|high`、`--sticky`（不自动消失）。

完整的线协议：

    请求 := uint32 长度（小端） | 该长度的 UTF-8 JSON 字节
    响应 := 同样的分帧，一个 UTF-8 JSON 对象

用长度前缀而不是换行分帧，因为正文里可能含换行。
字段：`app`、`title`、`body`、`icon`、`urgency`、`sticky`。
`title` 和 `body` 都为空的消息会被**拒绝**，而不是显示成一张空白卡片。

卡片在通知到达时打开，停留六秒；`--sticky` 的要等到确认才走。
点灵动岛会把所有通知标记为已读并关闭卡片。

管道**故意**允许 Everyone 访问：命名管道只在本机，恶意写入者最坏也只能显示一条通知，
而且解析器把每个字段都当作不可信输入。理由写在 `NotificationPipe.cpp` 的 `PipeSecurity` 注释里。

## 音乐与歌词

栏通过 C++/WinRT 从系统媒体控件（SMTC，也就是音量浮出控件用的同一个会话）读取正在播放的内容。
**任何发布了媒体会话的程序都能用** —— Spotify、浏览器、QQ 音乐、Media Player —— 不需要针对某个应用做适配。

有东西在播放时，灵动岛用一行显示当前歌词并滚动；没有歌词时回退到曲名。
点击它会打开详情卡片：专辑封面、标题、`Playing - M83 - MusicApp`、三个传输按钮和一条进度条。
传输按钮的调用跑在轮询线程上，**绝不跑在 UI 线程**。
没有上报时长的曲目（比如直播流）**完全不画进度条**，而不是画一条空条 —— 一条永远为空的进度条看起来就像 bug。

歌词按顺序从两个公开 API 拉取 —— 先网易云，再 QQ 音乐 —— 跑在 worker 线程上，**绝不跑在 UI 线程**。
用两个源是因为网络可能挡住其中一个而不挡另一个，且各自会报告自己的失败原因，所以日志里两个都看得到。
其它任何情况（没有歌词、搜索无结果、超时、响应结构变了）都按"没有歌词"处理，
灵动岛回退到曲名。**这是正常结果，不是需要上报的错误。**

`--music-preview` 用示例数据循环各个音乐形状，这是**在没有歌播放时检查排版**的办法。
`--lyrics-test "<曲名>" ["<歌手>"]` 拉取一首歌的歌词并打印返回了什么，
这是**不启动栏就能检查网络路径**的办法。加 `--lyrics-verbose` 还会打印请求的确切 URL 和每个响应的开头，
这是**区分 403 和"结果为空"的唯一办法**。

## 隐藏系统任务栏

栏会在启动时隐藏系统任务栏，并在退出时**始终恢复**它 —— 因为这个项目的意义就是**成为**那个任务栏。
`--keep-taskbar` 保留系统栏不动，开发时你会想要这个。

**这需要进程与 Explorer 运行在同一完整性级别。** UIPI 会静默丢弃从较低完整性级别发往较高级别的窗口消息，
所以在降低了令牌的沙箱里，这个调用返回 0、什么也不发生，**而且没有错误**。
程序会检测到这一点并在启动时说明，而不是悄悄失败。检查方法：

```powershell
& .\build\Debug\src\Debug\island_bar.exe --probe-taskbar
```

如果任务栏被留在隐藏状态（进程是被强杀而不是正常关闭）：

```powershell
taskkill /f /im explorer.exe && start explorer.exe
```

## 同一时刻只允许一个实例

用一个命名互斥体。两份副本会画出两条叠在一起的栏，而**症状看起来不像"有两个程序在跑"** ——
它看起来像**一条画错了位置的栏**，因为第二份副本在第一份没有的地方。
第二次启动会写日志并立即退出；**它不弹对话框**，因为对话框会阻塞进程，
而一个被拒绝却还赖着不走的副本，正是这个保护本身要防止的状态。

## 构建

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Config Debug -RunSmoke
```

可选参数：

| 参数 | 含义 |
|---|---|
| `-Config Debug\|Release` | 构建配置，默认 Debug |
| `-Clean` | 先删除构建目录 |
| `-RunSmoke` | 构建后跑冒烟测试 |
| `-NoSmoke` | 完全不构建冒烟测试 |

要在交互式会话里获得 MSVC 环境：

```powershell
. .\scripts\env.ps1
cl
```

## 运行

```powershell
# --- PowerShell：& 是【调用运算符】，引号路径前必须有它 ---
& .\build\Debug\src\Debug\island_bar.exe --self-test
& .\build\Debug\src\Debug\island_bar.exe --probe-taskbar
& .\build\Debug\src\Debug\island_bar.exe --demo
& .\build\Debug\src\Debug\island_bar.exe
```

```bat
REM --- cmd.exe：& 是【命令分隔符】，不能放在路径前面 ---
"build\Debug\src\Debug\island_bar.exe" --self-test
start /wait "" "build\Debug\src\Debug\island_bar.exe" --probe-taskbar
```

在 cmd.exe 里写 `& "path"` 会失败并报 *"& was unexpected at this time"*，程序根本不会运行。

**要捕获输出，必须用 `cmd /c` 加重定向，不能用 PowerShell 的重定向。**
这是一个 GUI 子系统的二进制，它通过 `AttachConsole` 借用父控制台，
所以它写的是控制台缓冲区，而不是继承来的句柄。从 PowerShell 里
`& .\island_bar.exe --self-test > log.txt` 会留下一个 **0 字节的文件**：

```bat
cmd /c "build\Debug\src\Debug\island_bar.exe" --self-test > selftest.txt 2>&1
```

完整参数表：

| 参数 | 含义 |
|---|---|
| `--keep-taskbar` | 不隐藏系统任务栏 |
| `--keep-strip` | 不让出系统任务栏占着的那条工作区（最大化窗口会照旧在顶部留一条空白） |
| `--self-test` | 运行运行时断言；退出码 = 失败数 |
| `--probe-taskbar` | 本进程能否隐藏系统任务栏？退出 0 = 能，1 = 被 UIPI 挡住，2 = 任务栏恢复失败 |
| `--demo` | 无需输入，循环 collapsed / hover / expanded |
| `--state <名称>` | 钉住某一个形状，方便观察排版 |
| `--system-panel` | 启动时就让快捷设置面板打开 |
| `--start-hover` | 让开始按钮处于悬停态 |
| `--music-preview` | 用示例数据循环各个音乐形状 |
| `--lyrics-test <曲名> [歌手]` | 拉取一首歌的歌词并报告返回了什么 |
| `--lyrics-verbose` | 连同请求 URL 和响应开头一起打印 |
| `--notify` | 向正在运行的栏推送一条通知 |
| `--volume-test` / `--volume-set` | 检查 / 设置默认输出端点的音量 |
| `--window-list` / `--icon-dump` | 打印窗口列表 / 转储图标 |
| `--appbar` | 以 AppBar 方式认领屏幕边缘（可能被拒绝） |
| `--no-console` | 表现得像没有控制台一样（诊断用） |
| `--help` | 打印用法 |

## 目录结构

```
CMakeLists.txt            顶层工程（C++20、/W4、UNICODE、静态 CRT）
CMakePresets.json         同样两套配置，按预设名使用
cmake/WinRt.cmake         栏本体和两个探针共用的 C++/WinRT 设置
scripts/env.ps1           把 MSVC 环境导入当前会话
scripts/build.ps1         配置 + 构建 + 可选冒烟
scripts/snapshot.ps1      给源码树打一个带时间戳的本地快照
docs/                     交接文档、功能清单、实测截图

src/                      栏本体 —— 一个目录一个模块
  main.cpp                  入口：--self-test、--demo，或运行栏
  island_bar.manifest       PerMonitorV2 DPI 感知
  island_bar.rc             版本资源
  app/                      窗口，以及挂在它身上的东西
    IslandWindow            窗口：置顶、不抢焦点、点击穿透、悬停
    AppBar                  认领屏幕边缘，以及被拒绝
    PlayerWindow            跟着某个窗口走的小播放器
  core/                     纯逻辑，以及所有模块共用的东西
    Geometry.h              度量常量 + 【唯一一份】圆角矩形命中测试
    Motion                  灵动岛的状态、几何与插值
    StatePolicy.h           指针和面板分别要求哪个形状
    TaskStripLayout.h       两个舱：每个按钮的位置与命中测试
    CardLayout.h            卡片内容：磁贴、滑块、齿轮、音乐卡片
    Log                     所有控制台输出都过这里
    MiniJson                通知协议背后的 JSON 读取
  platform/                 栏所依赖的 Win32 与 shell 接口
    Monitor                 工作区、DPI、推断出的任务栏厚度
    NativeTaskbar           隐藏/恢复系统任务栏（+ UIPI 检测）
    WindowList              已打开的窗口，顺序稳定
    DisplayBrightness       DDC/CI，每次写入都回读
    GlassMaterial           背景材质
    ThumbnailPreview        DWM 实时缩略图
  media/                    在放什么，以及它长什么样
    MusicSession            SMTC 正在播放（C++/WinRT）
    Lyrics                  LRC 解析，以及背后的搜索
    SoundOutput             混音器；AudioVolume 是主音量
    WicImage                图片解码：专辑封面、模糊底纹
  render/                   一切往合成面上画像素的东西
    Renderer                D3D11 + Direct2D + DirectComposition + 像素回读
    TextRenderer            DirectWrite：时钟、磁贴、图标
    Theme                   配色集
  shell/                    把系统 shell 自己的控件镜像过来
    StartMenu, StartApps    开始按钮和它的固定应用
    SystemStatus            WLAN、蓝牙、音量、电源、输入法
    SystemToggles           切换无线电和夜间模式，并回读结果
    Weather                 状态行上的天气读数
    TrayIcons, TrayPress    托盘，以及按下它的图标
  services/                 把东西送进来的通道
    Notification*           模型、命名管道、发件人图标
    ToastStore              Windows 自己的通知数据库
    WeChatWatch             来电与消息的观察者
  selftest/Diagnostics      运行时自检（-self-test）

tools/                    开发用探针，都不随程序发布
setup/                    安装器，栏本体嵌在里面
```

## 贡献者须知

- **`.ps1` 文件必须保持纯 ASCII。** Windows PowerShell 5.1 除非脚本带 UTF-8 BOM，
  否则按系统 ANSI 代码页读取，非 ASCII 字面量会被损坏。
  路径要从 `$PSCommandPath` 推导，不要写死。
- **C++ 源码里的中文字面量一律用 `\uXXXX` 转义**，绝不用 UTF-8 原文 ——
  工具链已经把它悄悄转成乱码过一次。
- 透明度必须通过 `IDCompositionEffectGroup` 驱动，不能用 `IDCompositionVisual3` —— 见 HANDOFF.md 6.3。
- 任务栏厚度在运行时从工作区差值测量，**永远不要写死**。
- 窗口只创建一次、尺寸固定、**永不缩放**。缩放合成交换链窗口会闪烁；要在它内部做动画。
- **渲染、命中测试和窗口的输入区域必须用同一个排版函数。**
  已经有三次改动只动了其中之一，而每次的症状都和原因长得不一样：
  一个画出来却点不动的按钮、一个被裁成条的面板、消失的图标。
- `FillRoundedRectangle` / `DrawRoundedRectangle` 接受的是 `ID2D1Brush`，**不是颜色**。
  其它已经踩过的 API 陷阱见 HANDOFF.md 6.5。
- **每一次绘制调用，`Clear` 也算，都必须夹在 `BeginDraw()` 和 `EndDraw()` 之间。**
  D2D 会静默丢弃这对之外的调用：画面全空，而每个 HRESULT 都报成功。
  正是这个错误曾经造出一个"45 项自检全绿却看不见的胶囊"。
- 因此**永远不要相信"API 返回了 S_OK"就等于画出来了**。要断言像素 ——
  `Renderer::ReadBackFrame` 就是为此存在的。
- 胶囊是**顶部锚定**的，所有状态共用同一条顶边。改之前先读 `IslandAnchor` 上的注释。
- 屏幕坐标和窗口内坐标要分清。锚点用窗口内 DIP 表达；
  在那里再减一次窗口原点会重复计算偏移，把灵动岛放到自己的窗口之外 ——
  这在屏幕上看不出来，只有自检能抓到。
- **给 frame 设字段，必须在任何东西读它之前。**
  `contentOpacity`、`pill.box`、`systemPanelOpen`、`outerCardUp` 各自都至少被写过一次"在第一个读者之后赋值"，
  而每一次的症状看起来都像渲染坏了，而不是顺序错了。
- 诊断信息要报告**哪一步失败了、HRESULT 是多少**，而不是只报 true/false。
  `ReadBackFrameVerbose` 的存在就是因为一个裸 bool 曾让真实失败变得无从下手。
- 两个相关的数值有疑问时，**把它们打到同一行**。
  两个错数字并排就无处可藏；单独看任何一个都像"还没到时候"。

完整的跨会话记录 —— 每一轮的决策、已验证的环境事实、API 发现、踩过的坑和路线图 ——
见 [`HANDOFF.md`](HANDOFF.md)。[`FEATURES.md`](FEATURES.md) 是功能清单，每项都标了证据等级。
