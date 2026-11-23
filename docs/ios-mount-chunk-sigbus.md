# 解锁 mount 链路：iOS 内核 chunk 写 0 SIGBUS

> 来源：阶段 3.1（[`IOS_PORTING_TASKS.md`](IOS_PORTING_TASKS.md)）。状态：✅ 已解决。
>
> **一句话结论**：mount 时内核 chunk 清零写触发 SIGBUS，根因是 Apple Silicon 用 16 KB host page、而 EKA2L1 按 4 KB 粒度调 `mprotect` 被 kernel 静默 `EINVAL` 拒绝；修法是把 `common::virtualmem` 的 protect 范围对齐到 host page size（详见 [iOS guest PC=0 死循环](./ios-guest-pc0-deadloop.md)）。

- 定位现场：阶段 2 #14 的 SIGBUS 出现在 mount 走到 dispatcher 初始化分配 kernel chunk → `std::fill_n` 清零时，地址 `0x11f904000` 区间。先把崩溃栈、所在 chunk 的 `create_info`（region flag / size / max_size / permission）、`commit()` 的实际入参与 `mprotect` 的 errno 全部抓出来登记到本阶段修复清单里，再决定怎么改。
- 候选 root cause：①`common::map_memory` 在 iOS 上 reserve 一大块 `PROT_NONE`，随后 `commit()` 走 `mprotect(PROT_READ|PROT_WRITE)` —— iOS sandbox 对单进程最大 mmap 数 / RLIMIT_AS / `vm_allocate` 行为与 Linux / macOS 桌面不一致，可能 mprotect 返回 0 但实际页未真正变成 W；②kernel chunk 的某个 region 标志在 iOS 上被错误识别成 code（W^X 互斥下默认 R+X，写入就 KERN_PROTECTION_FAILURE）；③`max_size_` 与 page size 错位，commit 漏掉了 `fill_n` 写的尾部页。需要逐一排查，不要凭直觉一把改。
- 修法分层（按代价从低到高，验证一种再上下一种）：
  1. 在 `multiple_mem_model_chunk::create` / `commit` 路径上加 iOS 专属断言 + 详细日志，先把哪条 region / 哪段 offset 失败定死。
  2. 如果是 W^X 误判：在 `common::is_memory_wx_exclusive()` 的语义上明确 "非可执行内存不受 W^X 限制"，让 kernel data / ROM image / dispatcher static 走纯 RW 路径，不被当作 JIT。
  3. 如果是 mmap reserve 行为差异：iOS 下改用 "小步 reserve + commit 同步分配" 或直接 `mmap(MAP_ANON|MAP_PRIVATE, PROT_READ|PROT_WRITE)` 跳过 `PROT_NONE` 阶段；必要时给 `common::map_memory` 增加 iOS 分支或新增 `map_memory_committed(size, prot)` API。
  4. 如果是 dispatcher 自己的内部分配器假设了 "reserve 完整 chunk 后线性 fill" —— 在 iOS 下让分配器按已 commit 区域走，或一次性 commit 全 chunk。
- **不在 3.1 范围内**：dynarmic / MAP_JIT / `pthread_jit_write_protect_np`。这些是阶段 4。3.1 只解决 "非可执行内存的写访问"。
- 验收：xcodebuildmcp 自动化或手工，从启动 → 主屏 → 选 N95 → Mount → `set_device(0)` → reset → ROM 映射 → ROM chunk → kernel-data chunk → dispatcher 初始化 全程不再 SIGBUS；`symsys->loop()` 真正开始驱动 winserv 心跳。
