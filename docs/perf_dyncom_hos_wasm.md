# dyncom 性能优化（HOS + WASM）— 2026-06-16

参考 iOS fork（[docs/ios/](./ios/)）的优化结论,把对 **dyncom 解释器**（HOS 原生版、
WASM 版共用的 CPU 后端,两者都不能 JIT）有效、且跨平台安全的优化落到本仓库。每项独立
提交,可单独回退。

## 背景：为什么瓶颈在 dyncom

HOS 真机能启动后实测很慢。HOS/WASM/iOS 都禁 JIT(W^X/沙箱),跑的是 dyncom 纯解释器。
iOS fork 的采样显示:解释器线程几乎 100% 在 `InterpreterMainLoop`,其中**最大的可消除
开销不是解释本身,而是「指令翻译缓存被反复清空导致的重复翻译」**——这正是本轮的主目标。

## 已落地

### 1. ASID-tag 指令翻译缓存（最大单项收益）— commit 见 git log

**问题** dyncom 的翻译缓存原来在**每次地址空间(进程)切换**时被整个清空
(`flush_tlb → invalidate_translation_cache`)。带图形的 app 全是 IPC 重户(窗口服务器 +
每帧 FBS 字形光栅),进程切换极频繁 → 每次 IPC 往返后缓存被清 → guest 反复重新翻译整个
渲染循环。iOS fork 上这块重翻译占 guest CPU 线程约 **17%**。

**修复** 缓存改用 `(asid << 32) | vpc` 作键,不同地址空间的翻译共存、跨进程切换存活:
- `instruction_cache` 与 wasm `jit_block_map`:键 `uint32 → uint64`。
- `block_lookup` 直接映射 lookaside 项带上 asid 并校验(避免同 PC 不同进程的块在同一槽位
  混淆)。
- `arm_interface` 新增 `set_asid`（默认 no-op;dynarmic/12l1r 忽略),dyncom override 设
  `instruction_cache_asid`。
- scheduler 在每次进程切换推入新 asid（原来注释掉的 hook);`flush_tlb` 现在**只**清数据
  TLB(按 vaddr 键、必须清),不再清指令缓存。
- **asid 回收的正确性兜底**:`process::destroy` 清整个指令缓存——一个 asid 被回收给新进程
  前,绝不携带死进程的旧翻译。SMC（`imb_range`）、跨线程代码页 unmap
  (`icache_invalidate_pending`)、翻译缓冲溢出仍按原样整体 flush。

**风险与验证** 这是四平台共享的 CPU 核心改动。源 fork 要求用 differential harness 把关。
本仓库已移植该 harness（见下）。**上真机/跑 harness 验证前请把它当未验证。** 嵌入式 fallback
解释器永远不推 asid（asid 0,单一未 tag keyspace),不受影响。所有改动 TU 已做语法检查通过。

### 2. ReadCode 走 TLB exec 槽（小而稳）

`ReadCode`(取指)是唯一还总是 page-walk 的热访问器(`ReadMemory*` 早已走 dyncom 512 项
TLB 快路径)。新增 `tlb::lookup_exec()`(只命中标记为可执行的页),`ReadCode` 先试它、miss
回退原 `read_code` 走查 → 零正确性风险(更严格的 exec-only 匹配)。对标 iOS Stage-1a。

### 已确认本仓库已有、无需再做

- **内存快路径内联**:`ReadMemory32` 等已是「内联 TLB 命中 → `...Slow` 回退」结构,即 iOS
  fork 已落地的那项优化。
- **AddWithCarry**:本仓库实现已用 64 位算术干净计算 carry/overflow（非 iOS 文档描述的慢
  手写位运算）。iOS 实测内联它的净收益在噪声内,故**跳过**,不在共享 CPU 代码上做无谓改动。

## 暂缓（需要我能编译/运行的环境才负责任地做）

### present 流水线 / 双缓冲
iOS 上把 30→36-40 FPS 的关键是「guest 不阻塞在 present 上」。但:
- 它改的是**驱动层的命令提交/throttle 同步**(`flush_to_driver` → 驱动命令队列 fence),
  四平台共享、且 HOS 原生版的图形线程已是独立 `graphics_driver->run()` 循环。
- iOS fork 自己的经历:这个双缓冲**先原型、因一次 Snakes 卡死回退、后应要求重加并靠长时间
  真机测试才确认**。
- 本环境**编不了也跑不了 HOS 原生版**(无 OpenHarmony SDK),present 死锁比「慢但能跑」更糟
  (参见 CLAUDE.md 里 WASM/HOS 反复踩的 `Atomics.wait`/阻塞死锁史)。

**结论:暂缓。** 等能在可编译可真机验证的环境里做、并按 iOS 那样长时间测稳定性再上。盲改
高风险低把握,不值。

## 怎么验证 ASID 改动（关键）

差分测试 harness 已移植(默认 OFF,对正常构建零影响):

```bash
# 需要 host 上有 cmake + 一个能编 C++20 的编译器
scripts/cpu_difftest.sh                 # 默认 seed/count
scripts/cpu_difftest.sh <seed> <count>  # 自定义
```

它用 `EKA2L1_CPU_DYNCOM_ONLY=ON` + `EKA2L1_BUILD_DYNCOM_DIFFTEST=ON` 编一个独立 host 工具,
把随机 ARM 数据处理 + load/store 指令流跑过 dyncom,和独立 golden ALU 模型 + 第二个 dyncom
实例(自对照 A/B)逐状态比对,首个分歧即非零退出。**ASID 改动落地后务必先跑它,再上真机
跑多 app / 进程切换 / SMC / 退出重进的回归。**

## 怎么编 web(WASM)HOS 版测速

本环境没有 emsdk(且其安装源不在网络白名单),所以 **web 版我无法在这里产出**。用我新加的
脚本在你机器上编:

```bat
REM 先激活 emsdk(或设 EMSDK 环境变量指向 emsdk 根),然后:
build_hos.bat
REM 产物:build_wasm_hos\bin  (eka2l1_wasm,EKA2L1_HOS_BUILD=ON)
```

(`build_hos.bat` 是 `build_hos.sh` 的 Windows 版,编的是 WASM-in-WebView 的 HarmonyOS 版,
正是你要的「web 端 hos」。)

WASM 版会**自动受益于 ASID 缓存 + ReadCode-TLB**(都在 dyncom 共享路径上),不需额外开关。

## 预期收益

ASID 缓存是 IPC 重户(任何带图形的 app)上最大的单项 CPU 削减(iOS 上消掉了约 17% 的重
翻译线程占用)。ReadCode-TLB 再小削几 %。**纯解释器的派发+执行是硬下限**,JIT 在这些平台
不可用,所以 present 流水线(渲染侧)和指令融合(派发侧)是后续仅剩的大杠杆——都需要可验证
环境才好做。
