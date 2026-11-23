# `prot_read_write_exec` 在 iOS sandbox 下 `mprotect` 静默丢 W

> 来源：阶段 3 修复清单 #1（[`IOS_PORTING_TASKS.md`](IOS_PORTING_TASKS.md)）。状态：✅ 已解决（阶段 2 #14 SIGBUS 根因）。
>
> **一句话结论**：iOS arm64 sandbox 强制 W^X，`mprotect(R|W|X)` 返回成功但实际页是 RX（W 被静默剥离），随后写入即 `KERN_PROTECTION_FAILURE`；dyncom 是解释器、host 永不真正执行 guest 页，PROT_EXEC 纯冗余，故在 `translate_protection` 的 iOS 分支统一剥 PROT_EXEC。

崩溃 IPS 显示在 `std::fill(base, base+0x4000, 0)` 写第一个字节就 `KERN_PROTECTION_FAILURE`（`Data Abort byte write Translation fault`），栈顶是 `kernel::chunk::chunk` → `dispatcher::dispatcher`（创建 "DispatcherTrampolines" chunk，`prot_read_write_exec`，4 KB）。

原因：iOS arm64 sandbox 强制 W^X，`mprotect(PROT_READ|PROT_WRITE|PROT_EXEC)` 返回 0（成功）但实际页是 RX（PROT_WRITE 被静默剥离），随后写入触发 KERN_PROTECTION_FAILURE。dyncom 路径下 host 永远不真正执行 guest 页（dyncom 是解释器），PROT_EXEC 对 host_base 是纯冗余。

修法：`src/emu/common/src/types.cpp::translate_protection` 在 iOS 分支末尾统一剥 PROT_EXEC（若结果为 0 回落 PROT_READ），并加 `// TODO(ios)` 说明 stage 4 引入 `map_executable` / MAP_JIT 时替换。此改动只影响 iOS，POSIX / Win32 / Android / macOS 桌面不变。

验证：xcodebuildmcp 自动化 Mount N95 ROM 后 UI 显示 `Mounted: N95 8GB (S60v3 - FP1)` + `Apps (18)`，进程不再 SIGBUS。截屏 `docs/screenshots/ios-stage3/3.1-mount-unblocked/applist-18-apps.jpg`。
