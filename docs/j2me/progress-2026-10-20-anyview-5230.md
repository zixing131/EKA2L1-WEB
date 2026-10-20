# Anyview 4.0 与 Nokia 5230 的 J2ME 启动

接 [Web 联网、MP3 与权限设置](progress-2026-10-20-network.md)。输入为用户提供的原始 `Anyview4.0.jar`（manifest 版本 4.0.12）以及 `5230 [ROM + RPKG dump]/5230.rom`、`5230.RPKG`；未改写 JAR 或固件。

## Anyview 的短信初始化等待

5320 基线安装、保存、刷新都成功，J9 存活，但只显示原生状态栏，没有 Anyview 内容。`!jes-*-java-comms` 等待栈包含 `j9_23_wma.dll`。诊断确认 WMA 先发 SMS provider IOCTL `0x308`（ReadParams），随后读取数据。

SMS socket 没有覆盖 `receive`；基类只记录 “Receiving data from socket unimplemented!” 而不完成请求。J9 因此一直等待。基类未实现的 `receive`、`send`、`ioctl`、`shutdown` 现在以 `KErrNotSupported` 完成异步请求，callback 形式的 bind 同样回传不支持。没有把短信参数或发送结果伪造成可用功能。

构建 `0fa58d73e20a` 的 5320 对照已进入 Anyview 更新说明；关闭说明后主页显示“初次运行、自动设置、打开、在线、更换主题”。后续再次按右软键是应用的“退出”，确实会正常结束 VM，不能将该按键序列当作启动崩溃。

新增原生测试使用真实 process/thread、guest TRequestStatus 和请求信号量，覆盖基类四种异步操作及 SMS 继承的 receive；验证状态不再 pending、错误值正确、只发出一次完成信号。

## 5230 的安装前初始化

5230 属于 S60v5 / `epoc94`。初始日志报未实现 SVC `0xB1`，silent installer 随后退出 `-46`。v94 分发表补上已有的 `creator_security_info` 实现，让安装器读取实际父进程安全信息；不修改权限错误为成功。

补齐系统调用后又暴露冷启动竞争：host 在 AMS 进程刚创建时就启动 silent installer，二者同时打开尚不存在的 `DBS_102045FE_MIDP2SystemAMSStaticV2`。一个客户端创建成功，另一个创建得到 `-11`，`SystemAMSMain` 随后以 `E32USER-CBase 47` 退出。详细文件日志与“先让 ROM AMS 自行启动”的对照确认了这条顺序问题；不是缺少空 journal.db，也不需要复制其他 ROM 的数据库。

Web 增加安装前异步就绪检查，等待真实 `!MIDP.SystemAMS.MIDP2` 服务。每次轮询让出浏览器主线程，让 guest 定时器和调度器继续运行；失败或超时明确拒绝，尚未就绪时不启动另一个安装器。

ROM 已有长期驻留的 silent installer 时复用它。JAR/JAD 完整写入后向 RFs 发送文件变更通知，使监视器扫描新包；直接通过 host VFS 写入原先没有这项通知。若没有监视器，只在 AMS 已就绪后创建安装器。原有 SWInst 完成回包、目标注册、保存及去重检查继续保留。

## 5230 的 J9 内核调用与 DLL 共享

安装通过后，v94 分发表仍缺少 J9 使用的 thread critical flags、undertaker 和 condition variable 调用。补齐 `0x2E`、`0x88–0x8A`、`0xD7–0xDA`，复用已有内核实现；不套用 v93 或 v10 的不同编号。缺少 condition variable wait 原先造成大量未实现调用和忙循环。

随后定位到真正的主线程内存异常：`Main` 在 `0x73A0015A` 读取 `0x01700000`。运行时加载地址确认该代码属于其他进程的 `javalogger.dll`，调用者是被错误共享的 `javaregistryclient.dll`。注册库自身没有静态数据，但其依赖有进程私有数据，因此代码中的导入地址不能跨进程复用。

根因是 `codeseg::eligible_for_codeseg_reuse()` 与 attach 共用 `mark`。已经标记的依赖被直接跳过，导致有私有数据的依赖漏检。现在使用独立 visited 集合检查完整依赖闭包，保留纯代码 DLL 共享，并让含私有依赖的代码逐进程重定位。原生测试覆盖重复 eligibility 查询和两个进程的实际导入地址。

浏览器测试也补充检查 J9 Main 是否停止。内存异常可能只停线程、保留进程的 pending 状态；过去仅检查进程存活会把黑屏误报为 PASS。截图和菜单操作仍作为实际启动验收。

## 验证记录

- 原生 codeseg / socket / notifier / manifest / timer / handle：12 个用例、206 条断言通过。
- Node 异步准备与保存：5 个用例通过，覆盖等待让出主线程、失败、超时、并发保存和失败后重试。
- 正式构建 `b252c03c8a65` 已更新 `localhost:8080`；浏览器回归使用同一产物的稳定测试服务 `127.0.0.1:18092`，Edge 可见窗口、独立存储上下文。
- `5230-anyview46`：导入原始固件、首次安装、重装、保存刷新、单卡片、冷启动通过；关闭更新说明后显示主页，左软键打开菜单、方向键移动选择。无启动 AV 或未实现 SVC。
- `5320-anyview46`：安装、刷新、主页、菜单和方向键回归通过。
- `5230-bounce46`：Bounce Tales 安装和冷启动通过，确认进入第一关并显示游戏角色、场景及教程提示。
- `musicbox46`：5320 在线歌单、下载及播放回归通过；31 次 TCP 连接、接收 2,110,215 字节、网络错误 0。AudioContext running，输出峰值 0.7530，播放截图为 0:42 / 1:58。前一轮 15 秒按键间隔未等到音频初始化完成，本轮延长到 40 秒后通过实际 PCM 检查。
- 证据目录均位于本机 `/tmp/j2me-e72/`。5230 的两个应用仍只绘制屏幕上部，Anyview 部分文字有裁切，下部留白；启动已恢复，但本轮不宣称 S60v5 显示适配或全部 JAR 已完善。该显示现象与已修复的主线程 AV 分开记录，未通过修改 JAR 或强制屏幕分辨率掩盖。
