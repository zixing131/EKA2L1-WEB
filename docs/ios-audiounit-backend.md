# iOS 原生 AudioUnit 后端

> 来源：阶段 3.7（[`IOS_PORTING_TASKS.md`](IOS_PORTING_TASKS.md)）。状态：🟡 后端接通，听感与 MIDI bank 等 follow-up 待办。
>
> **一句话结论**：弃用 cubeb shim，iOS 直接接 AURemoteIO + AVAudioSession（`audiounit_ios` 后端），`make_audio_driver(cubeb,…)` 在 iOS 下返回该后端；services 拿到真 audio_driver、加载三个 audio patch DLL。FFmpeg 压缩格式回接见 [iOS DSP / FFmpeg 回接](./ios-dsp-ffmpeg.md)。

- ✅ **不再通过 cubeb**。第一版（commit `a3d20d326`）用一个 cubeb shim 把 RemoteIO 包成 cubeb_ops；user 要求改成"平台原生 API"，于是把 iOS 直接接到 AURemoteIO + AVAudioSession，cubeb 在 iOS 整个不构建：
  - `src/external/CMakeLists.txt`：iOS 重新走 `if (NOT EKA2L1_IOS)` 跳过 `add_subdirectory(cubeb)`。
  - cubeb 子模块回退到上游 `d512bfa07` 干净 SHA（删除 shim 文件 + CMake patch），不再 dirty。
  - ffmpeg 继续 skip（bundled binary 没 iOS cross-build）。
  - 2026-05-24 临时恢复 DSP out：iOS target 仍未接回 FFmpeg headers/libs，直接打开 `dsp_output_stream_ffmpeg` 会在 `libavcodec/avcodec.h` 缺失处编译失败；当前先提供 iOS PCM16/PCM8-only `dsp_output_stream_pcm`，让已解码 PCM 的 DSP out stream 能创建并走 AudioUnit。
- ✅ 新增 `src/emu/drivers/{include,src}/drivers/audio/backend/audiounit_ios/`：
  - `audio_audiounit_ios.{h,mm}` —— `audiounit_ios_audio_driver : public audio_driver`，构造时 `dispatch_once` 配 `AVAudioSession.Playback + MixWithOthers + setActive:YES`，`native_sample_rate()` 直接读 `[AVAudioSession sharedInstance].sampleRate`。
  - `stream_audiounit_ios.{h,mm}` —— `audiounit_ios_stream_base` 持 `AudioUnit`(RemoteIO)，`AudioComponentFindNext + AudioComponentInstanceNew + AudioUnitSetProperty(StreamFormat S16LE interleaved, mChannelsPerFrame=channels, mBitsPerChannel=16) + AURenderCallback + AudioUnitInitialize`；output 用 bus 0 input scope，input 启用 bus 1 + enable IO 1 / 0；`AudioOutputUnitStart/Stop` 走运行控制。`audiounit_ios_output_stream` / `audiounit_ios_input_stream` 是 final 子类，加上 `pause / volume / current_frame_position` 等接口，soft volume + idle-frames 处理与 cubeb 老路径一致（避免 DSP 时间戳跳变）。
- ✅ `drivers/CMakeLists.txt`：iOS 编 `DRIVERS_AUDIOUNIT_IOS_SRC`，桌面 / Android 编 `DRIVERS_CUBEB_SRC` + `DRIVERS_FFMPEG_SRC`。`drivers` iOS link 去掉 `cubeb`，保留 `AudioToolbox / CoreAudio / AVFoundation` 系统 framework。
- ✅ `audio.cpp::make_audio_driver(audio_driver_backend::cubeb, ...)` 在 `EKA2L1_PLATFORM(IOS)` 下返回 `audiounit_ios_audio_driver`，桌面 / Android 不变（cubeb 枚举名保留是有意的：上层 services 用同一个 backend 标签）。
- ✅ `IosEmulator`：未改动 —— 仍按 3.7 一版调 `make_audio_driver(cubeb, master_vol, player_type_tsf)`、`set_bank_path(HSB/SF2)`、塞进 `system_create_components.audio_`，mount 后 `sys->set_audio_driver(...)`；`-pause` / `-resume` 仍 `AVAudioSession setActive:NO/YES`，但 session 配置现在由 audio_driver 构造统一负责，避免两边重复 `dispatch_once`。
- ✅ xcodebuildmcp 验证（iPhone 16 Pro / iOS 26.5 sim）：cubeb 不再编入；build SUCCEEDED；mount N95 → 日志仍出 `mediaclientaudiostream_general.dll` / `mediaclientaudio_general.dll` / `audiooutputrouting_general.dll` 三个 audio patch DLL（证明 services 拿到了真 audio_driver）；applist 64 app；Calculator UI 渲染稳定 ≥10s，无新 `.ips`。截屏 `docs/screenshots/ios-stage3/3.7-audio/calculator-native-audiounit.jpg`。
- 🟡 剩余 follow-up：①真的"能听见声音"要选一个会出声的 ROM 应用（候选：N-Gage demo / Music Player / 一段 SIS 带 BGM 的小游戏），跑起来人耳确认 —— 不能用 xcodebuildmcp 自动断言；②MIDI 用 TSF 后端走 file-based bank，HSB/SF2 路径默认空，sandbox 里没把 `defaultbank.hsb` / `defaultbank.sf2` 拷过去（先前没人需要），有需要时和字体一样在 startup 里复制；③Audio input（mic）路径已经接好但未实测，3.x 没有需要录音的功能流程。
