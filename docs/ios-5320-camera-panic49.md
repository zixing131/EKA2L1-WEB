# 5320 Camera 真机 `E32USER-CBase 49`

## 现象

在 iPhone Air 真机运行 Nokia 5320d-1（rm-409）的内置 Camera（UID
`0x101F857A`）时，应用打开后发生 guest panic：

```text
Unable to find jump for patched address 0x8019DB8C (impossible)
Thread Camera panicked with category: E32USER-CBase and exit code: 49
```

此问题只能在有真实相机设备的路径复现；模拟器无法覆盖相机初始化流程。

## 根因与修复

问题由三个连续缺陷组成：

1. **SVC ROM patch trampoline 使用了过期线程上下文。** `SVC #0xFF` 回调发生时，
   CPU backend 已将实时 PC 前移，但 `jump_trampoline_through_svc()` 从尚未保存的
   `thread_context` 读取 PC，并向同一过期 context 写回跳转目标。因此
   `trampoline_lookup_` 查找失败，且原定的 ecam patch 函数没有执行。现改为直接读写
   live CPU core，并按 ARM/Thumb 指令宽度还原 SVC 地址，同时同步目标地址的 Thumb 位。
   这是共享内核修复，不包含 Camera 或 iOS 特判。
2. **ECam dispatcher 缺失 `ECamDuplicate`（ordinal `0x71`）。** 5320 Camera 会调用该
   接口，旧实现只记录 `Can't find dispatch function 113`。现注册并实现 duplicate；由于
   camera dispatcher 没有独立 close 操作，duplicate 共享原 camera 实例和 handle，返回
   缓存的 camera info，保持正确生命周期。
3. **iOS backend 的能力声明与参数实现不一致。** `get_info()` 声明
   `CAPTURE_OPTION_ALL`，但 optical zoom、contrast、brightness、white balance 等参数
   返回 unsupported，guest patch 随即 `Leave`；在该旧 ROM 的低地址 patch DLL 异常展开
   路径中最终形成 access violation。现对尚无 AVFoundation 映射的标量参数提供实例级
   缓存式 get/set，和已有 exposure/digital zoom/flash 行为一致，使声明的参数契约完整。

## 验证

- iPhone Air + rm-409：清理诊断代码后的最终包直接启动 Camera 并运行约 55 秒；日志中不再出现
  panic 49、patched-address lookup miss、dispatch 113 缺失、unsupported camera
  parameter 或 access violation。
- Release 模拟器回归连续两轮 **8/8** 通过（首轮安装 Release 包，第二轮不重装）；模拟器
  仅验证共享行为无回归，不作为真实相机路径的功能验收。

调查期间加入的 `PATCHDBG` / `CAMDBG` 诊断探针均已删除。
