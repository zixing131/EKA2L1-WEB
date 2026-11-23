# X7 Asphalt 6 启动与比赛链路修复

## 结论

Asphalt 6（UID `0x2003B2CC`）在 X7 / rm-707 上的黑屏与卡死不是单一游戏缺陷，而是 Symbian^3/Belle 路径连续命中了 SCDV、Belle executive、ROM DLL 加载、MMF/ALF、Window Server、Accessory 和 Loader 等多处通用仿真缺口。

修复后可稳定完成：Gameloft 动画 → Asphalt 6 宣传片 → 标题画面 → 主菜单触控 → Free Race → Nassau → Normal Race → Mini 选车 → 正式比赛。车辆、赛道和 HUD 正常渲染，调查时实测约 17–27 FPS、车速 199 km/h；没有加入 Asphalt UID、文件名或资源路径特判。

## 复现条件

- iOS 模拟器：iPhone 16 Pro，iOS 26.5
- EKA2L1 设备：X7，firmware code `rm-707`
- 游戏 UID：`0x2003B2CC`
- 游戏安装在当前设备的 E 盘
- CPU：dyncom（默认正常运行路径）
- 启动命令：

  ```sh
  xcodebuildmcp simulator launch-app --json '{
    "simulatorId": "booted",
    "bundleId": "com.eka2l1.emulator",
    "launchArgs": ["-LaunchROMCode", "rm-707", "-LaunchAppUID", "0x2003B2CC", "-LaunchKeypadLayout", "fullscreen"]
  }' --output json
  ```

正常情况下启动约 2–3 秒后即出现 Gameloft 动画；两段片头合计约 64 秒，不跳过时约 75 秒后进入标题画面。若这段时间始终纯黑、随后直接跳到标题，属于视频补丁未生效，不是正常加载。

## 根因链

| 阶段 | 现象 | 根因 | 修复 |
|------|------|------|------|
| SCDV 创建设备 | `BITGDI 10` | replacement SCDV 不支持 Belle 请求的 `EColor16MAP` 预乘 alpha 模式 | 复用 32-bit alpha draw device，并保留 `EColor16MAP` display mode |
| Screen surface | 旋转/区域状态错误，继而触发 BITGDI panic | `GetInterface(KSurfaceInterfaceID=8)` 返回不支持，缺少 `MSurfaceId` 和设备旋转接口 | SCDV 提供 ABI 兼容的 surface ID、四方向能力、设置/读取方向 |
| Belle executive | SecLdd 等对象句柄无效 | v10 executive 表缺少/错位的 `ThreadProcess`、server/session 和 logical-device/channel SVC | 补齐 `0x7D..0x84` 映射与通用 HLE logical channel 创建 |
| ROM DLL 加载 | `hxmedpltfm.dll` 无法加载 | ROM 中该文件是非 XIP E32Image；同时 `\sys\bin\*.dll` 是 rooted 但不带盘符 | ROM 文件先识别 E32Image；无盘符 rooted 路径依次在挂载盘符解析 |
| MMF A3F | 启动线程同步 IPC 永久等待 | new-architecture MMF 缺 Stop、priority、client-thread-info 等 opcode | 映射到既有 stop/priority 处理；in-process 音频模型完成 client-thread-info |
| ALF | transition policy 请求语义错误 | 长生命周期通知被当成立即完成的普通 no-op | 保存 pending policy request，并实现 cancel；同步 effect 调用返回序列化成功码 |
| Window Server | screen device 请求挂起 | 缺 `GetCurrentScreenModeAttributes` 和 extension query | 返回 ABI 正确的 60-byte `TSizeMode`，无扩展时完成 `0` |
| Accessory Server | Belle accessory client 挂起 | 只实现了旧 S60v3 opcode | 增加 modern connection subsession、状态查询/通知/取消 |
| Loader | IVE policy server 等待 PDD load | `ELoadPhysicalDevice` 未处理 | 与现有 LDD HLE 策略一致：校验名称并完成成功 |
| 早期开场 | 两段电影播放期间纯黑，随后直接进标题 | iOS bundle 只收集 `*_general.dll`，遗漏 X7 所需 `mediaclientvideo_v100.dll` | bundle/staging 接受全部版本化 patch DLL，由 loader 按 EPOC 版本选择 |
| 真机音频回收 | 黑屏且 host 偶现卡死，最终被 iOS watchdog 杀死 | guest 持 kernel lock 停止 AudioUnit，render callback 同时阻塞等待同一 kernel lock | 实时音频通知改用 kernel `try_lock`，锁忙时保留请求并在后续 callback 重试 |
| 真机重力感应 | CoreMotion worker 在 `desc_base::get_max_length()` 空指针崩溃 | 传感器异步完成先取 pending IPC、后拿 kernel lock，和 guest stop/close 释放 descriptor 竞态 | callback 先拿 kernel lock并校验 session/requester/descriptor；stop/close 同锁取消，详见 [`ios-sensor-async-ipc-crash.md`](./ios-sensor-async-ipc-crash.md) |

最后一个阻塞是 MMF new-architecture Stop；补齐后游戏开始持续加载 `.bdae`、shader、车辆和赛道资源，并正常提交 GLES 帧。

日志中针对不存在的 loose `.tga`、`.glsl.config` 和 fallback shader 路径仍会出现 `Trying to open a non-existent file`。游戏会继续从归档资源加载，这些不是启动失败或 guest crash。

## 真机 watchdog 卡死

TestFlight build `260737`（commit `16ee63770`）的三份 `.ips` 都是 `EXC_CRASH / SIGKILL`、`0x8BADF00D scene-update watchdog`，不是 guest panic。`EKA2L1-2026-07-13-114528.ips` 与 `...122638.ips` 在触摸事件时卡死，`...114848.ips` 在关闭运行中应用时卡死。

符号表从 GitHub Actions TestFlight run `29219577085` 下载：

```sh
gh run download 29219577085 \
  -n EKA2L1-testflight-dSYM-16ee63770c4c3d8e74e7f288a2850f54ee8106eb \
  -D /tmp/eka2l1-as6-dsym

dwarfdump --uuid /tmp/eka2l1-as6-dsym/EKA2L1.app.dSYM
```

dSYM UUID `FBB29AC8-FB7B-395E-AF89-0C17C9CF294D` 与三份 crash 的 EKA2L1 image 完全匹配。符号化后的共同锁序是：

```text
guest/SVC thread
  kernel lock held
  -> ~mmf_dev_server_session / eaudio_dsp_stream_destroy
  -> audiounit_ios_output_stream::stop
  -> AudioOutputUnitStop (同步等待 render callback 退出)

AURemoteIO render thread
  -> DSP more-buffer callback
  -> kernel lock (等待 guest/SVC thread)

iOS main thread
  -> touch / closeRunningApp
  -> kernel lock (同样等待，最终触发 scene watchdog)
```

修复为通用的实时线程通知语义：`dsp_stream_notification_callback` 返回通知是否已经投递；MMF 与 EAudio 的 CoreAudio callback 只尝试获取 kernel lock，失败即返回，底层保留 completed request / `more_requested` 状态并在下一次 callback 重试。这样 `AudioOutputUnitStop` 不再等待一个被 guest 自己持有的锁，同时通知不会丢失。没有绕过 watchdog、强制超时或 Asphalt 专用条件。

同目录另有一份 TestFlight `260729` 的 `.crash`，是独立的 CoreMotion/Sensor Framework 异步 IPC 竞态：slot 1 数据 descriptor 读取后，guest stop/close 释放了 slot 2 counts descriptor，host 回调随后空指针写回。CI dSYM 匹配、完整时序与通用修复见 [`ios-sensor-async-ipc-crash.md`](./ios-sensor-async-ipc-crash.md)。

## 早期 Gameloft / Asphalt 电影

游戏资源本身完整：

- `movie/logo_gameloft_480x320.mp4`：MPEG-4 + AAC，7.33 秒
- `movie/a6_480x320.mp4`：MPEG-4 + AAC，56.04 秒

黑屏版本的日志在启动即报 `Can't find suitable patch DLL for map mediaclientvideo.dll`。仓库已有通过 E32 校验的 `src/patch/mediaclientvideo/group/mediaclientvideo_v100.dll`（UID2 `0x10003B19`、UID3 `0xEE000009`、167 exports），但 iOS CMake bundle glob 和沙盒 staging 过滤器都只接受 `_general.dll`，所以 `.map` 存在而真正的 Symbian^3 实现缺席。

修复后 iOS bundle 会携带所有 `group/*.dll`，沙盒 staging 同样接受所有 `.dll`；`lib_manager` 继续按 ROM EPOC 版本选择 `_v100`、`_v81a` 或 `_general`，没有改变 Android/Qt 加载策略。模拟器逐帧验证显示启动约 2–3 秒后开始 Gameloft 蓝色描边动画，完整显示 `GAMELOFT`，随后正常播放跑车宣传片。

## SCDV DLL 制作

### 当前交付：Belle SDK 完整源码构建

2026-07-27 在 Windows XP / Nokia Symbian Belle SDK v1.0 / GCCE 4.4.1
中重新验证后，完整 C++ 源码可以直接生成兼容的 replacement DLL，不再需要把
交付产物限制为原 DLL 的字节补丁。关键是保留 `scdv_general.def` 的 ordinal，
而不是保留各函数的内部地址。

Belle SDK 构建需要两项源码兼容：

- Belle 的受控头文件在 `epoc32\include\platform`，`priv.mmp` 和
  `scdv_general.mmp` 必须显式加入该目录。
- 公开 Belle SDK 不带 partner-only `cdsb.h`；仓库在
  `src/patch/scdv/inc/cdsb.h` 保存所需的最小 `CDirectScreenBitmap` ABI 声明，
  实现仍完全位于 EKA2L1 源码中。

XP 命令行中设置 SDK 根目录，然后依次构建静态库与 DLL：

```bat
set EPOCROOT=\Nokia\devices\Nokia_Symbian_Belle_SDK_v1.0\

cd C:\path\to\EKA2L1\src\patch\priv\group
sbs -b bld.inf -c arm.v5.urel.gcce4_4_1 ^
  --mo=POSTLINKER_SUPPORTS_ASMTYPE=1 ^
  --mo=ASM=C:/PROGRA~1/CODESO~1/SOURCE~1/bin/arm-none-symbianelf-gcc.exe

cd C:\path\to\EKA2L1\src\patch\scdv\group\general
sbs -b bld.inf -c arm.v5.urel.gcce4_4_1 ^
  --mo=POSTLINKER_SUPPORTS_ASMTYPE=1 ^
  --mo=ASM=C:/PROGRA~1/CODESO~1/SOURCE~1/bin/arm-none-symbianelf-gcc.exe
```

两个 make override 是 Belle SDK 这套 Raptor/GCCE 组合的工具问题：
`POSTLINKER_SUPPORTS_ASMTYPE=1` 让 `elf2e32` 生成 GNU 汇编；使用 `gcc.exe`
生成纯汇编 export stub，避免 `g++.exe` 无意义地查找安装包中不存在的
`libstdc++.a`。

Belle SDK 的 `drtaeabi.dso` 导出 227 个 ordinal，排在 `libgcc.a` 之前，因此
`__aeabi_idiv` / `__aeabi_uidiv`（ordinal 222/223）会被链接成 ROM 导入。所有
目标固件的 `drtaeabi.dll` 都只有 221 个导出，这两个导入永远解析不了。
`src/patch/scdv/src/aeabi.cpp` 在 DLL 内部自带这两个 helper，工程自身的目标
文件先于任何库参与解析，导入不再产生。详见
[Snakes dies on the N95](./scdv-aeabi-idiv-missing-rom-export.md)。

当前完整构建产物：

- UID：`10000079 10003B19 EE000002`
- ARMV5 / EKA2 / DEFLATE
- code link address：`0x8000`
- code size：`0x6ECC`
- exports：31，ordinal 表不变

在 iPhone 16 Pro 模拟器（`26D5FEDA-3BDC-4699-83ED-58B749D676DF`）上运行
Asphalt 6 完整自动回归为 `PASS=9 FAIL=0`：进入 Nassau 比赛且无 guest
crash。完整构建与旧 binary-patch 产物观察到的模拟器图形表现一致。

### 历史 ABI-preserving 二进制补丁路径

最初调查时尚未有可用的 Nokia SDK 环境，因此 `16ee637` 以仓库原始
`scdv_general.dll` 为基线，只修改确定的 code bytes 并重算 E32 header CRC。
这条路径仍保留为历史和诊断工具，但不再生成当前交付 DLL。

历史基线信息：

- Git blob：`ec2ffa212c3cc7e0a426adacb656e1b962db3e6a`
- SHA-256：`6ead71eb0538bf519a4a088a2efa4ccec711ed53acc5dad42d105f1b4418b4ee`
- code size：`0x6104`
- exports：31

### 生成历史二进制补丁

工具链放在 `~/Developer/symbian` 时执行：

```sh
scripts/build_scdv_belle_patch.sh /tmp/scdv_general_belle.dll
```

脚本会：

1. 从基线 Git blob 取原 DLL，并校验 SHA-256；浅克隆缺少该 blob 时可设置 `SCDV_BASE_DLL=/path/to/original.dll`。
2. 用 `arm-none-eabi-gcc/ld/objcopy` 将 [`surface_stub.S`](../src/patch/scdv/surface_stub.S) 组装为 ARMv5 Thumb position-independent code。
3. 使用 `elf2e32_next` 的 E32 compressor 解压原 image。
4. 由 [`patch_scdv_e32.cpp`](../scripts/tools/patch_scdv_e32.cpp) 做带原字节断言的最小修改。
5. 重新 DEFLATE、更新 UID checksum/header CRC，并用 `verify_e32.py` 校验 UID、CPU、signature、code offset 和 export table。

macOS 工具位置可覆盖：

```sh
SYMBIAN_ROOT="$HOME/Developer/symbian" \
SYMBIAN_DLL_KIT_ROOT="$HOME/Developer/symbian/symbian-dll-agent-kit" \
scripts/build_scdv_belle_patch.sh /tmp/scdv_general_belle.dll
```

### 二进制修改范围

下列 offset 均相对解压后的 E32 code section：

| Offset | 长度 | 用途 |
|--------|------|------|
| `0x1BDE` | 4 | 将 display-mode dispatch 上界扩到 `EColor16MAP` |
| `0x1D72` | 6 | 让 MAP 走 32-bit alpha 构造并保留请求的 display mode |
| `0x247C` | `0x34` | 替换 `CFbsThirtyTwoBitsDrawDevice::GetInterface` dispatch |
| `0x5010` | `0xA0` | 复用未执行的诊断字符串空间保存 `MSurfaceId` 实现、vtable 和 surface ID |

`surface_stub.S` 的 `.org 0x2B94` 表示第二块代码相对 `GetInterface` 的距离。补丁器只写首尾两个有效区间，不会用 `.org` 中间的零填充覆盖原代码。

生成后可进一步检查：

```sh
python3 ~/Developer/symbian/symbian-dll-agent-kit/tools/verify_e32.py \
  /tmp/scdv_general_belle.dll --uid2 0x10003B19 --uid3 0xEE000002

~/Developer/symbian/symbian-dll-agent-kit/.cache/elf2e32-build/elf2e32 \
  --dump=he --e32input=/tmp/scdv_general_belle.dll
```

不同 host compiler 可能生成不同但等价的 DEFLATE bitstream；验收依据是解压后的 code、header/CRC、31 个 exports 和运行回归，不以压缩文件逐字节相同为前提。

### macOS 工具链交叉校验

macOS 工具链仍可用于独立的源码/ABI 交叉校验：

```sh
cd ~/Developer/symbian/symbian-dll-agent-kit
./scripts/build-dll.sh projects/scdv.env
```

产物位于 `out/scdv_general/`。iOS bundle 当前部署的是 Nokia Belle SDK /
GCCE 生成的完整源码构建。

## 自动回归

先构建并安装 Release 包：

```sh
EKA2L1_IOS_CONFIGURATION=Release scripts/build_ios.sh simulator
scripts/ios_regression_test.sh --install \
  build/ios-simulator/src/emu/ios/Release-iphonesimulator/EKA2L1.app asphalt6
```

已安装 Release 包时直接运行：

```sh
scripts/ios_regression_test.sh asphalt6
```

脚本要求 booted simulator 已安装 rm-707 与 Asphalt 6，并依赖 `xcodebuildmcp`、`jq`、ImageMagick 和 xcodebuildmcp bundled `axe`。它先检查 `mediaclientvideo_v100.dll` 已 staged，并在标题出现前用 guest display band 的像素方差断言捕获到真实 Gameloft 电影帧。两段电影在 dyncom 下可能超过固定的 75 秒，因此脚本还要求 guest band 同时达到交互标题的亮度/方差阈值，才发送 `Touch to continue`。后续页面改用归一化 RMSE 判断整页切换；最终比赛必须同时远离赛前预览和已确认的主菜单，避免 showroom 车辆动画造成假阳性。它会保存以下状态到 `/tmp/eka2l1-regression/`：

- 早期 Gameloft 电影帧
- 两段电影结束后的 Asphalt 标题
- 主菜单
- Nassau track select
- race-mode select
- Mini car select
- 赛前预览
- 实际比赛

除截图差异断言外，脚本还检查新增日志中没有 `panic`、`access violation`、`KERN-EXEC` 或 emulation halt。

## 最终验证

- Asphalt 6：Gameloft 动画与宣传片正常出帧，正式进入 Nassau Normal Race，车辆/赛道/HUD 正常；专用回归 `PASS=9 FAIL=0`
- 标准 Release 回归：`PASS=8 FAIL=0`
- Angry Birds X7 触屏/GLES 回归：`PASS=5 FAIL=0`
- SCDV C++ 完整源码：macOS Symbian DLL 工具链编译成功
- SCDV binary patch：E32 header/UID/export 校验成功
- 最终日志：无 Asphalt guest panic、access violation 或 crash
- iPhone Air 当前离线，新的音频锁序与 CoreMotion IPC 修复仍需在真机复跑；模拟器已覆盖启动、完整片头、比赛与应用切换路径
