# IslandBar 手工自测清单

> 用途：**只有你能做的验证**。自检（`island_bar.exe --self-test`，现在 908 项全过）覆盖几何、解析、像素，
> 但"鼠标点下去到底发生什么"必须在真人手里过一遍。
> **已脱敏**：命令里的 `$env:USERPROFILE` 就是你的用户目录（本文档按 PowerShell 写，所以用这个变量而不是 `%USERPROFILE%`）。
>
> 日志（**每一步的证据都在这里**）：
> `$env:USERPROFILE\IslandBar\island_bar.log`
> 想边点边看：
> `Get-Content $env:USERPROFILE\IslandBar\island_bar.log -Wait -Tail 40`
>
> 启动方式：双击开始菜单里的 `IslandBar`（= 隐藏原生任务栏）；想两个任务栏都留着就双击
> `IslandBar (safe mode)`（带 `--keep-taskbar`，调试用）。两个快捷方式都在
> `$env:APPDATA\Microsoft\Windows\Start Menu\Programs\` 下。

---

## 1. 系统通知通道（刚做完那条）

**做**：开一个 PowerShell，粘这一行（会弹一条真的 Windows 通知）：

```
powershell -NoProfile -ExecutionPolicy Bypass -File .\IslandBar\scripts\toast-test.ps1 -Title "构建完成" -Body "0 errors, 3 warnings"
```

**应该看到**：系统通知出现在角落的**同时**，**灵动岛向左右各宽出一截**，里面显示 `ToastTest · 构建完成`
（约 6 秒后恢复成胶囊；把鼠标移到岛上则展开大卡片，可以读完整正文）。

> 变宽是**左右对称**的（对胶囊左右各 +100 DIP），开始按钮、系统按钮、右侧应用图标会自动让位 ——
> 这是 2026-10-11 按你说的"有通知的时候 灵动岛变宽 在里面显示 / 是左右宽展"改成的行为。

**日志里**：`toasts: 1 new system toast(s), 1 taken in total`

**注意**：卡片 6 秒后自动收起；点卡片 = 已读，之后不再弹。
**如果不弹**：把日志里 `toasts:` 那几行发我 —— 它会说明是"没读到数据库"还是"读到了没送进卡片"。

---

## 1b. 微信通知（第三条通道）

微信 4.x **不接 Windows 通知平台**（不在通知设置里注册、从不写通知数据库、也不用托盘气泡），
窗口是自绘的（UI Automation 读不到文字），所以这条通道只能靠**认它的弹窗**。现在的行为是：
检测到它弹通知 → 岛上显示 **“微信 · 有新消息”**（不含内容，按你的要求）。

**做**：正常收一条微信消息（手机发给“文件传输助手”，或让别人发你一条）。

**应该看到**：灵动岛变宽，里面显示 `微信 · 有新消息`；约 6 秒后收回；点一下岛 = 已读。
5 秒内多条消息只提示一次（避免刷屏）。

**日志里**（Get-Content $env:USERPROFILE\IslandBar\island_bar.log -Wait -Tail 20）：

`
wechat: window appeared <句柄> '<类名>' (x,y) 宽x高 visible=1 -> counted as a notification
wechat: announced '有新消息' (window <句柄>, 1 in total)
`

**如果没有弹出来**：把日志里 wechat: 那几行发我 —— 它会列出微信**新出现的每个窗口**以及为什么没算作通知
（尺寸不在范围内 / 位置在屏幕中间 / 窗口不可见…）。我按那几行调判据即可，不用你再描述。

---
## 1c. 微信通话（在岛上的常驻显示）

**做**：打一个微信语音/视频通话（或让别人打给你）。

**应该看到**：灵动岛保持宽着的状态，里面显示 `微信 · 通话中`；通话窗口关掉后自动收回。
点一下岛 = 已读，本次通话期间不再打扰（岛收回）。

**日志里**：

`
wechat: call window <句柄> '微信音视频通话 ' appeared
wechat: call announced as '通话中' (notification 1)
wechat: call ended (notification 1 retired)      <- 通话结束时
`

判据是那个窗口的**标题**（微信自己的标题就是"微信音视频通话"），不读通话内容、也不碰微信。

---
## 1d. 微信消息（第三条通道：靠"提示音"）

微信 4.x 在 PC 上不接系统通知、不弹窗口、也不改标题 —— 唯一的提示是**托盘图标**，而托盘在"替换任务栏"模式下**屏幕上没有任何入口**（2026-10-15 起连 IslandBar 自己也不再画它）。
所以这条通道用**声音**：微信安静时**没有音频会话**（实测 sessions 0），来消息响一声时会话就会出现。

**做**：正常收一条微信消息（手机发给"文件传输助手"最方便）。

**应该看到**：灵动岛变宽，显示 `微信 · 有新消息`（不含内容）；约 6 秒后收回；点一下岛 = 已读。

**日志里**：

`
wechat: audio baseline -- sessions 0 (was 0), active 0 (was 0), running 1
wechat: audio changed  -- sessions 1 (was 0), ...
wechat: announced '有新消息' from its notification sound (1 in total)
`

**已知会误报的情况**（不是 bug，是这条路的天性）：你自己发消息、播放语音消息时微信也会出声 → 也会提示。
**不会提示的情况**：微信里关了"新消息通知声音"，或开了免打扰。

---
## 2. 面板（点左侧 ⌃ 展开）

| 做什么 | 应该看到 | 日志里 |
|---|---|---|
| 展开后点 Codex 窗口 / 桌面（不在卡片上） | 面板自己收回 | `a press at (x,y) outside the bar` → `closed (a click outside the bar)` |
| 点"打开设置页"的瓦片（如 辅助功能） | 干净收回，**不再闪出音乐卡** | `click: tile N '辅助功能' action=…` |
| 点 **飞行模式** 瓦片 | 瓦片点亮、旁边 Wi-Fi 变 **已关闭**；再点恢复 | `Wi-Fi radio off/on (result 1, error 0 …)` |
| 按住音量滑条左右拖 | 手柄**紧跟指针**，上方显示百分比 | `volume: press on the slider -> N%` + 连续 `dragging -> N%` |
| 点滑条中间 | 手柄**走过去**（0.12 秒过渡） | `volume: set to N% (drag begins)` |
| 点喇叭 | 静音 / 取消静音 | `volume: muted / unmuted` |
| 点音量行右侧"设备 + >" | 进入"声音输出"子页 | `sound page: open` |
| 子页里滚鼠标滚轮 | 列表跟着滚 | `sound page: wheel -1 -> scroll 180` |
| 点一行**输出设备** | **会真的切换默认输出设备**（介意先别点） | `switching the default device to N …` → `switch default device ok/FAILED` |

### 2b. 展开的那一下，岛自己该纹丝不动（2026-10-12 修的那条）

**做**：点 ⌃ 展开面板，眼睛盯着**岛本身**、左边的 ⌃/⊞ 两个按钮、以及最右边的应用图标。

**应该看到**：展开的整个过程中，**岛一直是那个小胶囊**、时间留在原来的位置，面板像从栏后面
逐行滑出来；⌃/⊞ 和右侧图标**不动**。

**不该看到**（这是修之前的样子）：一块黑方块从胶囊里长出来压住面板瓦片、时间漂到黑方块正中间、
甲板和图标行横着甩出去再甩回来。

**日志里**（判断依据就是这一行，它打印的是岛填充和图标行共用的那个锚定盒）：

`
start button: island x [1170, 1390] ...
`

**在这一整段里它必须始终是 `[1170, 1390]`**，一个别的值都不该出现。修前的日志是
`[1158,1402]` → `[1112,1448]` → 最后跳回 `[1170,1390]`。

### 2c. 开卡片时，整条栏不该"暗一下再亮"（2026-10-12 修的那条）

**做**：两个场景各来一次 —— ① 点 ⌃ 开面板；② 放着音乐，把鼠标移进灵动岛（这会开音乐卡）。

**应该看到**：卡片自己淡入、岛的形状动，但**甲板 ⌃/⊞、右侧应用图标、歌词条**全程一样亮。

**不该看到**：整条栏（含图标行）一起暗下去、大约 1/3 秒后再亮回来。

**日志里**（这就是判定依据）：

`
start button: island x [1170, 1390] -> ... opacity 1.00
`

这一行**只在数值变化时打印**，所以**整段过渡里它只该出现一次**（值 = 1.00）。
修前它会连刷几十行、`opacity` 从 0.40 爬到 1.00 —— 那就是"闪一下"。

> 想看两个强度分开：设 `ISLANDBAR_TRACE_CARD=1` 启栏，卡片过渡每帧一行，
> `cardOpacity` 会从 0.35 涨到 1.00，而 **`barOpacity` 必须全程 1.00**。

> 通知纸条（第 1 节那条）也要顺带看一眼：来通知时岛**仍然左右展宽**、标题显示在纸条里 ——
> 修复动的是同一条规则，所以纸条是它唯一可能被弄坏的地方。

---

## 3. 音乐卡（有歌在放时把鼠标移到胶囊上）

| 做什么 | 应该看到 | 日志里 |
|---|---|---|
| **播放器"不开口"的时候**（它的系统媒体控制开关没开；或先用 `ISLANDBAR_NO_SESSION=1` 启动栏）（2026-10-13 HANDOFF 8.73 起） | 胶囊里**照样有歌词** —— 栏改读它的**窗口标题**（`歌名 - 歌手`）。可**没有封面**，岛也**打不开音乐卡片**（卡片用的是会话里的封面/进度/三个控制键，标题里没有） | `music: [cloudmusic.exe] 歌手 - 歌名 (from its window title -- this player publishes no session)`；歌词那几行尾上是 `(counted from the player's window title -- there is no session)` |
| **盯着岛上看一句"唱得慢"的词**（2026-10-13 HANDOFF 8.76 起） | 这一句的**词一直停在胶囊里**，直到下一句唱出来才换 —— **中间不该闪出歌名**。（很多 LRC 用"只有时间、没有词"的空行标记句尾；以前那种空行会把词顶掉、露出歌名） | `lyrics: line N …: 那句词` —— 每行都该**带词**；行号会**跳着走**（如 0,1,2,4,6,8…），被跳过的奇数就是那些空标记 |
| **放网易云的歌**（它不报时间轴）（2026-10-13 HANDOFF 8.74 + 8.75 起） | 胶囊里的歌词**一行行往上走**；卡片左边"已播"在走、右边是**目录里的真实总长**（如 `03:16`）、进度条**按总长填**。**栏是在歌的中段启动的**话，秒表从 0 开始 → 歌词与"已播"会一直差着那一段（不会自己收敛）：**按一下 ⏭/⏮ 或让栏从开歌就在跑**就重新对齐 | `lyrics: loaded (55 lines, the catalogue says 196426 ms)` —— 后面那个数是接口给的；`lyrics: line N at X ms (counted by the bar -- this session reports no position)` 里行号与毫秒都该往前走；`gsmtc_probe.exe` 对同一首歌看到的是 `Position 00:00 / EndTime 00:00` |
| 点 播放/暂停、上一首、下一首 | 歌真的跟着变 | `music: play/pause requested` |
| 看卡片右上角（2026-10-12 HANDOFF 8.29 起） | **没有喇叭、没有百分比、没有滑条** —— 整组控件按用户要求删掉了；名字那条来源行现在通到卡片右边距 | 日志里**不该再出现** `volume: press on the card's bar` / `volume: control opened` / `dragging the card's bar`；出现即回退。设备音量仍看第 2 节的面板滑条，单个应用的音量走子页「音量合成器」 |
| 放一首**歌词很长**的歌，看胶囊两头（2026-10-12 HANDOFF 8.30 起） | 字**从两端淡出/淡入**（左边正在离开的字、右边正在进来的字都变暗），中间全亮；不该再有"被圆角斜切掉半个字" | 没有日志行，看画面。判定标准是自检里量过的那条：同一个字在渐隐区读 `49.84`、紧挨其内侧读 `122.87`（关掉渐隐时两者都读 `122.8`） |
| 盯着胶囊看**歌词换句**的那一下 | **上下各半个行**：旧句往上滚出、新句从下面滚上来，两行各被一行高的窗口切掉一半 —— 这是**竖滚的定义**，不是缺陷（2026-10-12：8.31 曾改成原地淡入淡出，用户当天要求改回，见 HANDOFF 8.32） | 没有日志行，看画面。只有用户明确说"切换时不要出现两行"时才该动它 —— 那是把过渡换成原地淡入淡出的唯一理由 |
| 换一首**没有封面**的歌，把鼠标移到岛上开卡片（2026-10-12 HANDOFF 8.33 起） | 卡片是一块**深色卡面**，字和进度条都在卡面里，**不该**是"字直接飘在壁纸上" | 想确认那首歌到底有没有封面：跑 `.\IslandBar\build\Debug\tools\gsmtcprobe\Debug\gsmtc_probe.exe`，看 `Thumbnail: present/none` —— `none` 就是这首歌没发布封面，栏只能给它一块自己的底面 |
| 连续切两首歌，第二首**封面晚到**（2026-10-12 HANDOFF 8.42 起） | 第二首的封面必须**真的换成它自己的**；日志全对但画面不换，就是缓存键重复那类缺陷 | 日志正常不足以判定（`dropped`/`arrived late`/`decoded` 都会齐）；判定要看**画面**。核按钮：自检里 `a new cover with a repeated revision is not the old bitmap`（读数 `A cached 1, B cached 1, A still cached 0`）|
| 切歌时盯住卡片左边那块（2026-10-12 HANDOFF 8.41 起） | 新歌**没有封面时**显示 **♪ 占位符**（绝不能还是上一首的专辑图）；播放器随后发出封面就自动换上 | 日志：换歌那行下面应是 `music: cover NONE` + `music: cover dropped (the new track has none yet)`；若封面稍后到达会再出现 `music: cover arrived late (N bytes)` |
| 看**屏幕右上角**那一小块（2026-10-15 HANDOFF 8.134 起） | **那是系统自己的托盘**：折叠箭头、输入法、音量、时钟都在，鼠标移上去会高亮、左键右键都是真的（右键菜单、折叠区的浮出面板都是 shell 自己的）。它左边**没有任务栏**——那 2416 DIP 被裁掉了，点那里等于点桌面。**岛右边不应该再有任何三个点的圆钮**，应用图标行紧贴岛右侧 | 日志：启动时 `native taskbar: clipped to the tray -- the REAL system tray stays on screen`；shell 把任务栏抢回去时 `native taskbar: tray re-clipped`（这是正常现象，看门狗每约 0.8 秒查一次）。**回旧行为**：`--hide-taskbar`（整条藏掉，右上角什么都不要）；`--keep-taskbar` 则整条任务栏原样保留（调试用）。全屏应用在前面时托盘会跟着退场，退出全屏再回来 |
| **开一个全屏游戏**（2026-10-12 HANDOFF 8.54 + 8.55 起） | 平时顶部**什么都不该有**：没有胶囊、没有日期时间行、没有图标行、没有 Start/系统键（修之前这些会画在游戏上）。鼠标划到屏幕顶部也**不该弹出音乐卡片**。**来一通电话或一条通知时，只出现"岛"那一条**（纸条/通话条 + 里面的话），**左右两侧依旧干净** —— 那行日期时间和两边的按钮图标都不该冒出来；几秒后纸条自己收回去。**全屏时点岛**应该还能把消息消掉 | 日志：`fullscreen: an application has the screen -- the bar stands down` → 退出全屏 `fullscreen: over -- the bar is back`；全屏时来通知应有 `state: notice-strip`。**判定要点**：① 普通**最大化**窗口**不会**触发退场（它覆盖的是工作区、不是整屏）—— 若最大化窗口也让栏消失，就是 `WindowCoversMonitor` 判错了；② 全屏+通知时若左右两侧也冒出来，就是 `frame.fullscreenApp` 没传进渲染器（自检里对应 `island band=26.53, first icon=0.00, Start button=0.00`） |
| 鼠标**停在**某个应用图标上（2026-10-12 HANDOFF 8.51 + 8.52 + 8.53 起） | 停约 **350 ms** 后，图标下面弹出一张**圆角卡片**：顶部一行是**那个窗口自己的图标 + 标题**（太长会省略号），下面是**实时画面**。**所有卡片同一个尺寸**（276×198 DIP，9 个图标都一样大、离栏距离也一样），窗口的比例只体现在画面里：画面在槽内等比缩放并居中，**窄窗口左右留卡面、宽窗口上下留卡面**（不会变形、也不会撑大卡片）。**已经在显示时，顺着图标横移会立刻换成隔壁那个**（不等第二次 350 ms）；鼠标离开图标行就消失。**扫过一排图标时不该每个都闪一下**（只有停住才出）。卡片（音乐卡/面板）开着时不显示。预览本身**点不进去**（穿透） | 日志：`preview: hwnd … -> card x,y WxH, picture WxH (compositor says AxB), shown=1, hr=0x00000000`。**三条判定**：① `card` **必须恒为 276x198**（不是=new 大小不一致）；② `picture` 的宽高比要等于 `compositor says` 的宽高比（例如 `247x160` vs `1282x829`：1.544 vs 1.546）= 没压扁；③ `shown=0` 或 `compositor says 0x0` = 这个窗口画不出来（提权窗口/跨桌面）。想单独验证配方：`.\IslandBar\build\Debug\tools\thumbprobe\Debug\thumb_probe.exe`（复制到项目树外再跑）—— 它会显示两个自己的窗口，**绿色**出现即说明这台机器的 DWM 缩略图可用 |
| 放一个**全屏应用**（浏览器按 F11）（2026-10-12 HANDOFF 8.50 起） | 顶部条带被一条满宽深色条填掉，**高度等于你任务栏的厚度**（改任务栏大小/用别的机器都跟着变，不再是写死 32）；退出全屏恢复胶囊 | 日志：`fullscreen: over, back to the pill`；`--appbar` 时还有 `appbar: ABM_SETPOS asking for 0,0 2560x42` → `registered -- work area was …, now …`，退出时 `appbar: released (work area restored)` |
| 看 `星期 · 日期 · 时间 · 天气` 这一行长什么样、出现在哪（2026-10-12 HANDOFF 8.45 + 8.46 + 8.48 + 8.49 起） | **一条，两个位置**。样式按参考图（日历图标已按你要求去掉）：`周一 10月12日 | 17:49 | [云雨] 小雨 17°` —— 周一是亮的粗体、日期更暗更小、时间最大最亮、两条细竖线分格、天气前面是蓝色云雨图标。**空闲**在胶囊里面，**放音乐 / 有消息 / 来电**时挪到 ⌄ 系统按钮**左边那条带子里**（同一条，图标也在），**淡入**而不是弹出。鼠标划过它不反应（箭头、点下去穿透）。**关掉播放器**才回到岛内（有会话就算占用，**暂停不算**） | 没有"显示在哪"的日志行，看画面；判定标准是自检量过的：`widest run 195.0 DIP; pill 204.0 available, band 252.0`（两条 fit + "胶囊才是紧的那个"）+ `StatusPlacementFor` 的"两种情况下都恰好一个家"。天气（2026-10-12 HANDOFF 8.47 起**默认走系统定位**）：`weather: position from Windows -> 41.775863, 123.442448` → `weather: 16.7 C, code 51 -> 小雨 17°`；状态行 `state: music-strip` / `notice-strip`。定不到位置时日志会写明原因（`location is denied for this program` / `no fix (0x…)`），那就改 `%USERPROFILE%\IslandBar\weather.txt`（**取消注释**一行城市名，或写 `39.9042,116.4074`）—— 文件里**有**一行时它覆盖定位，重启栏或等 15 分钟。要单独确认定位：`.\IslandBar\build\Debug\tools\geoprobe\Debug\geo_probe.exe`（复制到项目树外再跑，见 START-HERE）。要在有音乐时看空闲态：`ISLANDBAR_NO_MEDIA=1` 起一份诊断实例 |
| 手动切歌后**封面没出来**（2026-10-12 HANDOFF 8.44 起） | 先别怪栏：日志里若持续出现 `music: cover retry N -> the player still has none`（每约 2 秒一行，最多 5 行），说明**栏在问、播放器没给** | 再跑 `.\IslandBar\build\Debug\tools\gsmtcprobe\Debug\gsmtc_probe.exe` 看同一首歌是不是 `Thumbnail: none` —— 是，就是播放器没有；若 `present` 而栏一直 `retry ... none`，那才是栏的读取问题，告诉我 |
| 放歌时**别把鼠标移到岛上**，等它自然换歌（2026-10-12 HANDOFF 8.43 起） | 胶囊里那条**声波的颜色应该跟着新歌变**（以前不动鼠标就一直是上一首的颜色）| 日志：换歌后应出现 `music: cover decoded 宽x高`，且那一帧前后**没有** `state: expanded`（= 卡片没打开也解码了）|
| 切歌后**封面还是空的**（2026-10-12 HANDOFF 8.40 起） | 应在一两秒内自己补上；日志里出现 `music: cover arrived late (N bytes)` 就是补上了 | 若仍是空的，跑探针看这首歌到底有没有封面：`gsmtc_probe.exe` 的 `Thumbnail: present/none`。**present 而栏写 `cover NONE` = 栏的读取出了问题**（8.40 修的就是这个：长度为 0 的流以前会被拒绝） |
| 在**QQ音乐自己的窗口里**点切歌（2026-10-12 HANDOFF 8.39 起） | 新歌标题一秒内换上；封面若播放器晚发，**一两秒内自己出现**，不需要重启栏 | 日志：`music: cover NONE (0 bytes)` 之后若出现 **`music: cover arrived late (N bytes)`**，就是"晚到的封面被接住了"（这正是以前必须重启才能看到的那种）；轮询行距应 ≈1 秒（旧版是 2 秒） |
| 点卡片上的 ⏭ ⏮ 切歌（2026-10-12 HANDOFF 8.38 起） | 新歌的**标题、封面、歌词**在**一秒内**换过来（以前要 2-3 秒） | 日志：`music: next requested` → `music: transport next accepted` → 新歌那行的时间差。跳曲后 4 秒内轮询是 ~250 ms 一次，所以新歌一出现就会被读到 |
| 遇到**没有封面**的歌（封面上是个 ♪ 占位符） | 这说明**播放器没发封面**，不是栏丢图 | 两个证据：日志里 `music: cover NONE (0 bytes)`（而不是 `present` + `decoded`）；`.\IslandBar\build\Debug\tools\gsmtcprobe\Debug\gsmtc_probe.exe` 显示 `Thumbnail: none` |
| 鼠标从**卡片上的按钮**（⏮ ⏸ ⏭）、**图标行**、**开始/系统键**、**面板磁贴和滑条**上慢慢划过去（2026-10-12 HANDOFF 8.37 起） | 这些地方是**小手**；胶囊本身、卡片上的封面/文字、卡片外的空白是**箭头** | 没有日志行，看光标。想复核：自检里 7 条断言（738 项全绿），或临时把指针挪到某点再读 `GetCursorInfo` 与 `LoadCursor(IDC_HAND)` 是否同一个句柄 |
| 想知道**封面到底有没有取到**（2026-10-12 HANDOFF 8.36 起） | 日志里每首歌各一行：`music: cover present (N bytes, revision R)` + `music: cover decoded 宽x高` = 拿到了；`cover NONE` = **播放器没发**（探针也会显示 `Thumbnail: none`）；`COULD NOT DECODE` = 发了但栏解不开 | 三个证据按顺序看：① `.\IslandBar\build\Debug\tools\gsmtcprobe\Debug\gsmtc_probe.exe` 的 `Thumbnail:`；② 栏日志的两行 `music: cover`；③ 都不对才看渲染/卡片截图 |
| **打一通微信电话**，然后**别碰岛**，等一条消息进来（2026-10-12 HANDOFF 8.34 起） | 通话条（头像+绿计时+电平条）出现 → 消息顶掉它 → **消息 6 秒后通话条自己回来**，整通电话都在；点一下岛则只消掉消息，**通话留着** | 日志：`wechat: call announced as '通话中'`；**不该**在通话还在时出现 `state: collapsed`。没有真人电话时用 `scripts\fake-call-window.ps1`（把 `powershell.exe` 复制成 `WeixinCallProbe.exe` 再跑它，窗口在屏幕外） |
| 让纸条显示一句**长英文**（例如 `--notify "hello there my friend again" --app WeChat`） | 一句话**在一行里**（放不下就滚动），**不该**出现"上面半句 + 下面半个词" | 没有日志行；判定与自检同一条：墨迹必须是**一段**，折行是两段（`16 rows, 2 band(s)` 是缺陷、`13 rows, 1 band(s)` 才对） |
| 放着音乐去**点 ⌃ 开面板**（2026-10-12 修的那条） | 胶囊里**仍然是歌词**（没有歌词就是歌名），**不是时间**；面板自己开着 | 日志没有专门一行，看画面即可 —— 判据见 8.12：有卡片时胶囊那条带的像素必须和没卡片时**完全一样** |
| 放着音乐：鼠标移进岛（出卡片）后**快速左右横移**（2026-10-12 修的那条） | 歌词**待在胶囊里不动**，不该往卡片中间掉一下再回来 | 日志里不该出现 `WARNING: the bar's own line is being drawn in a box taller than the bar` —— 这条告警只在"栏自己那一行画进了比栏更高的盒子"时报一次 |

### 3b. 歌词到底有没有拿到（2026-10-12 修完那条）

**做**：放一首歌，看胶囊里那行字；同时看日志。

**日志里**（判定依据）：

`
lyrics: loaded (43 lines)                                   ← 拿到了
lyrics: none (0 lines)                                      ← 这首歌确实没有歌词（正常，只该出现一次）
lyrics: netease sent no original -- using its TRANSLATION    ← 用的是译文（原文为空）
`

**关键**：`none` 在这首歌里**只该出现一次**。修之前每次都重试 3 次（连刷三行 `none`），
因为"原文为空、文字在译文里"被当成了一次可重试的失败。

### 3c. 歌词跟得上、而且换行有动画（2026-10-12 修的那条）

**做**：放一首**有歌词**的歌，盯着胶囊里那一行。

**应该看到**：歌词**换行时是从下往上滚动**（新的那行从下面升上来、旧的那行往上滚出去，约 0.26 秒，
缓出收尾），滚动是**在一个"一行高"的窗口里**进行的 —— 所以任何时刻最多只看到一行文字的量，
不会出现两行挤在一起的重影。换行的**时刻也要和唱到的位置一致**（不再是晚一两秒"啪"地跳一下）。

> 如果歌词整体**早/晚一段固定时间**（不是偶发），那多半是 LRC 自带的 `[offset:N]` —— 8.16 之前
> 我们忽略它，现在会应用。若还有固定偏差，把歌名发我，我用 `-LyricsProbe` 看原始 LRC 里的 offset。

**还有一个计数**（8.17 修的那条）：日志里 `lyrics: line N at …` 这行**每次换行打印一次**，
所以同一行号**连着出现两次**就是异常（说明"没换行却被当成换行"，屏幕上会出现两行同样的字滚在一起）。
判据：拿最近的播报序列，**行号应该是递增的、没有相邻重复**。修前实测 183 次里 96 次重复，
修后 30 秒里 5 次、0 次重复。

**日志里**（判定依据，每次换行一行）：

`
lyrics: line 7 at 51230 ms (the session last reported 49800 ms)
`

括号里是 SMTC 上次上报的原始值，前面的才是**推算到当前时刻**的值 —— 两者会差 0.5～2 秒，
**那个差值就是以前会晚到的量**。另外报告行里 `redraws: ... lyrics=N` 应当**大于 0**（修前一直是 0：
连入场动画都没有重绘理由，所以什么都不会动）。

想单独试一首歌的抓取（不碰你的栏，只发两次 HTTPS）：

`
powershell -NoProfile -ExecutionPolicy Bypass -File .\IslandBar\scripts\verify-panel-anchor.ps1 -LyricsProbe -Title "歌名" -Artist "歌手"
`

它会打印网易云与 QQ **两个服务各自的** URL、HTTP 状态、响应前 400 字节和各自结论。

---

## 4. 开始按钮（岛左边那个 Windows 标）

**做**：点它。**应该看到**：开始菜单弹出（再点收起）。
**日志里**：`start click: sent`（或 `sent (keybd_event)`）。
**如果不弹**：日志会写 `this process is BELOW the shell's integrity level …` —— 栏的权限比 Explorer 低，
Windows 把合成按键丢了；必须用那两个快捷方式启动，别从 IDE / 沙箱 / 别的终端里启动。

---

## 5. 通知卡片（本机推送通道）

**做**：

```
& "$env:USERPROFILE\IslandBar\island_bar.exe" --notify "标题" --body "正文" --app "我的工具"
```

**应该看到**：卡片弹出，来源行左边有 **"我的工具" 的图标**（取自发通知那个进程）。
**日志里**：`notification: [我的工具] 标题` 与 `notification: icon from '…' -> 32x32…`

---

## 6. 出问题时

---

## 5c. 系统面板：亮度条 + 三个开关（2026-10-14 加的那批）

**做**：点栏上系统按钮打开面板。应该看到：

1. **音量行下面多了一条"太阳 + 轨道"的亮度行** —— 拖它，屏幕亮度跟着动，拖动时轨道上方显示百分比。
   日志里：
   ```
   brightness: press on the slider -> 62% (asked 62, landed)
   brightness: dragging -> 55% (asked 55, landed)
   brightness: drag ended at 55%
   ```
   如果启动日志里有 `the monitor did not answer its capabilities`，说明显示器菜单里 DDC/CI 被关了
   （LG 默认开），面板上**不会**有这一行。

2. **点蓝牙那一格**（以及旁边**第一格 Wi-Fi**）：无线电真的开关（格子亮/灭）。日志：
   ```
   radios: '蓝牙' (bluetooth) -> asked off, answered 1, now off
   radios: Bluetooth off -- as asked
   ```
   Wi-Fi 那一格现在**也是开关**（以前是打开设置页）：亮 = 无线电开着；下面写 未连接（开着但没连上，
   这台机器走网线，所以一直是这个）/ 可用（连上了）/ 已关闭（无线电关了）。两格都没有箭头了，因为
   它们是原地开关、不展开列表（辅助功能那些还有箭头）。

3. **点飞行模式那一格**：Wi-Fi 和蓝牙**两个一起**关/开（以前只关 Wi-Fi）。

4. **翻到第二页**（面板右边的向下箭头），点**夜间模式**：会先弹一下"设置"窗口（Windows 没有别的接口），
   约 2–3 秒后它自己关掉，格子变亮，屏幕变暖。日志：
   ```
   night light: pressed 'turn it on now' (the page said off)
   night light: the page now says on -- the press took
   ```

5. **点实时字幕**：字幕条出现（第一次会弹它自己的同意框，那是 Windows 的行为）；再点一次关掉。
   日志：`win+ctrl+L: chord sent (keybd_event)`。

**留意一条**：夜间模式那格下面的"开/关"字，正常情况与系统一致；万一它与实际不符（我探针里手写过那个值，见过
一次不一致），说一声 —— 面板里的开关本身仍然正确，它按设置页自己的状态走。

---

## 5d. 外观与最大化窗口

外观现在是**固定的 Niri 材质**：没有主题选择、没有主题循环，也不读取旧 `theme.txt` 或自定义主题目录。
启动日志应只有：

```
theme: fixed Niri material (0 selectable themes)
```

下面这条是最大化窗口的历史行为，仍然有效。

**做**：把任意一个窗口最大化（双击标题栏，或 Win+↑）。

**应该看到**：窗口内容从屏幕**最上面 y=0** 开始铺满整块屏幕，**岛浮在它上面**；岛的左右两侧
**不再有空白条**（空白条 = 系统任务栏原本占的那一条）。

**日志里**（启动时应该已经有这两行，这是"让出去"发生的证据）：

```
  work area: the strip is released -- 0,32 - 2560,1440  ->  0,0 - 2560,1440
```

**如果启动时最大化过的窗口还留着一条空白**：正常 —— 让出去之后**已经最大化**的窗口不会自己动，
栏会把它搬一次，日志里会出现：

```
  work area: 1 already-maximized window(s) brought to the full screen
```
看到这行却还有空白，就手动还原再最大化一次（Win+↓ 再 Win+↑）。

**如果某个窗口最大化后栏"退场"了**（只剩岛，图标行也不见）：那是把这条 32 px 宽的
窗口当成了全屏应用。目前已知会这样的是**画不出边框**的那种窗口（见 FEATURES §二十六 的边界）。
日志里对应 `fullscreen: an application has the screen` —— 把是哪个应用告诉我就行。

**不想要这个行为**（想让最大化窗口照旧停在条下方）：用 `--keep-strip` 启动。

**如果一切都乱了**（栏被任务管理器强杀之后，任务栏一直不回来）：

```
powershell -NoProfile -ExecutionPolicy Bypass -File .\IslandBar\scripts\restore-desktop.ps1
```

它会把 shell 任务栏显示回来、把工作区按任务栏算回去；只有在两样都做不到时才重启 Explorer。

---

```
# 最近 20 行日志
Get-Content $env:USERPROFILE\IslandBar\island_bar.log -Tail 20

# 全部自检（当前 1228 项；退出码 0 == 全过）
& "$env:USERPROFILE\IslandBar\island_bar.exe" --self-test

# Windows 侧最新的真通知（这条通道的调试口）
& "$env:USERPROFILE\IslandBar\island_bar.exe" --toast-dump 5
```

---

## 5e. 安装包：装一遍 / 卸一遍（2026-10-14，HANDOFF 8.97）

**包在哪**：`.\IslandBar\dist\IslandBarSetup-1.3.exe`
（本机还有一份在 `Downloads`，那正是"用户会双击的位置"）。

**装**：双击它。不需要管理员（只写你自己的目录和 `HKCU`），结束时会弹一句"装好了"。

**应该看到**（装完立刻）：

* 系统任务栏消失，顶部出现灵动岛（如果栏本来就在跑，它会先被关掉、再换成装好的那一份）；
* 开始菜单里有 `IslandBar` 和 `IslandBar (safe mode)`；
* `设置 → 应用` 里有 `IslandBar 1.3`，可以在这里卸载；
* **下次登录它会自己起来**（启动文件夹里有它的快捷方式）。

**日志**：`%TEMP%\IslandBarSetup.log`（那个目录写不进去时会落在安装程序旁边）。最后两行应该是
`registered under Settings > Apps (HKEY_CURRENT_USER)` 和 `the bar has been started`。

**卸**：`设置 → 应用 → IslandBar → 卸载`。**你的设置和固定区会被保留**
（`weather.txt`、`start_folder.txt`、`Start\` 里那些快捷方式）。想连它们一起清掉，手动跑
`& "$env:USERPROFILE\IslandBar\IslandBarUninstall.exe" /uninstall /remove-settings`
—— 固定区是**进回收站**，不是永久删除。

**卸完应该**：任务栏回来了、开始菜单两个快捷方式没了、`设置 → 应用` 里也看不到 IslandBar 了。

**装到别的电脑上**：那台机器只要能跑 Windows 11 就够了（运行库是静态链接进 exe 的）。
第一次运行会被 SmartScreen 拦一下（包没签名）：点 `更多信息 → 仍要运行`。
拿不准就先双击 `IslandBar (safe mode)` —— 它**不隐藏**系统任务栏。

---

## 7. 内存 / 句柄泄漏：让同一件事做几十遍，看计数有没有涨

这条是**机器自己报**的，不需要外部工具：栏每隔一段时间打一行 `mem[...]`，
里面除了字节数还有**句柄、线程、GDI、user 对象、位图**的计数。判读规则（`core/ProcessMetrics.h` 里写着）：
**计数和字节一起涨 = 泄漏；只有字节涨 = 缓存填满了**。

默认那行每 **5 分钟**才打一次，做泄漏测试等不起，所以有一个开关把它调快：

```powershell
# 让栏每 5 秒报一次，并且每 2.5 秒开关一次系统面板，跑 200 秒（约 40 个来回）
$env:ISLANDBAR_MEM_MS='5000'
$env:ISLANDBAR_PANEL_MS='3000'; $env:ISLANDBAR_PANEL_CLOSE_MS='5500'; $env:ISLANDBAR_PANEL_CYCLE_MS='2500'
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\regression\run-demo.ps1 `
    -Name leak-cycle -Seconds 200 -Deploy -StopRunning

# 换成托盘页（每格图标都要从注册表解码一张 PNG）再跑一遍
$env:ISLANDBAR_TRAY_PAGE='1'
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\regression\run-demo.ps1 `
    -Name leak-tray -Seconds 200 -Deploy -StopRunning

# 然后看序列，不看单个数
Select-String -Path .\scripts\regression\out\leak-cycle-raw.txt -Pattern 'mem\[' | ForEach-Object { $_.Line }
```

**2026-10-16 实测（两轮各 40 个来回）**：句柄 `1081 → 960`、线程 `71 → 54`、GDI **全程 41 不变**、
user 对象 `25 → 13`、位图 0；常驻字节在前 5 秒涨到 `~90 MB`（封面 / 歌词 / 托盘图标那几份缓存落地），
之后 3 分钟里在 ±0.5 MB 内平着。**句柄是先降后平**（托盘学习结束、线程退出），不是涨——涨才是泄漏。

**为什么不能用"卡片关闭后那行"代替**：那行自带 30 秒延迟，而卡片每关一次就把这 30 秒重新推后一次，
所以"每 2.5 秒开关一次卡片"这种循环**一行都不会打**（实测：200 秒 40 个来回，0 行）。

---

## 8. 全屏游戏同时放歌：CPU 降载验收

同一款游戏、同一条 `island_bar.exe` 运行实例、同一首歌下连续采样 20 秒：

```powershell
$p = Get-Process island_bar
$t0 = $p.TotalProcessorTime
$w0 = Get-Date
Start-Sleep -Seconds 20
$p.Refresh()
$dt = ((Get-Date) - $w0).TotalSeconds
$used = ($p.TotalProcessorTime - $t0).TotalSeconds
"island_bar one-core: {0:N2}%" -f ($used / $dt * 100)
```

验收数字：单核占用从 2026-11-13 的基线 **20.45%** 降到不超过 **12.0%**。
日志必须出现：

```text
throttle: ... frames=20.0/s ... hud=music cap=20fps
```

歌词换句、切歌仍应立即更新；普通桌面帧率不变；全屏来电仍按原来的通话路径显示，不受音乐 HUD 的 20 Hz 限制。

---

## 9. 媒体封面线程泄漏 A/B

这一步只隔离封面缩略图读取，不关闭媒体状态、歌词、音量或播放控制：

```powershell
$env:ISLANDBAR_NO_ARTWORK='1'
& "$env:USERPROFILE\IslandBar\island_bar.exe"
```

在 QQMusic 中连续切歌至少 20 次，每次切歌后等 5 秒，再静置 60 秒。日志开头必须有：

```text
music: artwork fetch disabled by ISLANDBAR_NO_ARTWORK=1; media state and lyrics remain active
```

验收数字：`island_bar.exe` 线程数稳定在 **90** 以内，且不随切歌持续增长。

若关闭封面后线程仍增长，移除环境变量并重跑；下一组实验只关闭每 2 秒的 media manager 重建。两组不能同时修改，否则无法确定根因。

---

## 10. 媒体 Manager 周期重建 A/B

封面读取保持正常，只关闭每约 2 秒的 `MediaSessionManager::RequestAsync()` 兜底重建；由
`PlaybackInfoChanged` 触发的事件刷新仍然保留：

```powershell
$env:ISLANDBAR_NO_MANAGER_REFRESH='1'
& "$env:USERPROFILE\IslandBar\island_bar.exe"
```

日志开头必须有：

```text
music: periodic media-manager refresh disabled by ISLANDBAR_NO_MANAGER_REFRESH=1; event-driven refresh remains active
```

用 QQMusic 连续切歌并运行至少 30 分钟。验收数字：`island_bar.exe` 线程数保持在 **90** 以内，
private bytes 不再随运行时间单调增长。

若仍然增长，移除两个诊断变量并改用 `ISLANDBAR_NO_MEDIA=1` 对照整条媒体会话链；若稳定，则正式修复
应缓存 manager，并只保留低频、可取消的安全兜底。

---

## 11. 并发、通知管道与窗口图标重试

这套检查已包含在 `--self-test` 里，不需要另开测试程序：

```powershell
# 必须把 exe 和 DLL 复制到项目树外再跑（%USERPROFILE%\IslandBar 或 %TEMP% 下的目录）
& "$env:USERPROFILE\IslandBar\island_bar.exe" --self-test
```

验收行：

```text
[ OK ] Stop() cancels a connected client that sends no data
[ OK ] Stop() cancels a partially received frame
[ OK ] a complete framed notification is acknowledged response='{"ok":1,...}'
[ OK ] a failed window icon retries with finite 500/2000/5000 ms backoff
[ OK ] window-icon retries wait for their deadline and stop at the attempt limit
[ OK ] window-icon identity rejects a reused HWND from another process
[ OK ] the worker keeps its shared owner alive after the UI releases it
[ OK ] the owner is destroyed exactly once after the worker resumes
[ OK ] the notification client times out when no acknowledgement arrives
```

当前本机完整自检是 **1228 项：1228 OK / 0 FAIL**。

通知管道如果停止超时，日志必须出现：

```text
worker thread: stop timeout state=retained thread=alive prepare=blocked
```

这表示旧线程尚未退出，后续 `Start()` 会被拒绝；不要关闭它仍可能使用的句柄。

---

## 12. 窗口图标失败后的恢复

第一次提取失败时，日志应出现：

```text
window icon: hwnd 0000000000000001 attempt 1/4 failed; retry in 500 ms
```

若第二次或后续提取成功：

```text
window icon: hwnd 0000000000000001 recovered on attempt 2/4
```

若一直失败，只会再出现一条：

```text
window icon: hwnd 0000000000000001 no icon after 4 attempts; retries exhausted
```

验收要求：同一 HWND 的 `attempt ... failed` / `retries exhausted` 合计不超过 4 次；
不会每帧重复执行 `WM_GETICON / OpenProcess / QueryFullProcessImageName / 图标解码`。
窗口离开任务栏行后仍应出现 `window icons: kept=... released=... capacity=32`。

---

## 13. TrayHook `DllMain` 与 Explorer 重启

`island_tray_hook.dll` 的 `DllMain` 只调用 `DisableThreadLibraryCalls`。模块被加载但 Hook 回调尚未执行时，
不应创建或追加 `%USERPROFILE%\IslandBar\tray_learned.txt`。第一次 Hook 初始化后才应出现：

```text
attached pid ... exe Explorer.EXE
hooked pid ... exe Explorer.EXE: ...
```

Explorer 重启验收（会短暂隐藏任务栏，已打开的 File Explorer 窗口会关闭）：

```powershell
$old = (Get-Process explorer | Select-Object -First 1).Id
Stop-Process -Name explorer -Force
Start-Sleep -Seconds 3
Start-Process "$env:WINDIR\explorer.exe"
Start-Sleep -Seconds 15
"old=$old new=$((Get-Process explorer | Select-Object -First 1).Id)"
Select-String "$env:USERPROFILE\IslandBar\island_bar.log" -Pattern 'new process|tray learn|observer is up' |
    Select-Object -Last 20
```

新 shell 必须出现且仅出现一次：

```text
tray learn: the shell is a new process (pid ...) -- offering the module to it again (background)
tray hook: the observer is up in the shell ...
```

并最终完成一次 `tray learn: ... registration(s) captured ...`。原生 `Shell_TrayWnd` 随后应重新不可见。

---

## 14. 图形驱动重置（device-lost）

先记录正在运行的 PID 和日志时间，再发送 Windows 标准的图形驱动重置快捷键：

```powershell
Get-Process island_bar | Select-Object Id,StartTime
Get-Date
```

按 `Win+Ctrl+Shift+B`。屏幕会短暂闪烁并可能蜂鸣，这是驱动重置本身。

验收：

* PID 不变，`Get-Process island_bar` 仍为 `Responding=True`；
* 顶部胶囊、任务窗口图标和卡片随后能继续绘制；
* 重置当下允许出现一次系统级停顿，但后续 `interval ... hitches` 应回到 0；
* 60 秒资源采样中 private bytes、handles、threads 不得持续单调增长。

2026-11-23 实测：PID `31504` 保持不变；重置时 `taskbar-check 846.7 ms`、
`wechat-sound draw 2481.0 ms`，随后约 36 帧/秒恢复。之后 60 秒 private `83.72 -> 83.46 MB`、
handles `1094 -> 1025`、threads `74 -> 66`。

本阶段验收范围是**单显示器**。多显示器暂不作为阻塞项；真实 DPI 切换仍需在单显示器上单独验收。

DPI 验收必须确认实际值已经生效，不能只看设置页显示：

```powershell
# 期望缩放 125% 时读到 120；期望 100% 时读到 96
Get-ItemProperty 'HKCU:\Control Panel\Desktop\WindowMetrics' -Name AppliedDPI
```

2026-11-23 尝试：设置页选择 125% 后，`AppliedDPI` 与 effective DPI 仍为 96，Windows 提示需要
关闭重开或重新登录，因此该次不计通过；设置已恢复为 100%。真正的 `WM_DPICHANGED` 验收应在新登录
会话中完成。
