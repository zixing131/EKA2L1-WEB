# ARM/Thumb → WASM JIT 算术优化（2026-09）

基线提交：`92a34a413`。本次修改运行时 WASM 代码生成器，不改变解释器或 JIT 热点阈值。

## 修改

原先带标志位的 ADD、SUB、RSB、CMP、CMN 都通过通用的 64 位 AddWithCarry 实现，包含操作数扩展、额外进位加法、结果截断和高位提取。

现在使用 32 位加减法计算结果，通过无符号比较计算 C，通过异或和符号位提取计算 V。减法的 C 仍表示「没有借位」。N/Z、条件执行和 Thumb 转译保持原有行为；ADC、SBC、RSC 继续使用支持动态进位的原实现。生成器先保存两个操作数，避免 RSB 的输入被结果临时变量覆盖。

## 性能

测试机器为 macOS arm64，Emscripten 5.0.7 Release，Node v26.7.0。基准运行实际生成的 WASM：每轮 340 万条 ARM 指令，包括 ADDS、SUBS、CMP、消费进位的 ADC 和循环分支，且核对解释器与 JIT 的最终寄存器及 CPSR。

每个进程先预热一次，再取 7 次运行的中位数；新旧构建交替顺序运行 12 轮，最后取各自中位数：

- 优化前：3.5585 ms（12 轮范围 3.486–3.581 ms）。
- 优化后：3.2900 ms（12 轮范围 3.237–3.342 ms）。
- 此算术微基准耗时减少 **7.5%**。

这不是整机模拟速度或游戏 FPS 的提升比例；浏览器引擎、应用指令分布及其他瓶颈会影响实际收益。

## 回归测试

新增显式构建目标 `wasmjit_difftest`，复用宿主差分测试的内存夹具和独立 ARM 算术模型，在 Emscripten 中真正编译、安装并执行 JIT WASM 模块。

- 4,810,624 组 ARM/Thumb 输入状态，比较 JIT、解释器和独立模型的全部寄存器及 CPSR。
- 覆盖 15 种条件码、S 位、全部 16 种 NZCV 输入、边界值和随机操作数、目标寄存器重叠、相同操作数、立即数旋转、LSL/LSR/ASR 的边界移位。
- 覆盖优化的五种运算及相邻的 ADC/SBC/RSC，另包含 Thumb ADD/SUB/CMP/CMN。
- 每个指令块预热后断言 JIT 指令计数增长，防止测试悄悄只走解释器。
- 多指令循环基准验证标志位消费、自循环和解释器退出路径。

完整 Web Release 构建通过，回归页计算器验证 `6 + 9 = 15`；Jelly Chase 正常进入关卡并持续运行，日志确认使用 JIT。额外尝试构建旧的宿主 `dyncom_difftest` 时，发现其既有 VFP 测试仍引用已不存在的 `vfp_set_single_host_fast_for_test` 等接口，因而无法构建；这不是本次 WASM 差分目标的测试结果。

在已配置的 Emscripten Release 构建目录中运行（先加载对应的 `emsdk_env.sh`）：

```sh
cmake --build build_wasm_release --target wasmjit_difftest -j6
node build_wasm_release/bin/jit-tests/wasmjit_difftest.js
```

该目标不参与默认前端构建，不需要 ROM，不写入浏览器存档。应用回归页仍为 `http://127.0.0.1:18091/smoke.html`，服务由 `python3 buildscript/serve_regression.py` 启动。
