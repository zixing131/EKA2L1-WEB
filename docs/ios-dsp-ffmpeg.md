# iOS DSP / FFmpeg 回接

> 来源：阶段 3.7.1（[`IOS_PORTING_TASKS.md`](IOS_PORTING_TASKS.md)）。状态：🟡 simulator 路径回接，device 路径与真实解码验证待办。
>
> **一句话结论**：新增 `scripts/build_ios_ffmpeg.sh` 用 bundled FFmpeg source out-of-tree 构建 iOS static libs（不 dirty 子模块），CMake 在 `EKA2L1_HAVE_FFMPEG` 时编回 `dsp_ffmpeg` / `player_ffmpeg` / `video_player_ffmpeg`，否则保留 PCM16/PCM8 fallback。承接 [iOS 原生 AudioUnit 后端](./ios-audiounit-backend.md)。

- ✅ 2026-05-24 simulator 路径已回接：新增 `scripts/build_ios_ffmpeg.sh`，用 bundled FFmpeg source out-of-tree 构建 iOS static libs 到 `build/ios-<device|simulator>/ios-ffmpeg`，不 dirty `src/external/ffmpeg` 子模块；`scripts/build_ios.sh` 在配置 iOS 前自动构建对应 FFmpeg 产物，并传 `EKA2L1_IOS_ENABLE_FFMPEG=ON` / `EKA2L1_IOS_FFMPEG_ROOT=...`。
- ✅ CMake 回接：`src/external/CMakeLists.txt` 在 iOS 下把 `libavformat/libavcodec/libswscale/libavutil/libswresample` 暴露成 `ffmpeg` interface target；`drivers` 在 `EKA2L1_HAVE_FFMPEG` 时编回 `dsp_ffmpeg.cpp` / `player_ffmpeg.cpp` / `video_player_ffmpeg.cpp`，并定义 `EKA2L1_HAS_FFMPEG=1`。如果显式关闭 FFmpeg，iOS 仍保留 PCM16/PCM8-only fallback，避免回到空 stream。
- ✅ 验证：`scripts/build_ios.sh simulator` 通过；Xcode build 中 drivers 编译 `video_player_ffmpeg.cpp`，最终 app link line 包含五个 iOS FFmpeg static libs；`nm` 确认 `libdrivers.a` 内已有 `dsp_output_stream_ffmpeg` / `player_ffmpeg` 符号。
- 🟡 剩余：device 路径使用同一脚本支持 `iphoneos`，但本轮尚未跑 `scripts/build_ios.sh device`；运行时还需要用压缩 DSP sample 或 MP3/AMR 应用验证真实解码，并复测 Final Battle 无 `Unable to create new DSP out stream!` / `KERN-EXEC` / `Access violation`。
