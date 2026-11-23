# iOS CoreMotion 异步 IPC 写回崩溃

## 结论

TestFlight `26.7.0 (260729)` 的 `.crash` 不是 CoreMotion 或 UIKit 自身崩溃，而是 Sensor Framework 的 host 异步完成与 guest 取消/回收并发：CoreMotion 工作线程在取得 kernel lock **之前**读取并移走 pending `ipc_context`，guest 随后可执行 `StopListening`、关闭 channel 或销毁 client buffer；host 再写 slot 2 的计数 descriptor 时得到空 guest 指针并在 `desc_base::get_max_length()` 崩溃。

修复将传感器完成统一串行到 kernel lock 内，并用独立 callback state 隔离 service session 生命周期；无效 requester/descriptor 返回 Symbian 错误而不是让 host 解引用空地址。修复是 Sensor Framework/IPC 通用语义，不包含应用 UID 特判。

## Crash 与符号表

报告：`crashes/TestFlight – EKA2L1 26.7.0 (260729).crash`

- 设备：iPhone14,5，iOS 18.7.8
- 异常：`EXC_BAD_ACCESS / SIGSEGV`，地址 `0x0`
- EKA2L1 image UUID：`6C1BC4A4-5344-30E3-BBCF-40254DE7BC7D`
- 触发线程：CoreMotion `NSOperationQueue` worker

匹配符号表来自 GitHub Actions TestFlight run `29094599295`、commit `9c1736491f17c42bfbcc8a4b5eaa4f860f47764f`：

```sh
gh run download 29094599295 \
  -n EKA2L1-testflight-dSYM-9c1736491f17c42bfbcc8a4b5eaa4f860f47764f \
  -D /tmp/eka2l1-sensor-dsym

dwarfdump --uuid /tmp/eka2l1-sensor-dsym/EKA2L1.app.dSYM
```

dSYM UUID 与 crash 完全匹配。崩溃栈为：

```text
epoc::desc_base::get_max_length(process*)       // this == nullptr
service::ipc_context::write_data_to_descriptor_argument(...)
sensor_client_session::complete_channel_data_request(...)
drivers::sensor_driver_ios::dispatch_sample(...)
CoreMotion operation callback
```

ARM 状态的 `x0=0` 确认被调用的 descriptor 对象为空；写入长度为 8，对应 slot 2 的 `TSensrvAsyncChannelDataCountsRetval`。

## IPC 契约与竞态

Symbian Sensor Framework 的 `ESensrvSrvReqAsyncChannelData` 定义两个必需输出 descriptor：

- slot 1：传感器数据 buffer
- slot 2：8-byte returned/lost item counts package

官方 client 的 `CSensrvDataHandler::CreateAndSendRequest()` 同时传入 `iWriteBufferPtr` 与 `iDataCountsPckgBuf`；取消时 `DoCancel()` 同步发送 `ESensrvSrvReqStopListening`。因此 server 必须保证 stop 返回后不再访问两块 client memory。

旧实现的时序可复现为：

```text
CoreMotion worker                     guest/HLE thread
-----------------------------------   --------------------------------
take_ready_batch()
move pending ipc_context out of map
read slot 1 max size successfully
                                      StopListening / CloseChannel
                                      returns; client frees descriptors
lock kernel (too late)
write slot 2 -> null descriptor crash
```

此外 backend callback 捕获裸 `this`，已从 driver ready list 取出的 callback 还可能晚于 session 析构执行，形成同源的 session UAF 风险。

## 修复

1. `sensor_client_session` 为 backend callback 创建 shared lifetime state。state 只保存 kernel 指针和 kernel lock 保护的 session 指针；session 析构第一步将指针置空。
2. backend callback 在访问 session、pending map、requester 或 descriptor **之前**取得 kernel lock，使完成与 `StopListening`、`CloseChannel`、session teardown 串行。
3. kernel lock 内检查 `is_wiping()`、requester thread 是否仍存活，以及两个 output descriptor 是否仍有效；失效请求直接丢弃或以 `KErrBadDescriptor`/`KErrOverflow` 完成。
4. `StopListening` 与 `CloseChannel` 都取消 pending IPC 并清理 request timestamp；iOS controller 取消监听时清空尚未进入 ready list 的 callback/buffer。
5. `ipc_context` 的 descriptor write/size/length helpers 增加空 guest descriptor 防御，错误输入不再演变为 host crash。

锁序保持为 `kernel -> sensor driver list/controller`；CoreMotion 收集 ready batch 时仍先释放 driver locks，再进入 service callback，避免引入反向 ABBA。

## 验证

- 匹配 CI dSYM 精确符号化：通过
- Release iOS simulator build：通过
- Release iphoneos unsigned build：通过；版本化 `mediaclientvideo_v100.dll` 同时确认已进入 device `.app`
- 标准 Release 回归：连续两轮 `PASS=8 FAIL=0`
- Asphalt 6 完整片头/比赛回归：`PASS=9 FAIL=0`
- Angry Birds Symbian^3/GLES/触控回归：`PASS=5 FAIL=0`

iOS Simulator 不提供真实 accelerometer channel，无法直接重放 CoreMotion 样本；iPhone Air 在本轮验证时离线，最终仍需在物理设备用重力感应游戏反复执行「启动监听 → 退出/关 channel」确认。修复的序列化点覆盖 crash 中的确定竞态，不依赖采样值或具体游戏。
