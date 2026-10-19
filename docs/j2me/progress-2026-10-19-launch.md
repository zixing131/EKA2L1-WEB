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

可加 `--launch-via-app-uid` 覆盖真实 AppArc UID 的入口（页面中重复的 Bounce Tales 条目），默认入口是 host 列表的 virtual UID；二者最终都必须进入真实 guest AMS 启动链。

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
