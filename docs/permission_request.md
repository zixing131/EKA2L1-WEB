# EKA2L1 模拟器鸿蒙 NEXT 版 — JIT 相关受限权限申请说明

> **提交方**：EKA2L1 项目组  
> **目标平台**：HarmonyOS NEXT（API 14+）  
> **实测设备**：nova 12 Ultra / 麒麟 8000 / arm64 / HarmonyOS 6.1.0  
> **申请目的**：为上架 AppGallery 的商店签名版本申请 JIT 相关受限开放权限。在麒麟 8000 真机实测中，商店签名下 dyncom 解释器模式所有游戏均不足 10 帧，产品形同虚设；获得 JIT 权限后方可使商店版本具备实用价值。  
> **关联文档**：[EKA2L1 鸿蒙原生版开发记录](https://github.com/zixing131/EKA2L1-WEB/blob/master/docs/hos_dev.md)

---

## 一、项目背景

**EKA2L1** 是一个开源的 Symbian OS / N-Gage 模拟器，在 GitHub 上拥有活跃的社区。项目当前正在进行**鸿蒙 NEXT 原生版**的移植工作，目标是以原生 ArkTS/ArkUI + NAPI C++ 的架构，将模拟器完整运行在鸿蒙设备上。

### 技术架构

| 层级 | 技术栈 |
|---|---|
| 界面层 | ArkTS / ArkUI（`@Component`、`XComponent`） |
| 桥接层 | NAPI（对标 Android JNI） |
| 模拟核心 | C++ 交叉编译（OpenHarmony NDK / BiSheng clang） |
| CPU 模拟 | Dynarmic JIT 引擎（AArch64 host → ARM guest 动态重编译） |
| 图形渲染 | XComponent + EGL + OpenGL ES 3.0 |
| 音频 | Audio Kit `AudioRenderer`（PCM 回灌） |

### 当前状态与实测设备

| 测试设备 | 芯片平台 | 系统版本 |
|---|---|---|
| nova 12 Ultra（ADL-AL00U） | 麒麟 8000（arm64） | HarmonyOS 6.1.0 |

- ✅ ArkUI 界面完成（主页/运行页/设置页）
- ✅ XComponent + EGL/GLES 渲染通路调通
- ✅ ROM 安装、SIS 安装、应用启动流程完成
- ✅ 音频驱动（静音流 → 待接入 AudioRenderer）
- ⚠️ **CPU 模拟**：开发签名下 JIT 正常，商店签名下自动降级为 dyncom 解释器 —— 解释器模式下实测性能极差

---

## 二、为什么需要 JIT 权限

### 2.1 Dynarmic JIT 引擎的工作原理

EKA2L1 使用 **Dynarmic** 作为 CPU 模拟核心。Dynarmic 是一个高性能的动态重编译（Dynarec）引擎，其工作流程如下：

```
1. mmap(PROT_NONE)         分配匿名内存页
2. mprotect(PROT_READ|PROT_WRITE)  切换为可写
3. 将 guest ARM 指令翻译为 host AArch64 机器码，写入内存
4. mprotect(PROT_READ|PROT_EXEC)   切换为可执行（W^X 策略）
5. 跳转到该内存执行生成的机器码
6. 循环回到步骤 2（需要修改码缓存时）
```

这个过程中，步骤 4 的 `mprotect(PROT_READ|PROT_EXEC)` 是 **JIT 的关键操作**——它需要将一块运行时动态写入的内存标记为可执行。

### 2.2 鸿蒙 NEXT 对商店签名应用的限制

鸿蒙 NEXT 通过 **W^X（Write XOR Execute）沙箱策略**和 **代码完整性保护** 拦截了上述操作：

| 阻断点 | 机制 | 对 EKA2L1 的影响 |
|---|---|---|
| `mprotect(PROT_EXEC)` | 内核 MAC/SELinux 拒绝商店签名应用创建可执行内存映射 | Dynarmic 无法分配可执行页 → SIGSEGV 崩溃 |
| 代码完整性校验 | 系统检测到运行时被修改的代码页 → 判定为异常 | RW→RX 翻转后的页被拒绝执行 → SIGABRT |

### 2.3 降级方案（dyncom 解释器）的真实性能代价

项目已实现运行时探测降级：如果检测到无法获取可执行内存，自动切换到 **dyncom 解释器**。在麒麟 8000（nova 12 Ultra）真机上实测，解释器模式下的性能数据如下：

| 游戏类型 | dyncom 解释器（商店签名） | 可玩标准 | 结论 |
|---|---|---|---|
| 2D 游戏（如贪吃蛇、益智类） | **< 10 FPS** | ≥ 30 FPS | ❌ 不可玩 |
| 3D N-Gage 游戏 | **3-8 FPS** | ≥ 25 FPS | ❌ 严重不可玩 |
| 系统菜单 / 文件浏览 | **8-15 FPS** | ≥ 30 FPS | ❌ 卡顿严重 |

> 即便是最简单的 2D 游戏，在麒麟 8000 级别的芯片上也无法达到 10 帧，操作响应延迟超过 500ms，用户体验等同于**完全不可用**。

**根本原因**：dyncom 是纯解释器，每执行一条 guest ARM 指令需要数百条 host AArch64 指令的逐字段解码和分支跳转。Dynarmic JIT 则将整段 guest 代码**一次性重编译为 host 本地码**，后续直接执行，性能差距在 **20-50 倍**级别。在麒麟 8000 这样的中高端芯片上，解释器模式的性能瓶颈完全在 CPU 模拟吞吐量，而非 GPU 渲染。

这意味着：
- **若不获批 JIT 权限，EKA2L1 在鸿蒙平台上的商店版本形同虚设**——能装能开但所有游戏都不可玩
- 这与鸿蒙生态追求的高品质应用体验目标**背道而驰**。用户下载后必然是差评和卸载，反而损害平台口碑

---

## 三、申请权限清单及理由

### 第一梯队：核心 JIT 权限（必须）

#### 1. `ohos.permission.kernel.ALLOW_WRITABLE_CODE_MEMORY`

| 字段 | 内容 |
|---|---|
| **权限名称** | `ohos.permission.kernel.ALLOW_WRITABLE_CODE_MEMORY` |
| **权限说明** | 允许应用申请可写可执行匿名内存 |
| **使用场景** | Dynarmic JIT 引擎在运行时动态生成 AArch64 机器码，需要通过 `mmap` + `mprotect` 分配 RW/RX 内存页来存放和执行为 Symbian guest 系统重编译的代码块 |
| **调用位置** | `src/emu/common/src/virtualmem.cpp`（`map_memory`）、Dynarmic 的 `A32AddressSpace::EmitPrelude()` |
| **设备范围** | 平板、2in1 设备 |

#### 2. `ohos.permission.kernel.DISABLE_CODE_MEMORY_PROTECTION`

| 字段 | 内容 |
|---|---|
| **权限名称** | `ohos.permission.kernel.DISABLE_CODE_MEMORY_PROTECTION` |
| **权限说明** | 允许应用禁用本应用的代码运行时完整性保护 |
| **使用场景** | JIT 引擎在 W^X 策略下需要在 RW 和 RX 之间反复翻转内存页属性，每次翻转都会触发系统的代码完整性校验，导致页被拒绝执行或进程被杀。禁用该保护后，Dynarmic 可以安全地管理自己的码缓存页，而不触发完整性误判。 |
| **调用位置** | `src/emu/common/src/virtualmem.cpp`（`is_memory_wx_exclusive()` 返回 true 时走 W^X 安全翻转路径） |
| **设备范围** | 平板、2in1 设备 |

### 第二梯队：JITFort 安全路径（推荐申请）

#### 3. `ohos.permission.kernel.ALLOW_EXECUTABLE_FORT_MEMORY`

| 字段 | 内容 |
|---|---|
| **权限名称** | `ohos.permission.kernel.ALLOW_EXECUTABLE_FORT_MEMORY` |
| **权限说明** | 允许申请带 MAP_FORT 标识的匿名可执行内存 |
| **使用场景** | 优先使用鸿蒙官方提供的 JITFort 安全内存分配机制，替代裸 `mmap(RWX)`。JITFort 提供了系统级的 JIT 内存管理，比自行管理 RW↔RX 翻转更安全、更合规。Dynarmic 的码缓存可以改造为通过 MAP_FORT 分配。 |
| **申请理由** | 使用官方 JIT 方案而非裸 RWX，体现我们对鸿蒙安全最佳实践的支持 |

#### 4. `ohos.permission.kernel.ALLOW_USE_JITFORT_INTERFACE`

| 字段 | 内容 |
|---|---|
| **权限名称** | `ohos.permission.kernel.ALLOW_USE_JITFORT_INTERFACE` |
| **权限说明** | 允许调用 JITFort 接口更新 MAP_FORT 内存的内容 |
| **使用场景** | 配合 `ALLOW_EXECUTABLE_FORT_MEMORY`，Dynarmic 在需要修改已分配的码缓存时，通过 `JITFort` 安全接口做 RW↔RX 翻转，替代裸 `mprotect` 调用。这样整个 JIT 生命周期都在系统管控之下。 |
| **申请理由** | 走鸿蒙官方 JITFort 通道，全程可控可审计，不绕过系统安全机制 |

### 第三梯队：辅助权限（视审核反馈决定是否申请）

#### 5. `ohos.permission.kernel.DISABLE_GOTPLT_RO_PROTECTION`

| 字段 | 内容 |
|---|---|
| **权限名称** | `ohos.permission.kernel.DISABLE_GOTPLT_RO_PROTECTION` |
| **权限说明** | 允许关闭 .got.plt 段的只读保护 |
| **使用场景** | Dynarmic 在多模块动态链接场景下可能需要写入 GOT/PLT。如果 JIT 编译的 thunk 代码需要调用宿主 .so 中的函数，写入 PLT 可能被拦截 |
| **优先级** | 低（当前架构下可能不需要，视后续适配情况） |

---

## 四、为什么这不是"热更新"——安全合规声明

我们充分理解鸿蒙对 **"热更新"** 和 **"动态代码执行"** 的安全顾虑。EKA2L1 的 JIT 使用与热更新有**本质区别**：

| 维度 | 热更新（被禁止） | EKA2L1 JIT（本申请） |
|---|---|---|
| **代码来源** | 远程下载的不可信代码 | **本地确定性生成的机器码**（guest ARM 指令 → host AArch64 翻译） |
| **执行范围** | 可能影响宿主应用逻辑 | **严格限制在沙箱内的模拟 CPU 上下文** |
| **可审计性** | 无法预知执行内容 | **完全可预测、可复现**（同一条 guest 指令永远翻译为同一段 host 码） |
| **攻击面** | 扩大了应用的代码攻击面 | **不增加宿主攻击面**：执行的码来自 ROM 文件中的 Symbian 系统，guest 代码只在模拟器虚拟 CPU 中运行 |
| **目的** | 绕过应用审核更新功能 | **核心功能本身就需要动态翻译**（模拟器的本质） |

**本质区别总结**：热更新是从外部引入新代码来**改变应用自身行为**；而 EKA2L1 的 JIT 是把**已有的 guest 系统指令**翻译为 host 可执行的格式——这是一种编译技术，不是代码注入。

---

## 五、技术方案对比：JIT vs 解释器（麒麟 8000 真机实测）

| 对比维度 | Dynarmic JIT（申请权限后，预估） | dyncom 解释器（当前商店签名实测） |
|---|---|---|
| 实现原理 | guest ARM → host AArch64 动态重编译 | guest ARM 逐条解释执行 |
| 系统菜单/文件浏览 | 流畅（60 FPS） | **严重卡顿（8-15 FPS）** |
| 2D 游戏（贪吃蛇、益智类） | 流畅（60 FPS） | **不可玩（< 10 FPS，操作延迟 >500ms）** |
| 3D N-Gage 游戏 | 可玩（30-60 FPS） | **完全不可玩（3-8 FPS，等同于幻灯片）** |
| CPU 模拟吞吐量 | 高（批量重编译，一次翻译多次执行） | 极低（每条指令数百条 host 指令开销） |
| 内存占用 | 较高（码缓存 ~16MB） | 较低（无码缓存） |
| 代码成熟度 | 高（同 Android 版多年验证） | 中（fallback 路径，仅基础测试） |
| 用户体验 | ✅ 良好，可作为正式产品发布 | ❌ **形同虚设，无实用价值** |
| 安全合规 | 走 JITFort → 系统管控 | 无需权限 → 但产品不可用 |

---

## 六、申请范围与兼容性承诺

### 6.1 设备范围

本次申请仅针对 **平板（Pad）和 2in1 设备**：

- 理解并接受这两个权限在手机上不可用的限制
- 手机端不上架原生版，或仅以 WASM 版（WebAssembly + ArkWeb）封装上架；WASM 方案不涉及 JIT，无需本权限
- JIT 权限仅对平板/2in1 设备生效

### 6.2 运行时探测与自适应

代码中已实现 `is_executable_memory_available()` 运行时探测（位置：`src/emu/common/src/virtualmem.cpp`）：

```cpp
// 运行时探测逻辑（已实现）
bool is_executable_memory_available() {
    // 1. mmap 一页 PROT_READ|PROT_WRITE
    // 2. 写入一条 RET 指令（AArch64: 0xD65F03C0）
    // 3. mprotect 为 PROT_READ|PROT_EXEC
    // 4. __builtin___clear_cache 同步 I-cache
    // 5. 实际执行一次，成功则返回 true
    // 结果缓存，每次启动只探测一次
}

// 选核逻辑（已实现，位置：src/emu/system/src/epoc.cpp）
cpu_type = is_executable_memory_available() ? dynarmic : dyncom;
```

**这意味着**：
- ✅ 权限获批 → 自动使用 Dynarmic JIT（高性能）
- ✅ 权限未获批 → 自动降级 dyncom 解释器（保底可运行）
- ✅ 同一 HAP 二进制自适应，无需多版本维护
- ✅ 非平板/2in1 设备自动走降级路径，不会崩溃

### 6.3 承诺

1. **不用于热更新**：JIT 生成的代码仅限于 guest 指令翻译，不用于加载任何远程代码
2. **不绕过安全机制**：优先适配 JITFort，走官方推荐的 JIT 安全通道
3. **可审计**：代码开源（GitHub），任何人均可审查 JIT 使用方式
4. **W^X 合规**：即使获得权限，仍遵守 W^X 策略——木有任何时刻一块内存同时具有 W+X，始终在 RW↔RX 之间翻转

---

## 七、附录：相关代码路径索引

| 功能 | 代码位置 |
|---|---|
| 运行时探测可执行内存 | `src/emu/common/src/virtualmem.cpp` → `is_executable_memory_available()` |
| W^X 策略判定 | `src/emu/common/src/virtualmem.cpp` → `is_memory_wx_exclusive()` |
| CPU 核心选型（JIT / 解释器） | `src/emu/system/src/epoc.cpp` → `system_impl` 构造函数 |
| 内存保护属性翻译（RWX → RW 降级） | `src/emu/common/src/types.cpp` → `translate_protection()` |
| 内存映射 | `src/emu/common/src/virtualmem.cpp` → `map_memory()` |
| Dynarmic JIT 后端 | `src/external/dynarmic/` |
| EGL 上下文（XComponent 渲染） | `src/emu/drivers/src/graphics/backend/context_egl_ohos.cpp` |
| NAPI 桥接（JS ↔ C++） | `src/emu/hos/entry/src/main/cpp/napi_init.cpp` |
| ArkUI 主界面 | `src/emu/hos/entry/src/main/ets/pages/Index.ets` |

---

## 八、联系方式

如有任何关于权限申请的技术问题需要进一步沟通，欢迎随时联系。

---

> **一句话总结**：EKA2L1 是一个跨平台模拟器项目，核心 CPU 模拟使用标准 JIT 编译技术（非热更新、非代码注入）。在麒麟 8000（nova 12 Ultra）真机实测中，因商店签名无法获取可执行内存而被迫降级到 dyncom 解释器模式，导致**即使是简单 2D 游戏也不足 10 帧**，产品形同虚设。需要申请 `ALLOW_WRITABLE_CODE_MEMORY` + `DISABLE_CODE_MEMORY_PROTECTION` 权限（优先适配 JITFort 安全通道），仅用于平板/2in1 设备，使商店版本获得可接受的性能。项目代码开源可审计，已实现权限缺失时的自适应降级保底。
