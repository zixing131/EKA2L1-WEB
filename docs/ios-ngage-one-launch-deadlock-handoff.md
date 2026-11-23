# 交接文档 — iOS N-Gage "Start Game" 黑屏（ONE 启动死锁）

> 目的：让后续接手者能冷启动直接继续调查 Bug #2。背景性结论见
> [`ios-ngage-installer-popup-and-game-launch.md`](./ios-ngage-installer-popup-and-game-launch.md)，
> 本文只聚焦"怎么复现、已知什么、下一步查哪、别踩哪些坑"。

## 一句话现状

点 **Start Game** 后 N-Gage 破解游戏 ONE 黑屏：guest 渲染约 2 帧后**永久冻结**，
输入无法唤醒。已定位到 **guest 侧挂起/恢复死锁**（不是渲染、不是 CPU 后端问题），
但未修。

---

## 1. 复现

### 环境
- 已知机型：iPhone 16 Pro 模拟器，iOS 26.5（`xcrun simctl list devices booted`）。
- 复现样本：`roms/ONE_fixed_by_BodyZ.n-gage`（仓库自带）。
- 相关 UID：
  - `0x20003B78` = N-Gage 启动器（应用名 **Games**，进程名 `ngiplay0x20003b78`）
  - `0x2000AFBF` = 游戏 ONE 本体（进程 `ONE.exe`）——**不能直接启动**，必须从 Games 内 Start Game
  - `0x20007B39` = PlayServer（N-Gage 运行时/启动编排）
  - `0x20007B38` = 独立 "N-Gage Installer" app（另一条安装入口）
  - `0x2000730F` = Snakes（**干净** N-Gage 游戏，作对照——iOS 上能正常进游戏）

### 步骤
```sh
SIM=$(xcrun simctl list devices booted | sed -n 's/.*(\([A-F0-9-]*\)) (Booted).*/\1/p' | head -1)
APPDATA=$(xcrun simctl get_app_container booted com.eka2l1.emulator data)

# 1) 放样本（首次；已安装过则见"重置"一节）
mkdir -p "$APPDATA/Documents/data/drives/e/n-gage"
cp roms/ONE_fixed_by_BodyZ.n-gage "$APPDATA/Documents/data/drives/e/n-gage/"

# 2) 选后端（优先 dyncom；两个后端都要能复现）
#    config 在 $APPDATA/Documents/data/config.yml
#    ios-use-jit: false → dyncom(默认)   ；true → dynarmic(JIT)
#    注意：cpu: 行是 dynarmic 但真正决定 iOS 后端的是 ios-use-jit

# 3) 启动 Games，等安装完（约 90~120s），点 Start Game
xcodebuildmcp simulator launch-app --json '{"simulatorId":"booted","bundleId":"com.eka2l1.emulator","launchArgs":["-LaunchAppUID","0x20003B78"]}' --output json
# 用 xcodebuildmcp ui-automation snapshot-ui 拿到 OK 键 elementRef（通常 e18），tap 它启动游戏
```
安装完成后主页高亮 **Start Game**，按 **OK**（方向键中心，`e18`）即触发游戏启动。

### 判定"冻结"
- 截图黑屏、`0 FPS`。
- `EKA2L1.log` **完全停止增长**（`wc -c` 前后对比，90s 内 0 字节增长）。
- 发任意按键（`ui-automation tap`）也不恢复。

### 重置到"未安装 ONE"以便重复复现
安装是一次性的，重复实验前要清 guest 侧安装产物再重放样本：
```sh
C="$APPDATA/Documents/data/drives/c"
rm -rf "$C/private/20007b39" "$C/private/2000afbf" "$C/private/20003b85/gamepurchase.db" \
  "$C/sys/bin/one.exe" "$C/sys/bin/onedl.dll" "$C/sys/bin/onefileaccess.dll" \
  "$C/sys/bin/ngifilemanagerfactory_2000afbf.dll" "$C/sys/bin/as_2000afbf.exe" "$C/sys/bin/binpda_one_cr.dll" \
  "$C/sys/hash/one.exe" "$C/sys/hash/onedl.dll" "$C/sys/hash/onefileaccess.dll" \
  "$C/sys/hash/ngifilemanagerfactory_2000afbf.dll" "$C/sys/hash/as_2000afbf.exe" "$C/sys/hash/binpda_one_cr.dll"
cp roms/ONE_fixed_by_BodyZ.n-gage "$APPDATA/Documents/data/drives/e/n-gage/"
```

---

## 2. 已确认的事实（别再重复验证）

1. **与 CPU 后端无关**：dyncom 与 dynarmic 冻结点、症状完全一致。→ 不是解释器/JIT 指令 bug，
   也不是"解释器太慢"（等 90s+ 无任何进展）。
2. **不是渲染 bug**：guest 自身停了（log 冻结 + 采样显示无 guest 前进）。iOS present 路径正常
   （对照 Snakes 用同样 DSA `update_screen` 路径能持续出帧）。
3. **不是等输入**：冻结后发按键无反应。
4. **通用 N-Gage 路径没问题**：Snakes（0x2000730F）在 iOS 正常 Start Game → 进 3D 关卡。
   问题是 ONE 特有——它是 DRM 破解版，启动跑激活服务器 `AS_2000AFBF` + 反复加载
   `OmaDrmAgent.dll`/`wmdrmagent.dll`/`f32agent.dll`。
5. **本仓库已提交的弹窗修复与本 bug 无关**（commit `2fdf4a99d`：`end_redraw` 强制 server 重合成 +
   `mutex::wait` assert 移除）。已确认 mutex 改动不是死锁诱因（死锁线程是 suspend，不是等 mutex）。

### 决定性证据：freeze 时的调度器 idle dump
```
thr='ngiplay0x20003b78' state=2 reqcnt=8 susp=true sleeplvl=1 sleepsts=false waitobj='-' proc='ngiplay...'
thr='PlayServerSplashScreenFallBackThread' state=0 reqcnt=0 susp=true ...  proc='playserver[20007b39]'
（其余 server：AknIconServer/ecomserver/CdlServer/PlayServer/gtmdserver/NAF*×4/TzServer/wmdrmpkserver
  全部 state=5 wait_fast_sema、reqcnt=-1，阻塞在各自 requestSema —— 服务器空闲的正常态）
```
- `state=2` = `thread_state::wait`；`state=5` = `wait_fast_sema`；`state=0` = `create`。
- **核心异常**：启动器 UI 线程 `ngiplay` 被 **suspend（susp=true）**，且挂起发生在它正处于
  `User::After` sleep 时（`sleeplvl=1`），同时它有 **8 个已完成但未消费的请求**（`reqcnt=8`），
  却**永不被 resume**。一个 suspend 的线程不会被它的 sleep 唤醒定时器恢复——必须显式 `RThread::Resume()`。
- `PlayServerSplashScreenFallBackThread` 也 suspend 且 state=create（**从未启动**）。
- ONE.exe 线程在多次 dump 中**不在存活线程列表里**（要么已退出，要么该次 run 尚未 summon）。

**结论假设**：Start Game 时 PlayServer 的启动编排把 `ngiplay` UI 挂起（交给游戏/splash），
本应在游戏就绪或退出时 resume 它，但那个 resume 从未发生 → 启动器冻死。

---

## 3. 可复用的诊断探针

> KERNEL 的 `LOG_TRACE` 在当前日志级别下**被过滤**（日志里 `[Kernel]` 只有 `E`/`W`）。
> 探针必须用 **`LOG_ERROR`** 才能落盘。

### A. 调度器 idle 全线程 dump（本次用来抓到上面的证据）
放在 `src/emu/kernel/src/scheduler.cpp` 的 `thread_scheduler::switch_context`，
`else`（`next_thread == null`，即将 idle）分支里、`if (kern->should_core_idle_when_inactive())` **之前**：
```cpp
{
    static int dbg = 0;
    if (dbg < 8) {              // 限次，避免刷屏
        dbg++;
        LOG_ERROR(KERNEL, "IDLE dump: {} threads", kern->get_thread_list().size());
        for (auto &obj : kern->get_thread_list()) {
            if (!obj) continue;
            kernel::thread *th = reinterpret_cast<kernel::thread *>(obj.get());
            kernel_obj *wo = th->wait_obj;
            LOG_ERROR(KERNEL, "  thr='{}' state={} reqcnt={} susp={} sleeplvl={} sleepsts={} waitobj='{}' proc='{}'",
                th->name(), static_cast<int>(th->current_state()), th->request_count(),
                th->is_suspended(), th->sleep_level, static_cast<bool>(th->sleep_nof_sts),
                wo ? wo->name() : std::string("-"),
                th->owning_process() ? th->owning_process()->name() : std::string("-"));
        }
    }
}
```
需要 `#include <kernel/thread.h>`（已在）；`request_count()`/`is_suspended()`/`sleep_level`/
`sleep_nof_sts`/`wait_obj` 都是 `kernel::thread` 可访问成员。

### B. 主机侧线程栈采样（判 busy-loop vs idle、看 guest 在跑什么）
```sh
PID=$(ps aux | grep -i "EKA2L1.app/EKA2L1" | grep -v grep | awk '{print $2}' | head -1)
sample $PID 3 -file /tmp/eka2l1_sample.txt
# 关注 "system::loop()" 那条线程：reschedule→event::wait 表示 idle；
#   InterpreterMainLoop/dynarmic_core::run + ReadMemory8/WriteMemory8 表示 guest 在跑
```
本次采样特征：多数时间 idle（`idle_event.wait`），少量在 guest 做**字节级 ReadMemory8/WriteMemory8**
（疑似 DRM 解密/校验循环），偶发 SVC `session_send_sync` / `wait_for_any_request`。

### C. DSA 出帧探针（确认游戏是否还在提交画面）
`src/emu/dispatch/src/screen.cpp` 的 `update_screen` / `flexible_post` / `wait_vsync` 入口打
`LOG_ERROR(HLE_DISPATCHER, ...)`。本次结果：ONE 只调 `update_screen` **2 次**然后停，`wait_vsync`/
`flexible_post` 0 次。

> ⚠️ 这些探针都是临时的，调完**务必清干净再提交**（本轮已全部还原）。

---

## 4. 相关代码位置（按调查顺序）

| 关注点 | 文件:符号 |
|--------|-----------|
| 挂起 / 恢复 | `src/emu/kernel/src/thread.cpp` `thread::suspend`(747) / `thread::resume`(804) |
| sleep 唤醒回调 | `thread.cpp` `thread::notify_sleep`(514)；调度点 `scheduler.cpp` `sleep`(304) |
| 唤醒事件注册 | `scheduler.cpp` `"SchedulerWakeUpThread"`(47) |
| idle 停/起 | `scheduler.cpp` `switch_context` idle 分支 / `queue_thread_ready` 的 `idle_event.set()` |
| WaitForAnyRequest | `thread.cpp` `wait_for_any_request`(663)（含 stray-signal 吸收逻辑，见 stray 文档） |
| 信号量 signal/wait | `src/emu/kernel/src/sema.cpp` `signal`(41)/`wait`(63)/`suspend_waiting_thread` |
| 请求信号量 | `thread.cpp` `signal_request`(726)/`request_count`(730) |
| DSA 出帧 | `src/emu/dispatch/src/screen.cpp` `update_screen`/`flexible_post`/`wait_vsync` |
| N-Gage 安装/卡识别 | `src/emu/system/src/epoc.cpp` `install_ngage_game_card` 等（这条路已 OK，仅参考） |

核心待答问题：**谁 suspend 了 `ngiplay`、谁本该 resume 它、那个 resume 为什么没来？**
从 PlayServer(0x20007B39) 的游戏加载握手切入：它对 `ngiplay` UI 的挂起/恢复、splash fallback 线程
（state=create 从未启动，可能相关）、以及 `ngiplay` 那 8 个未消费请求分别对应哪些异步完成。

---

## 5. 下一步建议（按性价比）

1. **给 suspend/resume 加溯源日志**：在 `thread::suspend`/`resume` 打 `LOG_ERROR`（谁 suspend 了
   `ngiplay`、调用方 PC/线程；resume 是否被调用过）。这是最直接能证实/证伪"resume 丢失"的一步。
2. **看 PlayServer 的挂起序列**：确认是 guest 的 PlayServer 主动 `RThread::Suspend(ngiplay)` 然后
   等某条件再 Resume；若模拟器某个被它依赖的 IPC/属性/事件没兑现，就是那条 IPC 的 HLE 缺陷（**这类才是
   可接受的通用修**）。重点怀疑 `NGI_SEMAPHORE_xxx` 命名信号量协调（日志有 `Can't open object: NGI_SEMAPHORE_*`）。
3. **对照 Snakes**：同样探针跑一遍 Snakes 的 Start Game，diff 出 ONE 多做/卡住的那一步（激活服务器
   `AS_2000AFBF` 是 ONE 独有）。
4. **激活服务器**：查 `AS_2000AFBF`/`activationengineserver.exe` 是否真正起来并回应，还是它的某个
   DRM IPC 被 stub 成永不完成，导致游戏不发出让 PlayServer resume 启动器的信号。

---

## 6. 已排除的死路（别重复）

- **换 CPU 后端**：两后端一致，无用。
- **等更久**：90s+ 零进展，不是慢。
- **发输入唤醒**：无效，真死锁。
- **dynarmic 关 ConstProp**：能绕过 JIT 的**另一个**崩溃（`ConstantPropagation`→`FoldShifts`→
  `Value::IsImmediate` 沿长 `Identity` 链爆宿主栈，`src/emu/cpu/src/arm_dynarmic.cpp` `make_jit`(308)），
  让游戏在 dynarmic 上能启动**但仍撞同一个挂起死锁**——所以那个 JIT 崩溃是独立问题、不是本 bug 的根因，
  改法（`config.optimizations = all_safe_optimizations & ~ConstProp`）已还原未保留（会全局掉 JIT 性能）。
- **iOS present/`present_mutex`**：采样确认无线程卡在 present fence（graphics 线程空闲等命令），无关。
- **弹窗那两处改动**：与死锁无因果（死锁线程是 suspend，不是等 mutex/重绘）。

---

## 7. 政策提醒

本仓库规则（`CLAUDE.md`）：不加 app/游戏专属 hack；除非确属跨平台缺陷，避免大改共享 emulator 代码。
因此"直接在启动器死锁时强行 resume" 这类针对 ONE 的绕过不可取。可接受的是**定位到某条被 guest
依赖、但模拟器 HLE 没兑现的 IPC/属性/事件并把它补上**（通用修）。若最终确认是 DRM 破解版自身对真实
激活环境的依赖（非模拟器缺陷），则应作为"不支持此破解 title"结案，保持干净 N-Gage 路径不受影响。
