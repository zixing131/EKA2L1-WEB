# J2ME 卸载、播放稳定性与 5230 触屏显示

接 [Anyview / 5230 启动](progress-2026-10-20-anyview-5230.md)。本轮按用户确认分开验证：MusicBox 用 5320，原生触屏虚拟按键用 5230。测试不修改用户的 ROM 或原始 JAR。

## 卸载及重新安装

此前只删除 host JAR 数据库和图标，guest JavaRegistry / AppArc 注册仍在，列表去重失去 host 身份后就显示成没有图标的原生卡片。

现在用小型 ARM guest bootstrap 调用 ROM `RSWInstLauncher::SilentUninstall`，传入安装目录 `uids` 的套件 UID；该 UID 与后面的 AppArc UID、AMS 内部 suite ID 不同。真实事务成功后才删除对应非原生 AppArc 资源及 host 文件、数据库记录。前端异步轮询，保存完成后刷新主页和程序管理。reset 用 generation 排除旧回调；保存期间暂停 guest 执行，避免 IDBFS 同步与 guest 文件修改交错。

5230 的卸载暴露 Central Repository opcode 4 未实现。补上 Delete：检查 key、删除值、通知订阅者，缺失 key 返回 NotFound。

原厂预安装目录监视器会记住已经移除的套件，不能作为用户显式重装的入口。新安装暂存到独立目录，调用 `RSWInstSilentLauncher::SilentInstall`，等待实际请求完成及目标 AppArc 注册。最初使用普通 `RSWInstLauncher`，5320 成功而 5230 的 AMS 返回 `-939`，SWInst 返回 `-30471`；改用无界面服务后 5230 安装、卸载、刷新、重装均通过。两种入口传入的选项已与 ROM 监视器实际 IPC 对照；没有把失败码改成成功，也没有删除整个 AMS 数据库。`-939` 的私有错误名称未确认。

ARM 原文在 `src/emu/j2me/guest/uninstall.S`，对应 import/data 偏移与机器码由 `interface.cpp` 使用。

## MusicBox 播放与内存破坏

“音乐还在播放”不能证明 guest 调度正常。独立 Web Audio 输出可以继续消费已经排队的数据。本轮连续操作时定位到多处独立错误：

- window 析构没有从 sibling 链摘除，ASan 捕获父窗口遍历已释放 child；析构先解除链接。
- RFs 填充文件写入位置前的空洞时，把单字节零地址当成长缓冲区读取；改用有界零块循环写入，并检查位置、长度和短写。
- dyncom 切换进程未更新 ASID，可能复用其他地址空间的指令；恢复调度时的 ASID 设置。
- translation buffer 回卷仍保留编译块时不能清除 code-page bitmap，否则自修改代码漏掉 JIT 失效；在真正清空 JIT blocks 时清除。
- TLB 快路径不能将空槽的页号 0 当作 guest 零页，也不能把跨 guest 页访问当作连续 host 内存。解释器检查访问宽度/权限；跨页 MMU 访问逐字节翻译。生成的 WASM 对零页和跨页 load/store 在写回基址前退出快路径。

这些修复有实际 ASan 报告、guest 回归或独立 CPU 用例支撑，但不能据此保证所有游戏的内存问题都已解决。

MusicBox 使用先前已修复初始化顺序的源码构建 `JavaQQServer/qqmusic-midlet/dist/musicbox_202610192200.jar`，SHA-256 `0fca6bbe90d7803879541db7a6f6131f800e5f03626ead2343af8640a02f0c8a`；不是仍含 `BaseScreen.layout` 空指针的旧 myjump JAR。

## Sims 3 的 SDKSound

该 JAR 的 MMAPI 初始化需要 AMR-NB。Web/minimp3 后端新增分块 AMR-NB 解码，支持可选文件头、跨写入帧、8 kHz 单声道及重复到双声道、停止后解码器重置；MMF 同步公布这一实际支持的格式。此前 MP3 / PCM 路径保留。

解码器为 OpenCORE AMR 0.1.6 的 decoder/common 子集，固定来源、校验和及 Apache-2.0 许可证保存在 `src/external/opencore-amr/`。测试音调的生成来源记录在 `src/tests/common/assets/audio/README.md`。存储帧头的 padding 位按 RFC 4867 §5.3 忽略，不误当作 RTP F 位拒绝。

## 闪屏、局部更新及触屏按键

5230 的 J9 DSA 游戏区和下方原生虚拟键盘是不同窗口。此前把整个 DSA framebuffer 贴回屏幕，会覆盖键盘；键盘重绘又可能擦除游戏区。现在保留 DSA texture，按活动 DSA 窗口可见区域合成，屏幕模式变化使缓存失效。

legacy flicker-free 窗口也保留像素，局部重画不再丢失未变化的列表行；重放历史命令时按剩余区域裁剪，修正可见区域的绝对坐标比较与 region 运算。空裁剪区不再被解释为全屏。

新增缓存曾使 MusicBox 封面变成旧帧或黑块：`EColor16MU` 的高字节不表示 alpha，而 WebGL 不能执行桌面的纹理 alpha swizzle。opaque legacy 窗口保持不透明绘制和合成，ScreenPlay / alpha 窗口仍使用预乘透明度；修复后首页图片恢复。

Web canvas 保留绘图缓冲，只有 guest 重画计数或呈现设置改变才重新呈现；不靠持续全屏重刷维持画面。

设置 → Java ME → **Java ME 屏幕内键盘**：应用默认、隐藏、游戏按键、导航按键。写入暂存 JAD 的 `Nokia-MIDlet-On-Screen-Keypad`，不改 JAR；在下次安装或重新安装时生效。该选项控制 5230 等触屏固件的原生 Java 按键，与网页外部的 T9/E72 按键面板分开。

## 验证记录

本机证据在 `/tmp/j2me-e72/`：

- `uninstall63`：5320 Sims 3 安装、保存、刷新、卸载、再次刷新确认消失、重装及主菜单通过。`sim3-amr49` 已到角色创建画面。
- `5230-install67`：使用原始 Anyview JAR，安装、保存、卸载、刷新后消失、重装通过。
- `musicbox63` / `musicbox64`：Edge / 5320 连续播放、暂停恢复、数字 6 跳转、返回列表及重新选择后 J9 Main 仍存活，有非零实际 PCM；64 输出峰值 0.7485。这两轮仍有封面回归，不作为最终显示验收。
- `musicbox65` 首页图片恢复，但按键在构造阶段提前发送，没有进入有效播放。ASan64 同样没有有效音频输出，不能算作长时间播放通过。测试增加等待实际网络响应后再操作。
- `musicbox67`：图片及播放页封面恢复；暂停图标、歌词交互等截图显示 Java UI 继续响应。41 个 TCP 连接、2,254,443 字节接收、0 网络错误，实际 PCM 峰值 0.7636。
- `musicbox-asan65`：没有完成播放验收；检测版触及 WASM 2 GB 上限，先出现 ASan allocator out-of-memory，随后出现持锁异常退出后的 mutex 断言。不能将这轮报告为无错误长测，也不能将后续 mutex 栈当作最初故障。最终 release 回归独立进行。
- 早期 `5230-touch57` 已验证松开触屏按键后的连续帧不再白屏；最终发布构建继续回归。

正式 release **`b7c525cbd123`**，profiling symbols 关闭；`localhost:8080/index.html` 的资源版本已核对。原生相关测试 38 个用例、15,708 条断言通过；Node 保存、音频、网络、WASM module cache 共 13 个用例通过。实际生成 WASM 的差分测试已通过 4,810,624 组 ALU 输入及新增零页/跨页 load/store bailout 用例。

`sim3-release68` 在 localhost:8080 上通过安装、卸载、保存刷新后消失、重新安装和冷启动；截图推进到角色特性选择，没有 SDKSound 错误。

`5230-touch68b` 使用相同 release 产物的稳定测试服务 18092：Anyview 菜单、原生中央触摸键、打开文件列表均响应。松手后八张连续截图的键盘区域（237 × 145 像素，排除应用内容及构建水印）逐像素一致，没有白色覆盖。首次 localhost:8080 的该轮测试在保存后的页面重新载入时发生资源连接重置，未计为通过。

MusicBox 的 release 自动化将按键保持时间从 300 ms 改为 80 ms，避免触发 guest 自动连发而跳过推荐页；`musicbox-release68` 因该输入问题进入新曲页，未取得音频输出，不计作播放通过。

**`musicbox-release68b` 最终通过**：同一正式 release、Edge、5320，在线歌单、图片、播放、暂停恢复、数字 6 跳转、返回列表、下键选择及再次打开播放器后 J9 Main 仍存活。截图显示暂停于 0:01，恢复后推进至 0:44；返回列表时完整保留未变化的行、封面和悬浮播放器。AudioContext running，实际 PCM 峰值 **0.7557**；31 次 TCP 连接、收到 **2,110,008 字节**、网络错误 0。

自有修改的 `git diff --check` 与 Python 语法检查通过；新引入的第三方 AMR 原文件保留上游空白格式，未为消除 whitespace 提示重写 vendor 源码。
