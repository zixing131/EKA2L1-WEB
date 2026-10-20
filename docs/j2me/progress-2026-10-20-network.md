# Web 联网、MusicBox MP3 与 J2ME 权限设置

本轮延续 `progress-2026-10-19-launch.md`。此前“Web 没有注册 TCP、在线数据和播放未通过”的边界在本轮改变；并不代表所有 ROM、音频编码或联网应用都已兼容。

## 网络后端

参考用户提供的 `/Users/zixing/Documents/GitHub/myjump/javasrc/js/evalNative-bridge.js`，并核对 JavaQQServer 的 `WebSocketTcpProxy.cs`，实现 `wss://qq.zixing.fun/ws/tcp` 的 connect/write/destroy 与 event 消息。二进制 payload 使用 Base64；所有 socket 共用 WebSocket，按 ID 隔离。没有连接时不会主动访问代理。

实现位于 `services/src/internet/protocols/web.cpp` 和 `web/src/web_network.js`，接入 Symbian ESock，而非仅截获 MusicBox 的 HTTP 调用。因此原生 Symbian 与 J2ME 使用同一 TCP 后端。域名解析分配本地合成 IPv4 地址，连接时还原域名交给代理解析；不把合成地址发到网络。5320 的 HTTP 栈创建 resolver 时使用 UDP 协议号 17，即使实际连接为 TCP 6，因此 resolver 同时注册两者。

- 支持异步连接、TCP 收发、部分/完整接收、缓冲尾部后 EOF、取消、关闭、连接超时及代理断线错误。
- JS 回调通过 microtask 送回内核，避免在持有内核锁的 EM_JS 调用内重入；C++ 完成前检查请求线程及 TRequestStatus 仍有效。
- 接收上限 32 MiB，发送与 WebSocket 排队上限 4 MiB，超限返回错误。代理没有逐次 write ACK，发送完成表示代理传输队列已接受。
- 当前服务端只有 TCP；UDP 数据报、监听/accept、半关闭均未实现。DNS 不是本机真实 DNS 查询；不存在的域名在连接阶段失败。
- HTTPS 仍依赖固件自身的 TLS 能力；WSS 仅保护浏览器到代理的一段，不能赋予旧固件现代 TLS 支持。
- 可在创建模块前配置 `window.EKA2L1_NETWORK_RELAY_URL`，或使用 `Module.networkRelayUrl` 指向兼容协议的代理。

## 权限设置

设置 → Java ME → **自动允许 J2ME 权限**，默认开启。读取 `eka2l1_j2me_auto_allow`，值 `0` 为关闭，其余为开启；选择会保存在当前站点的 localStorage，刷新后保留，下次权限请求生效。

开启时，J9 进程 UID `0x102033E6` 的 Yes/No 系统权限查询直接完成 ROM 的 Yes 命令 3005。关闭后使用已有 HTML dialog 由用户手动选择。仅 Web/J9 查询适用；不改变其他原生程序的确认框，也不把尚未实现的系统能力伪装为成功。

真实浏览器已验证默认 ON、切换 OFF 后刷新保留、切回 ON 后刷新保留。5320 MusicBox 默认开启时联网和文件请求自动允许；关闭后显示实际文件/联网询问，选择 No 的测试中 TCP 连接数及收发字节均为 0，J9 仍存活。

## MP3 和文件缓存

仅新增通用 player 的 MP3 解码不足以支持 MusicBox：5320 的 Java MMAPI 会加载 ROM 的 `MP3AudioPlayControllerPlugin.dll`，将压缩 MP3 写到 MMF DevSound。此前该路径明确拒绝 FourCC `0x33504D20`，随后 MusicBox 把播放失败当作跳过下一曲。

引入固定版本、CC0 的 minimp3（来源/提交/许可证见 `src/external/minimp3/UPSTREAM.md`），同时支持通用文件/buffer player 和 MMF 的分块 MP3 解码。保留跨写入的半帧和 bit reservoir，按需生成 PCM，避免整首展开为 PCM；MMF 仍支持 PCM8/PCM16。不支持的编码仍返回失败。MMF 不再忽略后端 write 失败。

Web AudioWorklet 暂停时不再消费 ring；停止/重启通过音频线程确认丢弃旧队列。备用 AudioBuffer 路径按真实播放时钟计算位置，暂停保存剩余片段、恢复重新调度，seek/销毁取消旧音频。Worklet 加载未完成时不提前走备用队列，避免切换时重叠或丢样本。

另修复旧 J9 DirOpen 兼容逻辑：原先 FileConnection 检测 `E:\netmusic\music\xxx.mp3\`、`.part\`、`.sz\` 时会自动创建同名目录，破坏文件下载。自动建目录限制到 J9 启动需要的 private 路径，公共文件探测不再修改磁盘。已经由旧版生成的错误目录没有自动删除；如影响已有缓存，可在应用内清理缓存后重试。

## 已完成验证

- 原生 MP3 测试覆盖真实生成音调的非零 PCM、元数据、暂停、seek、完成一次、重放、custom 输入生命周期、137-byte 分块与逐次补缓冲、垃圾前缀重同步和错误输出格式。
- Node 网络测试覆盖 relay wire format、二进制顺序、异步回调、取消后的迟到事件、断线重连、超限和超时；音频测试覆盖备用输出暂停/播放位置/销毁以及 Worklet 暂停和停止后队列清理。
- 5320 / Edge 可见浏览器，正式源码 JAR `musicbox_202610192200.jar`：安装、保存、刷新、单卡片、冷启动、在线首页图片、推荐歌单、歌曲列表、下载及真实音频输出通过。
- 构建 `d0e7af92ee68` 的 `musicbox-network32`：31 个 TCP 连接，收到 2,114,861 字节，网络错误 0；播放页显示歌曲推进到 0:18 / 1:58，AudioContext 为 running，AudioWorklet 输出峰值 0.4984（实际 PCM，不只是 UI 计时）。原始 myjump JAR 没有改写。
- `musicbox-permission-off32`：关闭自动允许后手动拒绝，0 个 TCP 连接、0 收发字节，页面和 VM 保持可操作。

测试证据保存在本机 `/tmp/j2me-e72/`。`test_j2me_frontend.py` 新增 `--manual-permissions`、`--require-network`、`--require-audio`，保存网络、音频与缓存诊断。实际歌曲依赖线上 API/CDN 的可用性及歌曲授权；本轮没有绕过付费/版权限制。原生 Symbian 应用接入同一后端，但尚未逐个应用验收；E72 的其他启动/游戏问题仍参考前一篇记录。

最终发布构建 **`46ea3af23ca0`** 已更新 `localhost:8080`，并在该地址用隔离 Edge 上下文重跑（`musicbox-final33`）。网络 31 次连接、接收 2,115,090 字节、0 错误；AudioWorklet 峰值 0.7535。缓存 `E:\netmusic\music\2675114848.mp3` 为正常文件（1,891,629 字节），`.sz` 也是正常文件，无同名伪目录。截图显示播放推进、数字 0 暂停在 0:30，数字 6 在暂停状态跳转至 0:40；未以截图推断逐样本音质。原生相关测试最终为 12 个用例、231 条断言，Node 网络/音频 6 个用例、真实浏览器权限 dialog 测试及设置持久化检查通过。
