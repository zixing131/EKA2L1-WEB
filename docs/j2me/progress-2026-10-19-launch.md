# J2ME 原始启动链回归（2026-10-19）

接 [当日汇总](progress-2026-10-19.md)。此前文档提到的 PropertyL、ifeui、journal.db 修改在本次开始时并不在工作区；本次重新实现并实际跑了 Nokia 5320 ROM + Bounce Tales 2.0.14。

最新进展：已恢复首页「＋ → 安装 Java ME 程序（.jar）」入口，并修复后文记录的音频初始化阻塞。Bounce Tales 已显示主菜单和 Chapter 1: Misty Morning 章节选择。下面先保留定位过程，最新修复见文末。

## 已验证的安装与启动链

在 `?j9raw=1` 下，ROM 安装器已经完成真实安装，随后通过真实 AppArc → StubMIDP2 → AMS 启动 IBM J9。没有恢复 host MIDP 解释器、J9 parking hook 或 AMS Find/Launch 伪返回。

修复内容：

- SIS registry 子会话 opcode `0xC` 实现 `PropertyL`：arg0 整数 key、arg1 `TPckg<TInt>` 输出；缺失属性返回 `KErrNotFound`。
- raw 模式禁用 `ifeui.dll`/`midp2installerplugin.dll` 的提前返回补丁，保留真正的 RFs 初始化；不再创建 0 字节 journal.db，也不把 SWInst 失败码改成成功。
- AppArc 的 NonNative 扫描路径补上盘符冒号，并只扫描 `*_reg.r??`，避免把 `_loc.rsc` 当注册文件解析。非原生逻辑应用路径也使用完整 `C:\...`。
- raw 安装状态等待本次目标注册真正进入 AppArc。ROM 的 silent installer 是目录监视器，安装完成后可以继续常驻，不能以进程退出作为唯一完成条件。
- 按 guest `m.jad` 中的名称、vendor、version 找套件，再读同目录 `uids` 的首个 MIDlet AppArc UID，避免仅按显示名称匹配。启动时使用注册资源原始 opaque 数据；不再重建假套件、不使用目录 UID 作为 AMS ID。
- raw 路径不回退到 host 注入或直接 J9 启动。已有其他目标 J9 时返回失败，避免把旧进程当作新目标。

真实安装产物包括：

```text
C:\private\102033E6\MIDlets\[xxxxxxxx]\m.jar
C:\private\102033E6\MIDlets\[xxxxxxxx]\m.jad
C:\private\102033E6\MIDlets\[xxxxxxxx]\uids
C:\private\10003a3f\import\apps\NonNative\Resource\xxxxxxxx_reg.rsc
C:\private\10003a3f\import\apps\NonNative\Resource\xxxxxxxx_loc.rsc
C:\private\101F875A\journal.db
```

套件目录和 AppArc UID 在不同回归中会变化，不能写死。一次回归的 `uids` 为两个小端整数 `0x10178140, 0x101bc87d`；AppArc opaque 则为 `1, 1`（AMS 内部 suite/MIDlet ID）。两者不是同一套编号。

构建 `7ee94994ebad` 的真实命令行示例：

```text
-path c:\private\102033e6\midlets\[xxxxxxxx]\ -jad m.jad -jar m.jar
-msid 1 -msin 1 -suitename "Bounce Tales" -ccid 1 -app RMIDlet
```

J9 输出 IBM J9 2.3 banner 和 `MIDlet: Bounce Tales`，建立 `!MIDletSuiteAMS[1.1]` 和 LCDUI 服务。到此只证明安装与 VM 启动，不能等同于游戏画面成功。

## J9 初始化后的线程同步问题

上述构建随后停在黑屏/状态栏。诊断中多个 J9 线程等待 mutex 1848，holder 为处于 `wait_condvar` 的 Main，且此前已有“非持有者 Signal”和“Signal unowned mutex”错误。

代码核验发现三个关联问题，已修改：

1. mutex ownership handoff 后递归计数仍为 0；实际 SVC 已推进 PC，不会重新执行 Wait 来补计数。
2. condvar 唤醒后先使线程 ready，再尝试 reacquire。原 `scheduler::wait` 对 ready 线程不出队，导致线程虽然标记为 wait_mutex，却仍能被调度运行。
3. condvar reacquire 被其他 holder 阻塞时，仍立即用 waiter 的 saved count 覆盖当前 holder 的锁计数。现在保存在线程上，只在真正取得锁时恢复。

`wasm_debug_dump` 增加 mutex ID、递归计数、holder 和 holder state，供后续核验等待链。

修复后构建 `19ab3d54648e` / `3c905b18229b` 已实际显示 Bounce Tales 的棕色背景和黄绿色加载动画；窗口自然变为 active/focusable，无须强制焦点。此前 mutex 非 holder/unowned Signal 错误消失。原生计算器也能正常显示。

## 加载动画之后的定位

尚未验证游戏菜单或关卡。加载线程 `J9_*_0000000b` 停在 `User::WaitForRequest`，调用栈包含 `euser.dll:0x801A0EE1` 和 `j9_23_multimedia11.dll:0x81AC1DA1`。对本机 RM-409 ROM 的反汇编确认，前者是 `CActive::Cancel` 在 `DoCancel` 返回之后等待原异步请求；后者属于 multimedia 对象析构。

该 multimedia 对象包含 `CRemConInterfaceSelector`。ROM `remconclient.dll:0x803CAEB8` 的 ReceiveCancel 使用 opcode 14，与现有 HLE 枚举一致。RM-409 `User::WaitForRequest` 的 status 指针保存在 r5，r4 是等待计数；诊断应按 r5 读取。

构建 `178fc5b23d2d` 的解释器对照运行进一步确认：

- RemCon opcode 4、19、13 到达 HLE；Receive 的 status 为 `0x007916F4`，调用线程是 `!jes-*-java-mma`。
- 停住的 J9 `_0b` 线程 r5 也是 `0x007916F4`。前一字为 RemCon receiver vtable `0x803CAB58`，status=`0x80000001`（pending）、flags=3。opcode 14 没有到达 HLE。
- 同一个 receiver 在多媒体线程上建立，却在另一线程的异常清理中析构；其 session 为线程私有句柄。此时不能靠在 RemCon server 中额外完成请求来修复，因为 cancel 没有到达服务端。不要放宽跨线程句柄权限或强制清除 pending。
- 触发异常的链是 `j9_23_amms.dll` → `EnvironmentalReverbUtility.dll` → `CustomInterfaceUtility.dll`。后者 `0x82CF7760` 查询自定义接口，`0x82CF77D2` 因输出中没有有效接口目标而 Leave(-1)。`src/patch/mediaclientaudio/src/player.cpp` 的 `CustomCommandSync` 只返回 0，没有填充输出。
- 该 ROM 构造器忽略 CustomCommandSync 的返回值，单纯改成 `KErrNotSupported` 不能解决此异常。需要真正实现相应 MMF 自定义接口/可选音效能力，再验证 AMMS 初始化；本次没有伪造接口 handle，也没有修改 J9。

本机缺少 `bldmake`/`abld`/`elf2e32` 和 Symbian SDK，不能直接重建 `mediaclientaudio_general.dll`。现有构建入口是 `src/patch/build.py`，general 目标要求 `S60_5th_Edition_SDK_v1.0:com.nokia.s60`；Web 会把 `src/patch/mediaclientaudio/group` 下的预编译 DLL 打入 data 包，仅修改 C++ 源文件不会改变运行结果。

长时间运行另有浏览器主线程失去响应的现象，JIT 和 `jit=0` 都观察到，原因未定。前面的 CActive pending 状态是在浏览器仍响应时采集，不能把两个现象合并为同一个已确认问题。回归脚本对最终诊断/截图设置超时，避免这种情况下无限等待。

## 可重复运行

```sh
JOBS=6 bash buildscript/build_release.sh
python3 buildscript/serve_regression.py 18091
python3 buildscript/test_j2me_launch.py \
  --rom /absolute/path/SYM.ROM \
  --rpkg /absolute/path/SYM.RPKG \
  --jar /absolute/path/BounceTales.jar \
  --output /tmp/eka2l1-j2me --observe 60
```

测试脚本需要 Python Playwright 和 Chromium。页面使用 MEMFS，不读取或覆盖正常启动器保存的设备。`autolaunch=0` 防止同时启动计算器；脚本记录 console、AppArc 列表、runtime 和截图。脚本 PASS 的范围是“guest 安装提交、AMS 拉起仍存活的 J9”；画面和按键需另外核验。

可加 `--launch-via-app-uid` 覆盖真实 AppArc UID 的入口；脚本从 guest 套件的 `uids` 文件读取该 UID，不依赖页面显示重复条目。默认入口是 host 列表的 virtual UID，二者最终都必须进入真实 guest AMS 启动链。

最终构建 `fc41009787cd` 的验证：Release 构建、`git diff --check`、Python 语法、smoke 页 JavaScript 语法检查通过。原生 Calculator 回归成功启动，进程保持 pending、focus group=40。J2ME 的 60 秒观察回归**未通过**：安装提交、J9 启动后 `probeJavaRuntime()` 超时，诊断捕获也超时。保留完整日志于本机 `/tmp/j2me-run/final2/`，不能将此轮标记为完整游戏启动成功。

同一最终构建另跑 `--launch-via-app-uid --observe 5`，短回归通过，结果在 `/tmp/j2me-run/final-native-uid/`。它仅验证真实 AppArc UID 入口的安装/AMS/J9 交接，不替代上面的 60 秒失败结果。

本机可用 Python 是 `/opt/homebrew/Caskroom/miniforge/base/bin/python`，完整 emsdk 位于 `/Users/zixing/emsdk`。

## 后续修复：JAR 入口、冷启动与音频协商

### 正式前端

- 恢复首页右下角「＋」菜单的 JAR 安装按钮，复用现有 `pickJar()` 与中英文翻译。
- Web 默认开启原始 J9 路径，`?j9raw=0` 才显式关闭。安装必须等 guest 注册状态为 2 才报告成功并保存；状态 0、失败及 180 秒超时均不能当成功。
- 刷新后注册文件仍在，但 AMS 进程已消失；`launch_registered` 先启动真实 AMS，再走 StubMIDP2。启动期间重新查找 AppArc 注册，避免持有可能因 guest 运行失效的指针。
- 正式播放器重试保留原入口：真实 AppArc UID 继续调用 `launchApp`，virtual UID 才调用 `launchMidlet`。
- ROM J9 经 WindowServer 绘制，旧 host MIDP 的 `j9PresentCount` 可以一直为零。启动遮罩现在也接受 guest 屏幕 redraw，避免已经出画仍盖着“启动中”。

### MMF 自定义接口

先前 `CustomCommandSync` 返回成功但未写输出，导致 `CustomInterfaceUtility` 在获取 builder 时 Leave(-1)，进而进入跨线程析构等待。现实现 MMF builder 协议：

- `0x10200017 / ECibGetBuilder(0)` 返回 20 字节 destination：builder UID `0x10207A8E`、handle 1。
- `ECibBuild(1)` 验证输入 UID descriptor；当前没有实现可选硬件音效，返回标准 null destination（UID 0、handle -2），让上层正常处理 `KErrNotSupported`。
- `ECibRemove(2)` 验证 destination 后完成；没有分配实际音效对象。
- 同步输出验证 descriptor 类型、长度与容量；未知命令返回 `KErrNotSupported`。异步自定义命令显式完成为不支持，避免成功空事件反复 rearm。

实现位于 `dispatch/src/audio.cpp`，并同步更新 guest shim 源码。由于本机没有 Symbian SDK，在加载现有 `mediaclientaudio_general.dll` 时，把四个 CustomCommand 导出改指向 host dispatcher thunk。映射依赖本项目 patch DLL 的导出 ordinal，不依赖 J9 或设备 ROM 的固定地址。

协议对照：[Symbian CustomInterfaceBuilder](https://github.com/SymbianSource/oss.FCL.sf.os.mmaudio)。可选混响等 DSP 音效仍不支持，不能据此宣称声音完整。

### Thumb 补丁跳板的栈参数破坏

补齐音频协议后发现，20 字节输出包的容量被读成 11893949。根因是原 Thumb 跳板 `push {r6,lr}; ...; blx r6` 把 SP 改了 8 字节，replacement 读取第 5 个及后续参数时拿到了错误位置。

改为不借用寄存器或栈的尾跳转：Thumb 通过对齐的 `BX PC` 切换至 ARM，再由 `LDR PC` 跳到 replacement，直接保留调用者 LR。Thumb 导出起点位于字的上半字时必须额外对齐 BX。ARM / 两种 Thumb 起点分别占用 8 / 12 / 14 字节；相邻导出的空间检查也包含目标地址 literal，空间不足继续使用现有 SVC trampoline map。

`kernel/trampoline.h` 提供统一生成函数，ROM 导出补丁的两处使用方均已替换。新增 `buildscript/test_export_trampoline.py`，从实际 C++ helper 生成机器码，用 Unicorn 执行 12 种 entry / target / return 模式组合，验证 r0–r12、SP、LR、栈参数、返回指令模式和写入边界；全部通过。

修复后实测输出容量恢复为 20 字节，J9 正常处理不支持的可选音效，Bounce Tales 越过加载动画进入主菜单；确认键可进入 `Chapter 1: Misty Morning` 章节选择。未把章节选择当作关卡游戏过程已验证。

音频修复的 Release 构建为 `3135bc66fbc2`，已关闭临时 profiling 并移除 MMF 诊断 WARN。构建、Python/前端 JavaScript 语法检查和 `git diff --check` 通过。macOS 可给启动回归脚本加 `--angle-backend metal`，用于与 SwiftShader 软件渲染作对照。

### 安装完成判定与持久化

`3135bc66fbc2` 的同页安装/启动回归（真实 AppArc UID、Metal、60 秒）已显示主菜单，原生 Calculator 回归也通过。但正式前端的刷新测试暴露出更早的安装完成判定问题：AppArc 注册文件已经出现时，SWInst `Installer[101f875a]` 仍在安装事务中。前端立即暂停 guest 并保存，恢复的是中途状态；冷启动 J9 虽能打开 JAR，却在 LCDUI 阶段正常退出。失败日志中没有目标安装的 SWInst opcode 516 成功结果，同页成功日志中则有。

安装状态查询现在同时等待 SWInst Installer 退出，再确认本次目标的新 AppArc 注册；不等待长期驻留的 `MIDP2SilentMIDletInstall` 监视器退出。新增 `buildscript/test_j2me_frontend.py` 覆盖正式入口、真实安装、保存后刷新以及冷启动，避免仅靠同页 smoke 回归漏掉这种问题。

最终构建 **`7ad26376be08`** 的正式前端回归通过（Metal、冷启动观察 60 秒）：

- 日志先出现目标安装 `opcode=516 result=0` 和 Installer 正常退出，之后才保存到 IndexedDB。
- 刷新后应用和真实 AMS 数据保留；从应用卡片启动 J9，进程在观察结束时仍为 pending。
- `game.png` 显示 Bounce Tales 主菜单；虚拟 OK 按键后的 `key-OK.png` 显示 Chapter 1: Misty Morning。启动遮罩正常消失。
- 本机证据目录：`/tmp/j2me-run/formal-ui4b/`。完整关卡游玩及可听声音尚未验收；系统 `camcorderjavastart.wav` 缺少解码器时仍使用现有 silent player。

前一音频构建的 SwiftShader 60 秒测试能读取存活 J9，但随后诊断/截图超时（`/tmp/j2me-run/final-swiftshader/`）。它不是完整画面回归通过，软件渲染下的浏览器失响应仍需独立排查；不能把所有浏览器卡顿都归为这次音频问题。

正式前端回归命令：

```sh
python3 buildscript/test_j2me_frontend.py \
  --rom /absolute/path/SYM.ROM \
  --rpkg /absolute/path/SYM.RPKG \
  --jar /absolute/path/BounceTales.jar \
  --output /tmp/eka2l1-j2me-frontend --observe 60 --angle-backend metal
python3 buildscript/test_export_trampoline.py
```

`test_export_trampoline.py` 另需 C++17 编译器和 Python `unicorn`。前端回归脚本 PASS 验证真实安装、持久化和存活 VM，菜单/按键画面仍需查看截图。

## localhost:8080 安装卡住的后续修复

先提交前述启动和音频工作：`3c93f82ed`。随后在正式首页复现“安装 BounceTales.jar…”一直不结束，发现两个独立原因：

- `app_list::add_entry` 插入成功返回了布尔值 `true`，而调用者需要数据库 ID。已有其他应用时，Bounce Tales 的真实 ID 为 2，状态查询却一直查 ID 1。现在返回 `sqlite3_last_insert_rowid`，失败统一返回错误 ID。同时修正 UPDATE 中缺失的 `:iconPath` 参数，它会导致后续绑定和 WHERE ID 错位。
- 重装相同 JAR 时，ROM 可以跳过已有套件，既不改 AppArc 注册时间，也不退出空闲 Installer。现在核对已有套件的名称、厂商、版本和完整 JAR 字节；只有内容完全一致且旧 UID 仍有效时才接受未变化的注册。完成判定仍等待 MIDP silent installer 发出的 opcode 516 请求结束；普通新安装仍等待 Installer 退出。

首页在保存成功后才刷新应用列表和图标缓存，失败不再刷新缓存；`EKA2L1.save()` 将并发保存排队，每个调用者等待自己的同步结束，失败后允许下次重试。初始 IndexedDB 恢复失败会中止启动并清除陈旧列表缓存。

Release 构建 **`7ef71bf5b0db`** 已在 `http://localhost:8080/index.html` 验证：

- 在隔离浏览器中预置 ID 1 的其他应用，再安装真实 Bounce Tales，确认得到 ID 2 并完成保存。
- 刷新后重新安装同一 JAR，再保存、刷新，从应用卡片冷启动；60 秒后 J9 仍存活。
- 人工查看 `game.png` 为主菜单，`key-OK.png` 为 `Chapter 1: Misty Morning` 章节选择。
- 失败基线位于 `/tmp/j2me-reinstall/baseline/`，修复后的完整记录位于 `/tmp/j2me-reinstall/final/`。仍未把章节选择等同于完整关卡或可听声音验收。

回归命令：

```sh
python3 buildscript/test_j2me_frontend.py \
  --url http://localhost:8080/index.html \
  --rom /absolute/path/SYM.ROM --rpkg /absolute/path/SYM.RPKG \
  --jar /absolute/path/BounceTales.jar \
  --output /tmp/j2me-reinstall/final --observe 60 --angle-backend metal \
  --seed-existing-app --reinstall --install-timeout 60
node --test buildscript/test_web_save.cjs
```

Node 回归覆盖并发保存必须等待后续快照，以及失败调用拒绝但后续保存可恢复这两条行为。

另做保存失败注入：将 `EKA2L1.save()` 替换成 rejected Promise 后，首页显示失败且应用缓存保持不变；但“刷新后一定没有新 host 条目”的额外断言失败。追踪发现当前 Emscripten 的 `_fd_sync` 会直接调用挂载的 `syncfs`，SQLite 在最终前端保存前就能写入 IndexedDB（证据 `/tmp/j2me-reinstall/save-failure4/console.log` 的 `SYNCCALL` / `DBPUT`）。因此这次修改不提供整个安装事务的存储回滚，也不能把保存队列等同于所有底层同步的全局串行化；实际存储故障后的原子恢复仍需单独设计。该注入结果不影响上面正常安装、重复安装及冷启动的通过结论。


## E72 安装、5320 重复图标及浏览器死锁

使用用户的 `/Users/zixing/Downloads/e72/e72.rom`、`e72.RPKG` 和 Bounce Tales 继续回归，发现安装链有多处独立问题：

- E72 的 silent installer 是 ROFS 上的 E32，原未签名安装策略补丁在代码 attach 前查不到运行地址。现在在非 ROM codeseg attach 后按进程地址应用相同的指令匹配，并使修改范围的 CPU 代码缓存失效。ROM/XIP 路径保留。
- 内核 `SafeInc32` / `SafeDec32` 错把 host 指针加减，没有修改 guest 引用计数。E72 安装器的辅助线程退出后提前关闭共享堆，主线程随后访问已解除映射的 `0x007009E8`。修复为修改 guest 数值并返回旧计数；不再用绕过堆释放掩盖问题。
- AppArc 非原生应用事务此前保存在整个 server 上。E72 AMS 的 Lifecycle 线程在安装期间回滚自己的事务，清掉了另一个会话已准备的注册状态，目标注册返回 NotReady，SWInst 最后返回 -30471。事务现在属于各自 `applist_session`。
- E72 SWInst 在成功完成 opcode 516 后仍常驻，单纯等待 UID `0x101F875A` 退出会永远停在“安装中”。现在观察本次 silent installer 实例发给 AppServer 的目标 `m.jad/m.jar` 请求回包，再核对已提交的目标注册；负结果直接传给前端。固件自带的 GuitarRockTour/Widsets 安装失败不会误伤用户上传的 JAR。

5320 的两个图标分别来自 AppArc 注册和 host JAR 数据库。列表按 guest JAD 身份及 `uids` 文件解析出的真实 UID 去重，保留有 JAR 图标的 virtual UID 卡片。raw 模式下只有暂存 host 记录、没有完成 guest 注册的失败安装不显示为可启动游戏。

此外，Service Worker 激活通知不再强制刷新正在安装或运行游戏的页面；新版本在下一次导航生效。

浏览器失响应与 guest 音频等待是两个问题。可见 Edge 窗口中复现过启动阶段、以及进入章节选择后截图/脚本全部超时。代码核验发现：`WsDeliverReportVisiblityEvent` 的 ntimer 回调先取 screen mutex、再取 kernel lock，而主线程的 deferred animation redraw 先取 kernel lock、再取 screen mutex。两条路径存在 AB/BA 死锁。已统一为 kernel → screen，并用 RAII 释放；没有把 guest 请求强行完成或给锁等待伪造超时。

安装错误回归使用 Bounce JAR 的本地副本，将 manifest 的 CLDC 版本改为不支持的值，确认 guest 拒绝可到达 UI 且不会新增游戏卡片；测试副本不入库。`/tmp/j2me-e72/reject10/` 已通过。

### 继续定位到关卡加载

上述锁顺序修复并未解决所有卡死。用真实菜单按键继续推进，另外定位并修复了三处问题：

- 普通内核对象最后一个句柄关闭时，`kernel_system::destroy()` 的“删除成功”布尔值被当成 guest Close 返回值。结果所有对象销毁都返回 1，触发 guest `RLibrary::Close` 的 DLL 析构流程；E72 J9 栈最终越过 `0x06700000` 下界。现在分离对象删除结果与 Close 动作返回值；只有最外层真正产生 DLL 卸载工作才返回 1。待报告与已报告的 DLL 析构批次分开管理，嵌套关闭只排队，`LibraryDetached` 在全部完成时返回 EOF。
- 条件变量 signal 取消计时事件时没有移除超时记录，已出队的旧回调也不验证线程当前等待对象。线程进入下一次信号量等待后，旧回调会错误唤醒它，导致 J9 队列空读、USER 130 或菜单停住。现在每次等待分配独立 token，signal/timeout 只消费对应记录并平衡引用，回调在内核锁下按 UID 查找仍存活的条件变量。
- 5320 进入章节后，浏览器最后停在 `SVC 0x6B` 的 `ChunkAdjust(type=2)`；关闭 JIT 仍停在同一调用。multiple memory model 的 commit/decommit 没有实际对齐页边界，剩余不足一页时 `page_num` 为 0，循环永远不前进。现在先夹取合法范围，再向外按页对齐；零长度和越界起点立即返回。decommit 上限同时修正为 chunk 最大长度，而不是 `offset + max_size`。

本机 x64/Rosetta `ekatests '[mem-chunk],[handle],[property],[timer]'` 已通过 9 个用例、86 条断言，覆盖最后句柄关闭返回值、非整页提交/释放、跨页表边界、零长度、超大长度和越界起点。浏览器回归仍须以实际关卡画面与按键响应为准，不能仅用 J9 存活或章节选择截图判定完成。

构建 `37664e468fb7` 在可见 Edge 窗口验证：5320 安装/保存/刷新后只有一个 Bounce Tales 卡片，进入第一关并能推进剧情提示，原有 browser hang 不再复现（`/tmp/j2me-e72/5320-final18/`）。E72 同样进入关卡，但首帧后 FPS 归零、移动未通过，浏览器仍可响应诊断（`/tmp/j2me-e72/e72-play18/`）；这条剩余问题继续跟踪，尚不能称为完整游戏验收。

另外 E72 将安装后的 JAR 改名为 `midlet.jar`（运行时 RFs Open 日志已验证），而不是 5320 的 `m.jar`。相同 JAR 重装校验增加这一文件名分支，仍要求逐字节一致和有效 AppArc UID，未放松内容校验。构建 `fa504c9c765c` 已通过 E72 重复安装、保存、刷新并保持单卡片的回归（`/tmp/j2me-e72/e72-nojit19/`）。另有 `[mem-global]` 4 个用例、23 条断言通过。

5320 在构建 `fa504c9c765c` 的进一步回归中已经越过剧情提示，第一关计时推进到 0:16、0:20、0:25，方向键使角色和场景变化，末尾约 13 FPS（`/tmp/j2me-e72/5320-play19/`）。这次验证包含实际关卡操作，仍不代表完整通关或可听音频验收。相关安装、内存与内核修复已提交为 `4134d5ba7`。


### E72 的 A3F 音频协议

E72 虽然属于 S60 3rd Edition，但 `mmfdevsoundproxy.dll` 发送的是 A3F `PostOpen(0)`。原先仅按系统版本选择 legacy 协议，将它当成要求 slot 1 settings 的 Initialize0，直接返回 -6，MIDI controller 随后清理退出。

会话现在根据首个 PostOpen 的参数类型选择 A3F：slot 0 为只读 8-bit destination package、slot 1 未指定。不能要求 slot 1 的值为零，ROM 的 TIpcArgs helper 并不初始化未使用的参数槽。播放缓冲区布局与完成通知路径也跟随会话协议，不再仅看 OS 版本。

核对 E72 ROM 的发送指令后确认，S60 A3F 在 42 多一个请求，后续 SamplesPlayed=44、Priority=46、BTBF=56、Close=63、GetTimePlayed=65；会话将 43–65 转换到现有后续 A3F 枚举。补上 A3F 已有后端能力的输入格式枚举、FourCC 数组及音量渐变分派；音量渐变读取正确的 slot 1。消息队列指针初始化为空，Close 允许初始化前调用，不支持的同步请求返回 NotSupported，非法初始化状态完成后立即返回。

诊断构建的 E72 安装、重装、持久化和主菜单继续通过，原先 PostOpen -6、未实现 39/38/63 已消失，MIDI controller 不再在该初始化点退出。但第一关首帧仍不推进（`/tmp/j2me-e72/e72-ws22/`），不能据此宣称 E72 已能正常游玩。窗口采样显示后续主要是查询屏幕尺寸和焦点；没有证据证明游戏 JAR 横屏异常是根因。临时窗口探针及强制合成已移除，所有诊断 JAR 仅保存在 `/tmp`，不修改用户原始游戏或固件。


正式构建 **`46f3e07cd591`** 已更新到 `localhost:8080`。`/tmp/j2me-e72/5320-release23/` 再次通过原始 Bounce Tales 的安装、同 JAR 重装、保存后刷新、单卡片和冷启动；按键截图显示第一关计时达到 0:25、角色和场景响应方向键，约 12 FPS。临时诊断 JAR 的结果不计入此验收。

同一正式构建还通过真实 AppArc UID 的启动入口回归（`test_j2me_launch.py --launch-via-app-uid --angle-backend metal --observe 30`，证据 `/tmp/j2me-e72/5320-nativeuid23/`）。


## MusicBox 的 -30472 与运行日志开销

用户提供 `/Users/zixing/Documents/GitHub/myjump/java/jar/musicbox.jar`（SHA-256 `9e59601c233668947f7d1c34e1524f0a3e8c42800c44f0ba8bec87b21c006942`）。在全新 5320 数据中复现 `SWInst opcode 516 result=-30472`，证据 `/tmp/j2me-e72/musicbox24/`，因此并非仅由残留安装记录引起。

根因是 host 将 manifest 原样复制成 JAD。该 JAR 的 `MIDlet-Permissions` 在 `javax.microedit` 后按 manifest 规则折行，下一行以空格接续；直接作为 JAD 后，权限属性被截断。对照测试只展开暂存 JAD 的续行、不改原始 JAR，即安装成功（`/tmp/j2me-e72/musicbox-unfold25/`）。ZIP 压缩标志、JDK 版本、UTF-8 描述中的中点均保持原样，无须重打包或绕过 SWInst 错误。格式依据：[MIDP 2.0 Manifest / Application Descriptor](https://docs.oracle.com/javame/config/cldc/ref-impl/midp2.0/jsr118/javax/microedition/midlet/package-summary.html)。

新增统一 `jad_from_manifest`，按字节展开主段续行、保留 UTF-8 和属性值、排除文件独立段，并写入实际 JAR URL/大小。安装与缺失 JAD 的补建路径共用转换。原生 `[j2me][manifest]` 两个测试、五条断言通过，覆盖权限折行、UTF-8 字节跨行、CR/LF/CRLF、文件独立段和已有 URL/大小替换。

正式构建 **`cc9e801e27cb`**：原始 MusicBox 首次安装、相同 JAR 重装、保存、刷新和单图标通过（`/tmp/j2me-e72/musicbox-fixed24/`）。前端回归增加 `--install-only`，单独验收安装而不把它混同于启动成功。`localhost:8080` 的实际前端测试也完成安装和重装（`/tmp/j2me-e72/musicbox-launch24/`）。

MusicBox 启动仍未通过：J9 输出 `BaseScreen.layout → sizeChanged` 的 `NullPointerException`，随后观察到 VM `exit=0/0`。字节码显示 `BaseScreen` 构造器先调用 `setFullScreenMode`，后初始化 `font`，而 `sizeChanged` 立即调用读取 `font` 的 `layout`。这是早期回调的初始化风险；目前没有证明该回调异常就是随后退出的唯一原因。myjump 中未找到匹配源码，原 JAR 未修改。

性能方面，把逐条 J9 窗口 IPC、成功握手和动态函数查找日志放到 `log_ipc` / leave probe 开关后，保留安装结果和 Java 输出。相同 Bounce Tales 按键流程中日志从 22693 行降到 4569 行；首次安装、重装、持久化和第一关方向键回归继续通过，计时到 0:25（`/tmp/j2me-e72/5320-perf24/`）。本轮画面约 8 FPS，较早回归约 12 FPS，测试时机不同，不能声称已获得帧率提升或卡顿完全解决。

### 有界 WebAssembly 模块缓存

进一步对相同 5320 / Bounce Tales、15 秒启动观察与九次按键序列采样（Edge 默认 GPU，CDP CPU profiler）：

| 指标 | 未缓存（`5320-profile25`） | 缓存（`5320-cache25`） |
|---|---:|---:|
| 观察采样时间 | 46.51 秒 | 47.00 秒 |
| `WebAssembly.Module` 构造次数 | 255517 | 62371 |
| 构造计时总和 | 5.048 秒 | 2.112 秒 |
| 缓存命中 | — | 202851（约 76.5%） |
| 新增 table 槽数量 | 4555 | 4490 |

这表明此前 `compiled` 的持续增长主要不能直接当成活动 table 槽数量：大量失效后的重复构造可以复用模块编译结果。新增缓存以 FNV hash 查找，再完整比较生成字节，包括地址和链目标；仅复用不可变 `WebAssembly.Module`，每次仍创建新实例绑定当前 memory/table，原失效与槽回收逻辑不变。LRU 上限 8192 条、16 MiB 原始模块字节（不包括 V8 编译代码内存）；本轮结束 8192 条、约 5.7 MB 原始字节。哈希碰撞视为未命中，编译失败不缓存，超大模块可执行但不保留。

新增 `node --test buildscript/test_wasmjit_module_cache.cjs` 四个回归全部通过，使用真正的 WebAssembly 模块覆盖字节快照、实例隔离、槽复用、哈希碰撞、LRU/容量淘汰及失败处理。最终构建 **`7d7a62b3e8d7`**；缓存版本第一关计时到 0:13，方向键响应，末帧约 12 FPS。上述约 58% 降幅仅指这轮模块构造计时，不能当作整体帧率提升比例。完整日志、CPU profile 和截图在 `/tmp/j2me-e72/5320-profile25/`、`/tmp/j2me-e72/5320-cache25/`。

最终缓存构建另跑原始 MusicBox：首次安装、重复安装、保存和刷新均完成，启动后的同一空指针异常与 VM 退出仍存在（`/tmp/j2me-e72/musicbox-final25/`），不标记启动 PASS。不支持 CLDC 版本的负例继续被 guest 拒绝且不新增卡片（`/tmp/j2me-e72/reject25/`）；未将错误码改为成功。Node 缓存/保存六个用例、原生 manifest 两个用例、Python 语法及 `git diff --check` 通过。
