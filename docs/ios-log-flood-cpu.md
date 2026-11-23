# iOS 前端从不应用日志过滤器 → 同步刷盘洪水把 CPU 打满

> 来源：阶段 3 修复清单 #5（[`IOS_PORTING_TASKS.md`](IOS_PORTING_TASKS.md)）。状态：✅ 已解决（N97/S60v5 点 Calculator 整屏黑 + CPU 100% 的**主因**）。
>
> **一句话结论**：iOS 前端从不调 `parse_filter_string`、且非 `BUILD_FOR_USER` 构建默认 `*:trace`，叠加 spdlog `flush_on(debug)`，每次上下文切换 + 每条 dyncom VFP 子操作都被同步刷盘（N97 启动 6s 打 ~43 万行）；修法是在 `IosEmulator` 镜像 `BUILD_FOR_USER` 降级逻辑应用 normal-use preset + 追加 `CPU*:warn`。**注意：此修复只消除日志洪水导致的 100% CPU，N97 Calculator 自身仍黑屏**（剩余阻塞见 [S60v5 AVKON FEP/Pti](./s60v5-avkon-fep-pti.md)）。

定位链路：
1. `IosEmulator::startWithDocumentsPath` 只调了 `log::setup_log(nullptr)`，从没像 Qt（`qt/src/state.cpp:78`）那样调 `log::filterings->parse_filter_string(conf.log_filter)`；而 `log_filterings` 默认构造就是 `reset_all(spdlog::level::trace)`——所有 log class 全在 trace 通过。
2. iOS app 不是 `CI` → `BUILD_FOR_USER` 构建，`config::config.cpp` 里把 `*:trace` 降级为 normal-use preset 的那段是 `#if BUILD_FOR_USER`，于是 `conf.log_filter` 本身也停在 `LOG_FILTER_DEBUG_PRESET = "*:trace"`。
3. 结果每次 guest 上下文切换（`scheduler.cpp:128` 残留的 stage-3.2.1 诊断 `LOG_INFO(KERNEL, "Switch to thread …")`）和每条 dyncom VFP 子操作（`vfp/*.cpp`，class `[CPU.DynCom]`）都被记录，且 spdlog `flush_on(spdlog::level::debug)` 让每条 ≥debug 的消息都**同步 flush 到文件**。

轻量 S60v3（N95）勉强挺过去，但 S60v5（N97：`ecomserver`/`AknIconSrv`/`cdlserver` 启动风暴 + 大量浮点 AVKON 排版）被刷盘洪水拖死——点 Calculator 后约 **6s 打出 ~43 万行日志**，CPU 全耗在格式化 + 同步 I/O，guest 几乎不前进、出不了帧，表现为"黑屏 + CPU 100%"。

修法（仅动 iOS 前端 `IosEmulator.mm`）：`conf.deserialize()` 后镜像 `BUILD_FOR_USER` 的降级——`log_filter` 为空或等于 debug preset 时，按 `extensive_logging` 选 `LOG_FILTER_DEBUG_PRESET` 或 `LOG_FILTER_NORMAL_USE_PRESET`，并**额外追加 `CPU:warn CPU.DynCom:warn CPU.12L1R:warn`**（iOS 跑 dyncom，normal preset 仍把 CPU class 留在 trace，浮点重的 guest 会刷爆 VFP trace），再 `log::filterings->parse_filter_string(...)` 真正应用；`applyConfigSnapshot` 改 `logFilter` 时也即时重应用（无需重启）。

验证：N97 点 Calculator 后日志从 ~43 万行/6s 降到 ~220 行，`Switch to thread`/VFP trace 归零。

**注意：此修复只消除"日志洪水导致的 100% CPU"，N97 Calculator 自身仍黑屏——剩余阻塞见 [S60v5 AVKON FEP/Pti](./s60v5-avkon-fep-pti.md)。** 顺带记录：`scheduler.cpp:128` 的 `Switch to thread` 是 stage-3.2.1 遗留的临时诊断 `LOG_INFO`，本应清掉（被 `Kernel:Warn` 过滤后无格式化开销，故本次未动共享内核代码，但值得后续移除）。
