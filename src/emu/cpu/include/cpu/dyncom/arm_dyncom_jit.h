/*
 * dyncom hot-block JIT for the WebAssembly build.
 *
 * The browser cannot execute runtime-generated native code, but it CAN
 * compile runtime-generated *WebAssembly*: hot translated blocks are
 * re-compiled into tiny wasm modules (importing the emulator's own shared
 * memory), instantiated synchronously, dropped into the indirect function
 * table and from then on invoked by the interpreter dispatch loop as plain
 * function pointers. Cold / unsupported blocks keep running on the
 * interpreter — the JIT is purely an opportunistic fast path with a bail-out
 * protocol (set Reg[15] to the first un-executed instruction and return).
 *
 * Same idea as the v86 x86 emulator's wasm dynarec.
 */
#pragma once

#include <cstddef>
#include <cstdint>

struct ARMul_State;

namespace eka2l1::arm::dyncom_jit {
    /**
     * Authoritative indices of the instruction kinds the JIT understands,
     * resolved against arm_instruction_trans[] at static-init time in
     * arm_dyncom_trans.cpp (so reordering that table can never silently
     * desync the JIT's decoding).
     */
    struct trans_indices {
        int cmp, tst, teq, cmn;
        int and_, bic, eor, add, rsb, rsc, sbc, adc, sub, orr;
        int mov, mvn, cpy;
        int mul, mla;
        int ldrb, strb, ldr, ldrcond, str;
        int ldrh, strh, ldrsh, ldrsb;
        int ldm1, ldm2, ldm3, stm1, stm2;
        int bbl, bx;
        int b_2_thumb, b_cond_thumb, bl_1_thumb, bl_2_thumb;
    };

    extern const trans_indices indices;

#ifdef __EMSCRIPTEN__
    /// Process-wide default, settable from JS before the core is created
    /// (wasm_set_jit). Per-core state lives in ARMul_State::jit_enabled.
    extern int enabled_default;
    extern int compile_limit;

    /// Compile counters for diagnostics.
    extern std::uint32_t stat_compiled;
    extern std::uint32_t stat_rejected;
    extern std::uint64_t stat_jit_instrs;
    extern std::uint32_t stat_chained;
    extern std::uint32_t stat_blocker_hist[224];

    /**
     * Try to compile the translated block at trans-cache offset `trans_ptr`
     * (guest start `pc`, current TFlag mode). Returns the function-table
     * index (> 0) on success or -2 when the block is not worth compiling /
     * unsupported (callers cache that and never ask again).
     */
    std::int32_t try_compile(ARMul_State *cpu, std::uint32_t pc, std::size_t trans_ptr);

    /// Release a compiled block's table slot back to the free pool.
    void release_index(std::int32_t table_idx);

    /// Invoke a compiled block. Returns guest instructions executed; the
    /// block has updated Reg[]/flags and set Reg[15] for the dispatcher.
    inline int call(std::int32_t table_idx, ARMul_State *cpu) {
        using jit_fn_t = int (*)(ARMul_State *);
        return (reinterpret_cast<jit_fn_t>(static_cast<std::uintptr_t>(table_idx)))(cpu);
    }
#endif
}
