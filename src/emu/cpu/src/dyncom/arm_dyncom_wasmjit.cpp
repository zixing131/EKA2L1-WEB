/*
 * Hot-block ARM -> WebAssembly JIT for the Emscripten build of dyncom.
 * See arm_dyncom_jit.h for the design overview.
 *
 * Compilation source is the *translated* arm_inst stream (the same structs
 * the interpreter executes), so Thumb is already normalised and all decode
 * quirks are inherited from the translator. Each compiled block becomes one
 * little wasm module with a single function `(param i32 state)(result i32
 * executed)` that
 *   - keeps NZCV in locals (mirroring the interpreter's LOAD/SAVE_NZCVT
 *     dummy-flag pattern),
 *   - accesses guest memory through the same r12l1 TLB the interpreter uses
 *     (inline lookup; a miss bails out to keep every slow-path semantic),
 *   - on any bail sets Reg[15] to the first UN-executed instruction and
 *     returns how many instructions completed.
 *
 * Anything the compiler does not understand ends the block early (or rejects
 * it when the supported prefix is too short) — correctness never depends on
 * coverage, only speed does.
 */

#ifdef __EMSCRIPTEN__

#include <cpu/dyncom/arm_dyncom_jit.h>

#include <common/log.h>
#include <cpu/dyncom/arm_dyncom_trans.h>
#include <cpu/dyncom/armstate.h>

#include <emscripten/em_js.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>

// ARMul_State is not standard-layout (it has private members), but its
// public data members are at stable offsets; clang computes them fine.
#pragma clang diagnostic ignored "-Winvalid-offsetof"


// Compile a wasm module from heap bytes and install its export into the
// shared indirect function table. Returns the table index, or -1 on failure.
// `reuse_idx` >= 0 refills a previously released slot instead of growing.
EM_JS(int, dyncom_wasmjit_install, (const unsigned char *bytes, int len, int reuse_idx), {
    try {
        var mod = new WebAssembly.Module(HEAPU8.subarray(bytes, bytes + len));
        var inst = new WebAssembly.Instance(mod, { env: { memory: wasmMemory } });
        var idx = (reuse_idx >= 0) ? reuse_idx : wasmTable.grow(1);
        wasmTable.set(idx, inst.exports.f);
        return idx;
    } catch (e) {
        console.warn('[dyncom-jit] compile failed:', e);
        return -1;
    }
});

namespace eka2l1::arm::dyncom_jit {

    int enabled_default = 1;
    int compile_limit = 0x7FFFFFFF; // bisect aid: stop compiling after N blocks
    std::uint32_t stat_compiled = 0;
    std::uint32_t stat_rejected = 0;
    std::uint64_t stat_jit_instrs = 0;
    // First unsupported instruction kind per compile attempt: tells us exactly
    // which instruction to teach the JIT next.
    std::uint32_t stat_blocker_hist[224] = {};

    static std::vector<std::int32_t> g_free_indices;

    void release_index(std::int32_t table_idx) {
        if (table_idx > 0) {
            g_free_indices.push_back(table_idx);
        }
    }

    // ---- wasm opcodes --------------------------------------------------------

    namespace op {
        enum : std::uint8_t {
            LOCAL_GET = 0x20,
            LOCAL_SET = 0x21,
            LOCAL_TEE = 0x22,
            I32_LOAD = 0x28,
            I32_LOAD8_S = 0x2C,
            I32_LOAD8_U = 0x2D,
            I32_LOAD16_S = 0x2E,
            I32_LOAD16_U = 0x2F,
            I32_STORE = 0x36,
            I32_STORE8 = 0x3A,
            I32_STORE16 = 0x3B,
            I32_MUL = 0x6C,
            I32_LT_U = 0x49,
            I32_CONST = 0x41,
            I32_EQZ = 0x45,
            I32_EQ = 0x46,
            I32_NE = 0x47,
            I32_ADD = 0x6A,
            I32_SUB = 0x6B,
            I32_AND = 0x71,
            I32_OR = 0x72,
            I32_XOR = 0x73,
            I32_SHL = 0x74,
            I32_SHR_S = 0x75,
            I32_SHR_U = 0x76,
            I64_ADD = 0x7C,
            I64_SHR_U = 0x88,
            I32_WRAP_I64 = 0xA7,
            I64_EXTEND_I32_U = 0xAD,
            I64_CONST = 0x42,
            IF = 0x04,
            ELSE_OP = 0x05,
            END = 0x0B,
            RETURN = 0x0F,
            BLOCKTYPE_VOID = 0x40,
        };
    }

    // Locals. 0 is the state-pointer parameter; 1..9 are i32, 10 is i64.
    enum : std::uint8_t {
        L_STATE = 0,
        L_N = 1,
        L_Z = 2,
        L_C = 3,
        L_V = 4,
        L_T0 = 5, // lhs / result
        L_T1 = 6, // rhs / operand2
        L_T2 = 7, // scratch (CLOBBERED by every TLB resolve!)
        L_ADDR = 8,
        L_T3 = 9, // survives TLB resolves (LDM/STM original Rn)
        L_W64 = 10, // i64 scratch for AddWithCarry
    };

    struct wasm_writer {
        std::vector<std::uint8_t> b;

        void u8(std::uint8_t v) { b.push_back(v); }
        void uleb(std::uint64_t v) {
            do {
                std::uint8_t byte = v & 0x7F;
                v >>= 7;
                if (v) byte |= 0x80;
                b.push_back(byte);
            } while (v);
        }
        void sleb(std::int64_t v) {
            bool more = true;
            while (more) {
                std::uint8_t byte = v & 0x7F;
                v >>= 7;
                if ((v == 0 && !(byte & 0x40)) || (v == -1 && (byte & 0x40)))
                    more = false;
                else
                    byte |= 0x80;
                b.push_back(byte);
            }
        }

        void local_get(std::uint8_t l) { u8(op::LOCAL_GET); uleb(l); }
        void local_set(std::uint8_t l) { u8(op::LOCAL_SET); uleb(l); }
        void local_tee(std::uint8_t l) { u8(op::LOCAL_TEE); uleb(l); }
        void const_i32(std::uint32_t v) { u8(op::I32_CONST); sleb(static_cast<std::int32_t>(v)); }
        // i32.load/store with byte alignment (guest addresses may be unaligned)
        void load_u32(std::uint32_t off) { u8(op::I32_LOAD); uleb(0); uleb(off); }
        void load_u8(std::uint32_t off) { u8(op::I32_LOAD8_U); uleb(0); uleb(off); }
        void load_s8(std::uint32_t off) { u8(op::I32_LOAD8_S); uleb(0); uleb(off); }
        void load_u16(std::uint32_t off) { u8(op::I32_LOAD16_U); uleb(0); uleb(off); }
        void load_s16(std::uint32_t off) { u8(op::I32_LOAD16_S); uleb(0); uleb(off); }
        void store_u16(std::uint32_t off) { u8(op::I32_STORE16); uleb(0); uleb(off); }
        void store_u32(std::uint32_t off) { u8(op::I32_STORE); uleb(0); uleb(off); }
        void store_u8(std::uint32_t off) { u8(op::I32_STORE8); uleb(0); uleb(off); }
        void if_void() { u8(op::IF); u8(op::BLOCKTYPE_VOID); }
        void end() { u8(op::END); }
        void ret() { u8(op::RETURN); }
    };

    static std::uint32_t off_reg(unsigned r) {
        return static_cast<std::uint32_t>(offsetof(ARMul_State, Reg) + 4u * r);
    }

    // ---- per-idx classification ---------------------------------------------

    enum class alu_kind { AND, EOR, SUB, RSB, ADD, ADC, SBC, RSC, ORR, BIC, MOV, MVN, CMP, CMN, TST, TEQ, NONE };

    struct inst_class {
        alu_kind alu = alu_kind::NONE;
        bool cpy_no_flags = false; // CPY: MOV semantics, never touches flags
        bool is_ldst = false;
        bool ldst_load = false;
        bool ldst_byte = false;
        bool ldst_half = false;
        bool ldst_signed = false;
        bool is_ldm = false;
        bool is_stm = false;
        bool is_mul = false;
        bool mul_acc = false; // MLA
        bool is_bbl = false;
        bool is_bx = false;
        bool is_b2t = false;
        bool is_bct = false;
        bool is_bl1t = false;
        bool is_bl2t = false;
        int comp_size = -1;
    };

    static inst_class classify(unsigned idx) {
        const trans_indices &ti = indices;
        inst_class c;
        const int i = static_cast<int>(idx);

        if (i == ti.and_) { c.alu = alu_kind::AND; c.comp_size = sizeof(and_inst); }
        else if (i == ti.eor) { c.alu = alu_kind::EOR; c.comp_size = sizeof(eor_inst); }
        else if (i == ti.sub) { c.alu = alu_kind::SUB; c.comp_size = sizeof(sub_inst); }
        else if (i == ti.rsb) { c.alu = alu_kind::RSB; c.comp_size = sizeof(rsb_inst); }
        else if (i == ti.add) { c.alu = alu_kind::ADD; c.comp_size = sizeof(add_inst); }
        else if (i == ti.adc) { c.alu = alu_kind::ADC; c.comp_size = sizeof(adc_inst); }
        else if (i == ti.sbc) { c.alu = alu_kind::SBC; c.comp_size = sizeof(sbc_inst); }
        else if (i == ti.rsc) { c.alu = alu_kind::RSC; c.comp_size = sizeof(rsc_inst); }
        else if (i == ti.orr) { c.alu = alu_kind::ORR; c.comp_size = sizeof(orr_inst); }
        else if (i == ti.bic) { c.alu = alu_kind::BIC; c.comp_size = sizeof(bic_inst); }
        else if (i == ti.mov) { c.alu = alu_kind::MOV; c.comp_size = sizeof(mov_inst); }
        else if (i == ti.cpy) { c.alu = alu_kind::MOV; c.cpy_no_flags = true; c.comp_size = sizeof(mov_inst); }
        else if (i == ti.mvn) { c.alu = alu_kind::MVN; c.comp_size = sizeof(mvn_inst); }
        else if (i == ti.mul) { c.is_mul = true; c.comp_size = sizeof(mul_inst); }
        else if (i == ti.mla) { c.is_mul = true; c.mul_acc = true; c.comp_size = sizeof(mla_inst); }
        else if (i == ti.cmp) { c.alu = alu_kind::CMP; c.comp_size = sizeof(cmp_inst); }
        else if (i == ti.cmn) { c.alu = alu_kind::CMN; c.comp_size = sizeof(cmn_inst); }
        else if (i == ti.tst) { c.alu = alu_kind::TST; c.comp_size = sizeof(tst_inst); }
        else if (i == ti.teq) { c.alu = alu_kind::TEQ; c.comp_size = sizeof(teq_inst); }
        else if (i == ti.ldr || i == ti.ldrcond) { c.is_ldst = true; c.ldst_load = true; c.comp_size = sizeof(ldst_inst); }
        else if (i == ti.ldrb) { c.is_ldst = true; c.ldst_load = true; c.ldst_byte = true; c.comp_size = sizeof(ldst_inst); }
        else if (i == ti.str) { c.is_ldst = true; c.comp_size = sizeof(ldst_inst); }
        else if (i == ti.strb) { c.is_ldst = true; c.ldst_byte = true; c.comp_size = sizeof(ldst_inst); }
        else if (i == ti.ldrh) { c.is_ldst = true; c.ldst_load = true; c.ldst_half = true; c.comp_size = sizeof(ldst_inst); }
        else if (i == ti.strh) { c.is_ldst = true; c.ldst_half = true; c.comp_size = sizeof(ldst_inst); }
        else if (i == ti.ldrsh) { c.is_ldst = true; c.ldst_load = true; c.ldst_half = true; c.ldst_signed = true; c.comp_size = sizeof(ldst_inst); }
        else if (i == ti.ldrsb) { c.is_ldst = true; c.ldst_load = true; c.ldst_byte = true; c.ldst_signed = true; c.comp_size = sizeof(ldst_inst); }
        else if (i == ti.ldm1 || i == ti.ldm2 || i == ti.ldm3) { c.is_ldm = true; c.comp_size = sizeof(ldst_inst); }
        else if (i == ti.stm1 || i == ti.stm2) { c.is_stm = true; c.comp_size = sizeof(ldst_inst); }
        else if (i == ti.bbl) { c.is_bbl = true; c.comp_size = sizeof(bbl_inst); }
        else if (i == ti.bx) { c.is_bx = true; c.comp_size = sizeof(bx_inst); }
        else if (i == ti.b_2_thumb) { c.is_b2t = true; c.comp_size = sizeof(b_2_thumb); }
        else if (i == ti.b_cond_thumb) { c.is_bct = true; c.comp_size = sizeof(b_cond_thumb); }
        else if (i == ti.bl_1_thumb) { c.is_bl1t = true; c.comp_size = sizeof(bl_1_thumb); }
        else if (i == ti.bl_2_thumb) { c.is_bl2t = true; c.comp_size = sizeof(bl_2_thumb); }

        return c;
    }

    // The interpreter's INC_PC stepping: 8-byte aligned header+component.
    static std::size_t inst_step(int comp_size) {
        return ((sizeof(arm_inst) + static_cast<std::size_t>(comp_size) + 7) >> 3) << 3;
    }

    // ---- compiler ------------------------------------------------------------

    namespace {

        constexpr unsigned JIT_MAX_INSTS = 200;
        constexpr unsigned JIT_MIN_PREFIX = 4;

        class block_compiler {
        public:
            block_compiler(ARMul_State *cpu, std::uint32_t pc)
                : cpu_(cpu)
                , inst_size_(cpu->TFlag ? 2 : 4)
                , pc_(pc)
                , pc_start_(pc) {
                r12l1::tlb *t = cpu->jit_tlb();
                tlb_base_ = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(t));
                page_bits_ = static_cast<std::uint32_t>(t->page_bits);
                page_mask_ = static_cast<std::uint32_t>(t->page_mask);
            }

            std::int32_t compile(std::size_t trans_ptr);

        private:
            ARMul_State *cpu_;
            std::uint32_t inst_size_;
            std::uint32_t pc_;
            std::uint32_t pc_start_;
            std::uint32_t count_ = 0;
            unsigned idx_trace_[24];
            unsigned idx_trace_len_ = 0;
            std::uint32_t tlb_base_, page_bits_, page_mask_;
            wasm_writer w_;

            // ---- common fragments ----
            void emit_reg_read_dp(unsigned r) { // data-processing r15 rule
                if (r == 15) {
                    w_.const_i32(((pc_ & ~1u)) + 2 * inst_size_);
                } else {
                    w_.local_get(L_STATE);
                    w_.load_u32(off_reg(r));
                }
            }

            void emit_reg_write(unsigned r) { // value on stack -> Reg[r]; r != 15
                // (operand order: addr below value) -- callers push state first
                // via this helper instead:
                // [we emit: local.set T2; state; T2; store]
                w_.local_set(L_T2);
                w_.local_get(L_STATE);
                w_.local_get(L_T2);
                w_.store_u32(off_reg(r));
            }

            void emit_flag_writeback() {
                w_.local_get(L_STATE); w_.local_get(L_N); w_.store_u32(offsetof(ARMul_State, NFlag));
                w_.local_get(L_STATE); w_.local_get(L_Z); w_.store_u32(offsetof(ARMul_State, ZFlag));
                w_.local_get(L_STATE); w_.local_get(L_C); w_.store_u32(offsetof(ARMul_State, CFlag));
                w_.local_get(L_STATE); w_.local_get(L_V); w_.store_u32(offsetof(ARMul_State, VFlag));
            }

            void emit_exit_const_pc(std::uint32_t next_pc, std::uint32_t executed) {
                emit_flag_writeback();
                w_.local_get(L_STATE); w_.const_i32(next_pc); w_.store_u32(off_reg(15));
                w_.const_i32(executed);
                w_.ret();
            }

            void emit_exit_pc_from_local(std::uint8_t l, std::uint32_t executed) {
                emit_flag_writeback();
                w_.local_get(L_STATE); w_.local_get(l); w_.store_u32(off_reg(15));
                w_.const_i32(executed);
                w_.ret();
            }

            void emit_cond_value(unsigned cond) {
                switch (cond) {
                case ConditionCode::EQ: w_.local_get(L_Z); break;
                case ConditionCode::NE: w_.local_get(L_Z); w_.u8(op::I32_EQZ); break;
                case ConditionCode::CS: w_.local_get(L_C); break;
                case ConditionCode::CC: w_.local_get(L_C); w_.u8(op::I32_EQZ); break;
                case ConditionCode::MI: w_.local_get(L_N); break;
                case ConditionCode::PL: w_.local_get(L_N); w_.u8(op::I32_EQZ); break;
                case ConditionCode::VS: w_.local_get(L_V); break;
                case ConditionCode::VC: w_.local_get(L_V); w_.u8(op::I32_EQZ); break;
                case ConditionCode::HI:
                    w_.local_get(L_C); w_.local_get(L_Z); w_.u8(op::I32_EQZ); w_.u8(op::I32_AND);
                    break;
                case ConditionCode::LS:
                    w_.local_get(L_C); w_.u8(op::I32_EQZ); w_.local_get(L_Z); w_.u8(op::I32_OR);
                    break;
                case ConditionCode::GE: w_.local_get(L_N); w_.local_get(L_V); w_.u8(op::I32_EQ); break;
                case ConditionCode::LT: w_.local_get(L_N); w_.local_get(L_V); w_.u8(op::I32_NE); break;
                case ConditionCode::GT:
                    w_.local_get(L_Z); w_.u8(op::I32_EQZ);
                    w_.local_get(L_N); w_.local_get(L_V); w_.u8(op::I32_EQ);
                    w_.u8(op::I32_AND);
                    break;
                case ConditionCode::LE:
                    w_.local_get(L_Z);
                    w_.local_get(L_N); w_.local_get(L_V); w_.u8(op::I32_NE);
                    w_.u8(op::I32_OR);
                    break;
                default: w_.const_i32(1); break;
                }
            }

            // operand2 -> L_T1. When update_carry (logical S ops) also updates
            // L_C exactly like the matching DPO() evaluator. Returns false on
            // unsupported shift kinds (register-amount shifts, ROR/RRX).
            bool emit_operand2(unsigned shtop_idx, std::uint32_t sht, bool update_carry) {
                switch (shtop_idx) {
                case SHTOP_IMMEDIATE: {
                    const std::uint32_t imm8 = sht & 0xFF;
                    const std::uint32_t rot = (sht >> 8) & 0xF;
                    const std::uint32_t val = (rot == 0)
                        ? imm8
                        : ((imm8 >> (rot * 2)) | (imm8 << (32 - rot * 2)));
                    w_.const_i32(val);
                    w_.local_set(L_T1);
                    if (update_carry && rot != 0) {
                        w_.const_i32((val >> 31) & 1);
                        w_.local_set(L_C);
                    }
                    return true;
                }
                case SHTOP_REGISTER:
                    emit_reg_read_dp(sht & 0xF);
                    w_.local_set(L_T1);
                    return true; // carry unchanged
                case SHTOP_LSL_IMM: {
                    const unsigned imm = (sht >> 7) & 0x1F;
                    emit_reg_read_dp(sht & 0xF);
                    w_.local_set(L_T0);
                    if (imm == 0) {
                        w_.local_get(L_T0); w_.local_set(L_T1);
                    } else {
                        if (update_carry) {
                            w_.local_get(L_T0); w_.const_i32(32 - imm); w_.u8(op::I32_SHR_U);
                            w_.const_i32(1); w_.u8(op::I32_AND);
                            w_.local_set(L_C);
                        }
                        w_.local_get(L_T0); w_.const_i32(imm); w_.u8(op::I32_SHL);
                        w_.local_set(L_T1);
                    }
                    return true;
                }
                case SHTOP_LSR_IMM: {
                    const unsigned imm = (sht >> 7) & 0x1F;
                    emit_reg_read_dp(sht & 0xF);
                    w_.local_set(L_T0);
                    if (imm == 0) { // LSR #32
                        if (update_carry) {
                            w_.local_get(L_T0); w_.const_i32(31); w_.u8(op::I32_SHR_U);
                            w_.local_set(L_C);
                        }
                        w_.const_i32(0); w_.local_set(L_T1);
                    } else {
                        if (update_carry) {
                            w_.local_get(L_T0); w_.const_i32(imm - 1); w_.u8(op::I32_SHR_U);
                            w_.const_i32(1); w_.u8(op::I32_AND);
                            w_.local_set(L_C);
                        }
                        w_.local_get(L_T0); w_.const_i32(imm); w_.u8(op::I32_SHR_U);
                        w_.local_set(L_T1);
                    }
                    return true;
                }
                case SHTOP_ASR_IMM: {
                    const unsigned imm = (sht >> 7) & 0x1F;
                    emit_reg_read_dp(sht & 0xF);
                    w_.local_set(L_T0);
                    if (update_carry) {
                        const unsigned cbit = (imm == 0) ? 31 : (imm - 1);
                        w_.local_get(L_T0); w_.const_i32(cbit); w_.u8(op::I32_SHR_U);
                        w_.const_i32(1); w_.u8(op::I32_AND);
                        w_.local_set(L_C);
                    }
                    w_.local_get(L_T0); w_.const_i32(imm == 0 ? 31 : imm); w_.u8(op::I32_SHR_S);
                    w_.local_set(L_T1);
                    return true;
                }
                default:
                    return false;
                }
            }

            // AddWithCarry over (lhs_local [^invert], rhs_local [^invert]),
            // carry-in pushed by `push_cin`. Result -> L_T0; when set_flags,
            // C/V locals updated (N/Z done by caller from the result).
            //
            // Both operands are CAPTURED into scratch (T2/ADDR) before any
            // local is written: callers pass (T1, T0) for RSB/RSC, so staging
            // through T0/T1 would clobber an unread input.
            void emit_awc(std::uint8_t lhs, bool invert_lhs, std::uint8_t rhs, bool invert_rhs,
                void (block_compiler::*push_cin)(), bool set_flags) {
                // a -> T2, b -> ADDR (post-inversion values, as AddWithCarry sees them)
                w_.local_get(lhs);
                if (invert_lhs) { w_.const_i32(0xFFFFFFFFu); w_.u8(op::I32_XOR); }
                w_.local_set(L_T2);
                w_.local_get(rhs);
                if (invert_rhs) { w_.const_i32(0xFFFFFFFFu); w_.u8(op::I32_XOR); }
                w_.local_set(L_ADDR);

                // w64 = (u64)a + (u64)b + cin
                w_.local_get(L_T2); w_.u8(op::I64_EXTEND_I32_U);
                w_.local_get(L_ADDR); w_.u8(op::I64_EXTEND_I32_U);
                w_.u8(op::I64_ADD);
                (this->*push_cin)();
                w_.u8(op::I64_EXTEND_I32_U);
                w_.u8(op::I64_ADD);
                w_.local_set(L_W64);

                // result = wrap(w64) -> T0 (T2/ADDR still hold a/b for V)
                w_.local_get(L_W64); w_.u8(op::I32_WRAP_I64);
                w_.local_set(L_T0);

                if (set_flags) {
                    // C = (w64 >> 32) & 1
                    w_.local_get(L_W64); w_.u8(op::I64_CONST); w_.sleb(32); w_.u8(op::I64_SHR_U);
                    w_.u8(op::I32_WRAP_I64);
                    w_.const_i32(1); w_.u8(op::I32_AND);
                    w_.local_set(L_C);
                    // V = ((a ^ res) & (b ^ res)) >> 31
                    w_.local_get(L_T2); w_.local_get(L_T0); w_.u8(op::I32_XOR);
                    w_.local_get(L_ADDR); w_.local_get(L_T0); w_.u8(op::I32_XOR);
                    w_.u8(op::I32_AND);
                    w_.const_i32(31); w_.u8(op::I32_SHR_U);
                    w_.local_set(L_V);
                }
            }

            void push_cin_zero() { w_.const_i32(0); }
            void push_cin_one() { w_.const_i32(1); }
            void push_cin_c() { w_.local_get(L_C); }

            void emit_nz_from_t0() {
                w_.local_get(L_T0); w_.const_i32(31); w_.u8(op::I32_SHR_U); w_.local_set(L_N);
                w_.local_get(L_T0); w_.u8(op::I32_EQZ); w_.local_set(L_Z);
            }

            // TLB lookup of L_ADDR. perm_off: 0 = read_addr, 4 = write_addr.
            // On hit leaves the HOST address on the stack; on miss bails to
            // the interpreter at the current instruction.
            void emit_tlb_host_addr_or_bail(std::uint32_t perm_off) {
                // T2 = tlb_base + ((addr >> page_bits) & TLB_ENTRY_MASK) * 16
                w_.local_get(L_ADDR);
                w_.const_i32(page_bits_); w_.u8(op::I32_SHR_U);
                w_.const_i32(r12l1::TLB_ENTRY_MASK); w_.u8(op::I32_AND);
                w_.const_i32(4); w_.u8(op::I32_SHL);
                w_.const_i32(tlb_base_); w_.u8(op::I32_ADD);
                w_.local_set(L_T2);

                // if (entry.perm_addr != (addr & ~page_mask) || !entry.host_base)
                //     bail;   (empty entries have perm_addr == 0, which would
                // false-match guest page 0 without the host_base guard)
                w_.local_get(L_T2); w_.load_u32(perm_off);
                w_.local_get(L_ADDR); w_.const_i32(~page_mask_); w_.u8(op::I32_AND);
                w_.u8(op::I32_NE);
                w_.local_get(L_T2); w_.load_u32(offsetof(r12l1::tlb_entry, host_base));
                w_.u8(op::I32_EQZ);
                w_.u8(op::I32_OR);
                w_.if_void();
                emit_exit_const_pc(pc_, count_); // re-run this inst on the interpreter
                w_.end();

                // host = entry.host_base + (addr & page_mask)
                w_.local_get(L_T2); w_.load_u32(offsetof(r12l1::tlb_entry, host_base));
                w_.local_get(L_ADDR); w_.const_i32(page_mask_); w_.u8(op::I32_AND);
                w_.u8(op::I32_ADD);
            }

            void emit_reg_read_wa(unsigned r);
            void emit_ldm_word_load(int i, bool load_pc);
            void emit_stm_word_store(int i, unsigned Rn);
            void emit_flag_writeback_only_and_count(int extra);
            bool emit_alu(const inst_class &cls, arm_inst *base);
            bool emit_ldst(const inst_class &cls, arm_inst *base);
            bool emit_ldm_stm(const inst_class &cls, arm_inst *base, bool &terminates);
            void emit_mul(const inst_class &cls, arm_inst *base);
            void emit_bbl(arm_inst *base);
            void emit_bx(arm_inst *base);
            void emit_b2t(arm_inst *base);
            void emit_bct(arm_inst *base);
            void emit_bl1t(arm_inst *base);
            void emit_bl2t(arm_inst *base);
        };

        bool block_compiler::emit_alu(const inst_class &cls, arm_inst *base) {
            // All ALU creams share the leading layout {I, S, [Rn, Rd,]
            // shifter_operand, shtop_idx}; use the widest and pick fields per
            // kind.
            unsigned S = 0, Rn = 0, Rd = 0;
            std::uint32_t sht = 0, shtop = 0;

            switch (cls.alu) {
            case alu_kind::MOV:
            case alu_kind::MVN: {
                mov_inst *c = (mov_inst *)base->component;
                S = cls.cpy_no_flags ? 0 : c->S;
                Rd = c->Rd; sht = c->shifter_operand; shtop = c->shtop_idx;
                break;
            }
            case alu_kind::CMP:
            case alu_kind::CMN: {
                cmp_inst *c = (cmp_inst *)base->component;
                Rn = c->Rn; sht = c->shifter_operand; shtop = c->shtop_idx;
                S = 1;
                break;
            }
            case alu_kind::TEQ: {
                teq_inst *c = (teq_inst *)base->component;
                Rn = c->Rn; sht = c->shifter_operand; shtop = c->shtop_idx;
                S = 1;
                break;
            }
            case alu_kind::TST: {
                tst_inst *c = (tst_inst *)base->component;
                Rn = c->Rn; sht = c->shifter_operand; shtop = c->shtop_idx;
                S = 1;
                break;
            }
            default: {
                add_inst *c = (add_inst *)base->component;
                S = c->S; Rn = c->Rn; Rd = c->Rd; sht = c->shifter_operand; shtop = c->shtop_idx;
                break;
            }
            }

            const bool is_compare = (cls.alu == alu_kind::CMP) || (cls.alu == alu_kind::CMN)
                || (cls.alu == alu_kind::TST) || (cls.alu == alu_kind::TEQ);
            const bool is_logical = (cls.alu == alu_kind::AND) || (cls.alu == alu_kind::EOR)
                || (cls.alu == alu_kind::ORR) || (cls.alu == alu_kind::BIC)
                || (cls.alu == alu_kind::MOV) || (cls.alu == alu_kind::MVN)
                || (cls.alu == alu_kind::TST) || (cls.alu == alu_kind::TEQ);
            const bool writes_rd = !is_compare;

            // S=1 with Rd==15 has SPSR semantics — leave to the interpreter.
            if (writes_rd && S && Rd == 15) {
                return false;
            }

            const unsigned cond = base->cond;
            const bool guarded = (cond != ConditionCode::AL) && (cond != ConditionCode::NV);
            if (guarded) {
                emit_cond_value(cond);
                w_.if_void();
            }

            // operand2 -> T1 (with carry update when logical && S)
            if (!emit_operand2(shtop, sht, is_logical && S)) {
                // Roll back is handled by the caller (it snapshots the
                // writer); just report failure.
                return false;
            }

            const bool has_rn = !(cls.alu == alu_kind::MOV || cls.alu == alu_kind::MVN);
            if (has_rn) {
                emit_reg_read_dp(Rn);
                if (Rn == 15) {
                    // emit_reg_read_dp already applied (+2*size) with the DP
                    // rule; the handlers use plain Reg[15] + 2*size which is
                    // identical here because block PCs are aligned.
                }
                w_.local_set(L_T0);
            }

            switch (cls.alu) {
            case alu_kind::AND:
            case alu_kind::TST:
                w_.local_get(L_T0); w_.local_get(L_T1); w_.u8(op::I32_AND); w_.local_set(L_T0);
                break;
            case alu_kind::EOR:
            case alu_kind::TEQ:
                w_.local_get(L_T0); w_.local_get(L_T1); w_.u8(op::I32_XOR); w_.local_set(L_T0);
                break;
            case alu_kind::ORR:
                w_.local_get(L_T0); w_.local_get(L_T1); w_.u8(op::I32_OR); w_.local_set(L_T0);
                break;
            case alu_kind::BIC:
                w_.local_get(L_T1); w_.const_i32(0xFFFFFFFFu); w_.u8(op::I32_XOR);
                w_.local_get(L_T0); w_.u8(op::I32_AND); w_.local_set(L_T0);
                break;
            case alu_kind::MOV:
                w_.local_get(L_T1); w_.local_set(L_T0);
                break;
            case alu_kind::MVN:
                w_.local_get(L_T1); w_.const_i32(0xFFFFFFFFu); w_.u8(op::I32_XOR); w_.local_set(L_T0);
                break;
            case alu_kind::ADD:
            case alu_kind::CMN:
                if (S) emit_awc(L_T0, false, L_T1, false, &block_compiler::push_cin_zero, true);
                else { w_.local_get(L_T0); w_.local_get(L_T1); w_.u8(op::I32_ADD); w_.local_set(L_T0); }
                break;
            case alu_kind::SUB:
            case alu_kind::CMP:
                if (S) emit_awc(L_T0, false, L_T1, true, &block_compiler::push_cin_one, true);
                else { w_.local_get(L_T0); w_.local_get(L_T1); w_.u8(op::I32_SUB); w_.local_set(L_T0); }
                break;
            case alu_kind::RSB:
                if (S) emit_awc(L_T1, false, L_T0, true, &block_compiler::push_cin_one, true);
                else { w_.local_get(L_T1); w_.local_get(L_T0); w_.u8(op::I32_SUB); w_.local_set(L_T0); }
                break;
            case alu_kind::ADC:
                if (S) emit_awc(L_T0, false, L_T1, false, &block_compiler::push_cin_c, true);
                else {
                    w_.local_get(L_T0); w_.local_get(L_T1); w_.u8(op::I32_ADD);
                    w_.local_get(L_C); w_.u8(op::I32_ADD); w_.local_set(L_T0);
                }
                break;
            case alu_kind::SBC:
                if (S) emit_awc(L_T0, false, L_T1, true, &block_compiler::push_cin_c, true);
                else {
                    w_.local_get(L_T0); w_.local_get(L_T1); w_.u8(op::I32_SUB);
                    w_.const_i32(1); w_.u8(op::I32_SUB);
                    w_.local_get(L_C); w_.u8(op::I32_ADD); w_.local_set(L_T0);
                }
                break;
            case alu_kind::RSC:
                if (S) emit_awc(L_T1, false, L_T0, true, &block_compiler::push_cin_c, true);
                else {
                    w_.local_get(L_T1); w_.local_get(L_T0); w_.u8(op::I32_SUB);
                    w_.const_i32(1); w_.u8(op::I32_SUB);
                    w_.local_get(L_C); w_.u8(op::I32_ADD); w_.local_set(L_T0);
                }
                break;
            default:
                return false;
            }

            if (S) {
                emit_nz_from_t0();
            }

            if (writes_rd) {
                if (Rd == 15) {
                    // S==0 checked above. End the block through the new PC;
                    // DISPATCH re-masks per TFlag like after any handler.
                    emit_exit_pc_from_local(L_T0, count_ + 1);
                    if (guarded) {
                        w_.end();
                        // cond failed: fall through to the next instruction.
                    } else {
                        // Unconditional: code after this is unreachable; the
                        // caller treats it as a terminator.
                    }
                    return true;
                }
                w_.local_get(L_T0);
                emit_reg_write(Rd);
            }

            if (guarded) {
                w_.end();
            }
            return true;
        }

        // Push the value of Reg[r] with the word-aligned-pc rule used by the
        // addressing helpers (CHECK_READ_REG15_WA).
        void block_compiler::emit_reg_read_wa(unsigned r) {
            if (r == 15) {
                w_.const_i32((pc_ & ~3u) + 2 * inst_size_);
            } else {
                w_.local_get(L_STATE);
                w_.load_u32(off_reg(r));
            }
        }

        bool block_compiler::emit_ldst(const inst_class &cls, arm_inst *base) {
            ldst_inst *c = (ldst_inst *)base->component;

            const std::uint32_t inst = c->inst;
            const unsigned Rn = (inst >> 16) & 0xF;
            const unsigned Rd = (inst >> 12) & 0xF;
            const bool up = (inst >> 23) & 1;

            // Word offsets (LDR/STR/LDRB/STRB family) vs the miscellaneous
            // (halfword/signed) encoding, which splits its 8-bit immediate.
            std::uint32_t imm_off = 0;
            unsigned Rm = inst & 0xF;
            bool writeback_pre = false, writeback_post = false, reg_offset = false;

            switch (c->addr_mode) {
            case ADDRMODE_LNSW_IMM_OFFSET:
                imm_off = inst & 0xFFF;
                break;
            case ADDRMODE_LNSW_REG_OFFSET:
                reg_offset = true;
                break;
            case ADDRMODE_LNSW_IMM_PRE:
                imm_off = inst & 0xFFF;
                writeback_pre = true;
                break;
            case ADDRMODE_LNSW_REG_PRE:
                reg_offset = true;
                writeback_pre = true;
                break;
            case ADDRMODE_LNSW_IMM_POST:
                imm_off = inst & 0xFFF;
                writeback_post = true;
                break;
            case ADDRMODE_LNSW_REG_POST:
                reg_offset = true;
                writeback_post = true;
                break;
            case ADDRMODE_MLNS_IMM_OFFSET:
                imm_off = ((inst >> 4) & 0xF0) | (inst & 0xF);
                break;
            case ADDRMODE_MLNS_REG_OFFSET:
                reg_offset = true;
                break;
            default:
                return false; // scaled-register & misc pre/post: interpreter
            }

            // PC destinations/sources switch modes / need extra rules, and a
            // PC base together with writeback makes no sense to compile.
            if (Rd == 15 || (reg_offset && Rm == 15)) {
                return false;
            }
            if ((writeback_pre || writeback_post) && Rn == 15) {
                return false;
            }

            const unsigned cond = base->cond;
            const bool guarded = (cond != ConditionCode::AL) && (cond != ConditionCode::NV);
            if (guarded) {
                emit_cond_value(cond);
                w_.if_void();
            }

            // ADDR = access address (post-indexed accesses use the original Rn)
            if (writeback_post) {
                emit_reg_read_wa(Rn);
                w_.local_set(L_ADDR);
            } else {
                emit_reg_read_wa(Rn);
                if (reg_offset) {
                    emit_reg_read_wa(Rm);
                    w_.u8(up ? op::I32_ADD : op::I32_SUB);
                } else if (imm_off) {
                    w_.const_i32(imm_off);
                    w_.u8(up ? op::I32_ADD : op::I32_SUB);
                }
                w_.local_set(L_ADDR);
            }

            // Resolve the host pointer FIRST (bails happen before any side
            // effect), capture it in T0, then do writeback + the access.
            emit_tlb_host_addr_or_bail(cls.ldst_load
                ? static_cast<std::uint32_t>(offsetof(r12l1::tlb_entry, read_addr))
                : static_cast<std::uint32_t>(offsetof(r12l1::tlb_entry, write_addr)));
            w_.local_set(L_T0); // host address

            if (writeback_pre) {
                // Rn = computed address (mirrors LnSWoUB *PreIndexed)
                w_.local_get(L_STATE);
                w_.local_get(L_ADDR);
                w_.store_u32(off_reg(Rn));
            } else if (writeback_post) {
                // Rn = Rn +/- offset (mirrors LnSWoUB *PostIndexed; the
                // access itself used the original Rn already in L_ADDR)
                w_.local_get(L_STATE);
                w_.local_get(L_ADDR);
                if (reg_offset) {
                    emit_reg_read_wa(Rm);
                    w_.u8(up ? op::I32_ADD : op::I32_SUB);
                } else if (imm_off) {
                    w_.const_i32(imm_off);
                    w_.u8(up ? op::I32_ADD : op::I32_SUB);
                }
                w_.store_u32(off_reg(Rn));
            }

            if (cls.ldst_load) {
                w_.local_get(L_T0);
                if (cls.ldst_byte) {
                    if (cls.ldst_signed) w_.load_s8(0); else w_.load_u8(0);
                } else if (cls.ldst_half) {
                    if (cls.ldst_signed) w_.load_s16(0); else w_.load_u16(0);
                } else {
                    w_.load_u32(0);
                }
                emit_reg_write(Rd);
            } else {
                w_.local_get(L_T0);
                w_.local_get(L_STATE);
                w_.load_u32(off_reg(Rd)); // Rd != 15 enforced above
                if (cls.ldst_byte) w_.store_u8(0);
                else if (cls.ldst_half) w_.store_u16(0);
                else w_.store_u32(0);
            }

            if (guarded) {
                w_.end();
            }
            return true;
        }

        // LDM/STM, plain-register form only (no S/user-bank bit). Strategy:
        // pre-resolve the host pointers for the first and last byte of the
        // whole range BEFORE any side effect (bail-idempotent), then perform
        // writeback and the per-word accesses through the captured bases —
        // immune to concurrent TLB invalidation mid-op.
        bool block_compiler::emit_ldm_stm(const inst_class &cls, arm_inst *base, bool &terminates) {
            ldst_inst *c = (ldst_inst *)base->component;
            const std::uint32_t inst = c->inst;

            if (inst & (1u << 22)) {
                return false; // user-bank / SPSR forms: interpreter
            }

            const unsigned Rn = (inst >> 16) & 0xF;
            const std::uint32_t list = inst & 0xFFFF;
            if (list == 0 || Rn == 15) {
                return false;
            }
            const bool load_pc = cls.is_ldm && (list & (1u << 15));
            if (cls.is_stm && (list & (1u << 15))) {
                // STM with PC stores pc+8: supported via constant below.
            }

            int count = 0;
            for (int i = 0; i < 16; i++) {
                if (list & (1u << i)) count++;
            }
            const std::uint32_t span = static_cast<std::uint32_t>(count) * 4;
            const bool writeback = (inst >> 21) & 1;

            const unsigned cond = base->cond;
            const bool guarded = (cond != ConditionCode::AL) && (cond != ConditionCode::NV);
            if (guarded) {
                emit_cond_value(cond);
                w_.if_void();
            }

            // start address per LdnStM mode
            // IA: Rn        IB: Rn+4       DA: Rn-span+4     DB: Rn-span
            std::int32_t start_delta = 0;
            std::int32_t wb_delta = 0;
            switch (c->addr_mode) {
            case ADDRMODE_LDNSTM_INC_AFTER: start_delta = 0; wb_delta = static_cast<std::int32_t>(span); break;
            case ADDRMODE_LDNSTM_INC_BEFORE: start_delta = 4; wb_delta = static_cast<std::int32_t>(span); break;
            case ADDRMODE_LDNSTM_DEC_AFTER: start_delta = -static_cast<std::int32_t>(span) + 4; wb_delta = -static_cast<std::int32_t>(span); break;
            case ADDRMODE_LDNSTM_DEC_BEFORE: start_delta = -static_cast<std::int32_t>(span); wb_delta = -static_cast<std::int32_t>(span); break;
            default:
                if (guarded) { /* caller rolls back via snapshot */ }
                return false;
            }

            const std::uint32_t perm_off = cls.is_ldm
                ? static_cast<std::uint32_t>(offsetof(r12l1::tlb_entry, read_addr))
                : static_cast<std::uint32_t>(offsetof(r12l1::tlb_entry, write_addr));

            // ADDR = start address; T3 = original Rn (STM stores the
            // pre-writeback value for Rn per the interpreter's old_RN rule;
            // T3 because every TLB resolve clobbers T2).
            w_.local_get(L_STATE); w_.load_u32(off_reg(Rn)); w_.local_set(L_T3);
            w_.local_get(L_T3);
            if (start_delta) { w_.const_i32(static_cast<std::uint32_t>(start_delta)); w_.u8(op::I32_ADD); }
            w_.local_set(L_ADDR);

            // hostA = resolve(start) (bails if unmapped)
            emit_tlb_host_addr_or_bail(perm_off);
            w_.local_set(L_T0);
            // keep the guest start address in T1 for per-word page checks
            w_.local_get(L_ADDR); w_.local_set(L_T1);
            w_.local_get(L_ADDR); w_.const_i32(span - 1); w_.u8(op::I32_ADD); w_.local_set(L_ADDR);
            emit_tlb_host_addr_or_bail(perm_off);
            // hostB currently points at start+span-1; rebase to the second page start
            // hostB_base = hostB - ((start+span-1) & page_mask)
            w_.local_get(L_ADDR); w_.const_i32(page_mask_); w_.u8(op::I32_AND);
            w_.u8(op::I32_SUB);
            // (stack: hostB_pagebase)  boundary = (start+span-1) & ~page_mask
            w_.local_set(L_ADDR); // ADDR = hostB page base (guest start addr no longer needed in ADDR)

            // From here: T1 = guest start, T0 = hostA, ADDR = hostB page base.

            // writeback BEFORE the accesses (mirrors get_addr-then-access; safe
            // now that both pages are resolved and captured).
            if (writeback) {
                w_.local_get(L_STATE);
                w_.local_get(L_T3);
                w_.const_i32(static_cast<std::uint32_t>(wb_delta)); w_.u8(op::I32_ADD);
                w_.store_u32(off_reg(Rn));
            }

            std::uint32_t word_index = 0;
            for (int i = 0; i < 16; i++) {
                if (!(list & (1u << i))) continue;
                const std::uint32_t off = word_index * 4;
                word_index++;

                // host = (same page as start ? hostA : hostB_base + in-page offset)
                // guest addr g = start + off; same-page iff (g & ~mask) == (start & ~mask)
                // Emit: g = T1 + off; if ((g ^ T1) & ~mask) -> cross -> hostB_base + (g & mask) else hostA + off
                // Simpler branch-free: cross = ((T1 & mask) + off) >= page_size
                // host = cross ? (ADDR + (g & mask)) : (T0 + off)
                w_.local_get(L_T1); w_.const_i32(page_mask_); w_.u8(op::I32_AND);
                w_.const_i32(off); w_.u8(op::I32_ADD);
                w_.const_i32(page_mask_ + 1); w_.u8(op::I32_LT_U);
                w_.if_void();
                {
                    if (cls.is_ldm) {
                        w_.local_get(L_T0); w_.const_i32(off); w_.u8(op::I32_ADD);
                        emit_ldm_word_load(i, load_pc);
                    } else {
                        w_.local_get(L_T0); w_.const_i32(off); w_.u8(op::I32_ADD);
                        emit_stm_word_store(i, Rn);
                    }
                }
                w_.u8(op::ELSE_OP);
                {
                    // host = hostB_base + ((T1 + off) & mask)
                    w_.local_get(L_ADDR);
                    w_.local_get(L_T1); w_.const_i32(off); w_.u8(op::I32_ADD);
                    w_.const_i32(page_mask_); w_.u8(op::I32_AND);
                    w_.u8(op::I32_ADD);
                    if (cls.is_ldm) {
                        emit_ldm_word_load(i, load_pc);
                    } else {
                        emit_stm_word_store(i, Rn);
                    }
                }
                w_.end();
            }

            if (load_pc) {
                // Reg15/TFlag were written by the PC word; end the block.
                emit_flag_writeback_only_and_count(count);
                terminates = !guarded;
                if (guarded) {
                    w_.end();
                    // cond failed: fall through sequentially.
                }
                return true;
            }

            if (guarded) {
                w_.end();
            }
            return true;
        }

        // helper: stack has host addr; load word into Reg[i] (or PC+TFlag)
        void block_compiler::emit_ldm_word_load(int i, bool /*load_pc*/) {
            if (i == 15) {
                // ret = mem; TFlag = ret & 1; Reg15 = ret & ~1
                w_.load_u32(0);
                w_.local_set(L_T2);
                w_.local_get(L_STATE);
                w_.local_get(L_T2); w_.const_i32(1); w_.u8(op::I32_AND);
                w_.store_u32(offsetof(ARMul_State, TFlag));
                w_.local_get(L_STATE);
                w_.local_get(L_T2); w_.const_i32(~1u); w_.u8(op::I32_AND);
                w_.store_u32(off_reg(15));
            } else {
                w_.load_u32(0);
                w_.local_set(L_T2);
                w_.local_get(L_STATE);
                w_.local_get(L_T2);
                w_.store_u32(off_reg(static_cast<unsigned>(i)));
            }
        }

        // helper: stack has host addr; store Reg[i] (original Rn from T2 is
        // NOT used here: Rn's original value lives in... see caller note)
        void block_compiler::emit_stm_word_store(int i, unsigned Rn) {
            if (i == 15) {
                w_.const_i32(pc_ + 8);
            } else if (static_cast<unsigned>(i) == Rn) {
                // interpreter stores old_RN: original Rn preserved in T3
                w_.local_get(L_T3);
            } else {
                w_.local_get(L_STATE);
                w_.load_u32(off_reg(static_cast<unsigned>(i)));
            }
            w_.store_u32(0);
        }

        // flags writeback + return count (used by LDM-with-PC terminator)
        void block_compiler::emit_flag_writeback_only_and_count(int extra) {
            emit_flag_writeback();
            w_.const_i32(count_ + static_cast<std::uint32_t>(extra >= 0 ? 1 : 1));
            w_.ret();
        }

        void block_compiler::emit_mul(const inst_class &cls, arm_inst *base) {
            unsigned S, Rd, Rs, Rm, Rn = 0;
            if (cls.mul_acc) {
                mla_inst *c = (mla_inst *)base->component;
                S = c->S; Rd = c->Rd; Rs = c->Rs; Rm = c->Rm; Rn = c->Rn;
            } else {
                mul_inst *c = (mul_inst *)base->component;
                S = c->S; Rd = c->Rd; Rs = c->Rs; Rm = c->Rm;
            }

            const unsigned cond = base->cond;
            const bool guarded = (cond != ConditionCode::AL) && (cond != ConditionCode::NV);
            if (guarded) {
                emit_cond_value(cond);
                w_.if_void();
            }

            emit_reg_read_dp(Rm); // r15 -> pc-ish constant; harmless for UNPREDICTABLE encodings
            emit_reg_read_dp(Rs);
            w_.u8(op::I32_MUL);
            if (cls.mul_acc) {
                emit_reg_read_dp(Rn);
                w_.u8(op::I32_ADD);
            }
            w_.local_set(L_T0);
            if (S) {
                emit_nz_from_t0();
            }
            w_.local_get(L_T0);
            emit_reg_write(Rd);

            if (guarded) {
                w_.end();
            }
        }

        void block_compiler::emit_bl1t(arm_inst *base) {
            bl_1_thumb *c = (bl_1_thumb *)base->component;
            // LR = pc + 4 + imm; continue to the next instruction.
            w_.local_get(L_STATE);
            w_.const_i32(pc_ + 4 + c->imm);
            w_.store_u32(off_reg(14));
        }

        void block_compiler::emit_bl2t(arm_inst *base) {
            bl_2_thumb *c = (bl_2_thumb *)base->component;
            // tmp = (pc + 2) | 1; Reg15 = LR + imm; LR = tmp
            w_.local_get(L_STATE); w_.load_u32(off_reg(14));
            w_.const_i32(c->imm); w_.u8(op::I32_ADD);
            w_.local_set(L_T0);
            w_.local_get(L_STATE);
            w_.const_i32((pc_ + 2) | 1);
            w_.store_u32(off_reg(14));
            emit_exit_pc_from_local(L_T0, count_ + 1);
        }

        void block_compiler::emit_bbl(arm_inst *base) {
            bbl_inst *c = (bbl_inst *)base->component;
            const unsigned cond = base->cond;
            const bool guarded = (cond != ConditionCode::AL) && (cond != ConditionCode::NV);
            const std::uint32_t target = pc_ + 8 + static_cast<std::uint32_t>(c->signed_immed_24);

            if (guarded) {
                emit_cond_value(cond);
                w_.if_void();
            }
            if (c->L) {
                w_.local_get(L_STATE);
                w_.const_i32(pc_ + 4);
                w_.store_u32(off_reg(14));
            }
            emit_exit_const_pc(target, count_ + 1);
            if (guarded) {
                w_.end();
                emit_exit_const_pc(pc_ + inst_size_, count_ + 1);
            }
        }

        void block_compiler::emit_bx(arm_inst *base) {
            bx_inst *c = (bx_inst *)base->component;
            const unsigned cond = base->cond;
            const bool guarded = (cond != ConditionCode::AL) && (cond != ConditionCode::NV);

            if (guarded) {
                emit_cond_value(cond);
                w_.if_void();
            }
            // addr = Rm (r15: +2*size, no masking — mirrors the handler's RM)
            if ((c->Rm & 0xF) == 15) {
                w_.const_i32(pc_ + 2 * inst_size_);
            } else {
                w_.local_get(L_STATE);
                w_.load_u32(off_reg(c->Rm & 0xF));
            }
            w_.local_set(L_ADDR);

            // TFlag = addr & 1
            w_.local_get(L_STATE);
            w_.local_get(L_ADDR); w_.const_i32(1); w_.u8(op::I32_AND);
            w_.store_u32(offsetof(ARMul_State, TFlag));

            // Reg15 = addr & ~1
            w_.local_get(L_ADDR); w_.const_i32(~1u); w_.u8(op::I32_AND);
            w_.local_set(L_T0);
            emit_exit_pc_from_local(L_T0, count_ + 1);

            if (guarded) {
                w_.end();
                emit_exit_const_pc(pc_ + inst_size_, count_ + 1);
            }
        }

        void block_compiler::emit_b2t(arm_inst *base) {
            b_2_thumb *c = (b_2_thumb *)base->component;
            emit_exit_const_pc(pc_ + 4 + c->imm, count_ + 1);
        }

        void block_compiler::emit_bct(arm_inst *base) {
            b_cond_thumb *c = (b_cond_thumb *)base->component;
            emit_cond_value(c->cond);
            w_.if_void();
            emit_exit_const_pc(pc_ + 4 + c->imm, count_ + 1);
            w_.end();
            emit_exit_const_pc(pc_ + 2, count_ + 1);
        }

        std::int32_t block_compiler::compile(std::size_t trans_ptr) {
            // Function body prologue: load flags into locals.
            w_.local_get(L_STATE); w_.load_u32(offsetof(ARMul_State, NFlag)); w_.local_set(L_N);
            w_.local_get(L_STATE); w_.load_u32(offsetof(ARMul_State, ZFlag)); w_.local_set(L_Z);
            w_.local_get(L_STATE); w_.load_u32(offsetof(ARMul_State, CFlag)); w_.local_set(L_C);
            w_.local_get(L_STATE); w_.load_u32(offsetof(ARMul_State, VFlag)); w_.local_set(L_V);

            bool terminated = false;
            std::size_t ptr = trans_ptr;

            for (unsigned n = 0; n < JIT_MAX_INSTS; n++) {
                arm_inst *base = reinterpret_cast<arm_inst *>(&cpu_->trans_cache_buf[ptr]);
                const inst_class cls = classify(base->idx);

                if (cls.comp_size < 0) {
                    if (base->idx < 224) stat_blocker_hist[base->idx]++;
                    break; // unsupported: end the block before this inst
                }

                const std::size_t snapshot = w_.b.size();
                bool ok = true;
                bool inst_terminates = false;

                if (cls.is_ldm || cls.is_stm) {
                    bool term = false;
                    ok = emit_ldm_stm(cls, base, term);
                    inst_terminates = term;
                } else if (cls.is_mul) {
                    emit_mul(cls, base);
                } else if (cls.is_bl1t) {
                    emit_bl1t(base);
                } else if (cls.is_bl2t) {
                    emit_bl2t(base);
                    inst_terminates = true;
                } else if (cls.alu != alu_kind::NONE) {
                    ok = emit_alu(cls, base);
                    // Unconditional ALU writing r15 acts as a terminator.
                    if (ok && base->cond == ConditionCode::AL) {
                        const bool writes15 = [&]() {
                            switch (cls.alu) {
                            case alu_kind::MOV:
                            case alu_kind::MVN:
                                return ((mov_inst *)base->component)->Rd == 15;
                            case alu_kind::CMP:
                            case alu_kind::CMN:
                            case alu_kind::TST:
                            case alu_kind::TEQ:
                                return false;
                            default:
                                return ((add_inst *)base->component)->Rd == 15;
                            }
                        }();
                        inst_terminates = writes15;
                    }
                } else if (cls.is_ldst) {
                    ok = emit_ldst(cls, base);
                } else if (cls.is_bbl) {
                    if (inst_size_ != 4) { ok = false; } else { emit_bbl(base); inst_terminates = true; }
                } else if (cls.is_bx) {
                    emit_bx(base);
                    inst_terminates = true;
                } else if (cls.is_b2t) {
                    emit_b2t(base);
                    inst_terminates = true;
                } else if (cls.is_bct) {
                    emit_bct(base);
                    inst_terminates = true;
                } else {
                    ok = false;
                }

                if (!ok) {
                    if (base->idx < 224) stat_blocker_hist[base->idx]++;
                    w_.b.resize(snapshot);
                    break;
                }

                if (idx_trace_len_ < 24) idx_trace_[idx_trace_len_++] = base->idx;
                count_++;
                if (inst_terminates) {
                    terminated = true;
                    break;
                }

                pc_ += inst_size_;
                ptr += inst_step(cls.comp_size);

                // A non-terminator that still ends the translated block
                // (END_OF_PAGE etc.): stop cleanly at the next pc.
                if (base->br != TransExtData::NON_BRANCH) {
                    break;
                }
            }

            if (count_ < JIT_MIN_PREFIX) {
                stat_rejected++;
                return -2;
            }

            if (!terminated) {
                // Fell off the compiled prefix: hand the rest to the
                // interpreter at the next instruction.
                emit_exit_const_pc(pc_, count_);
            }

            // ---- wrap the body into a module --------------------------------
            wasm_writer m;
            const std::uint8_t header[8] = { 0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00 };
            m.b.insert(m.b.end(), header, header + 8);

            // type section: (i32) -> (i32)
            {
                wasm_writer s;
                s.uleb(1);
                s.u8(0x60); s.uleb(1); s.u8(0x7F); s.uleb(1); s.u8(0x7F);
                m.u8(0x01); m.uleb(s.b.size()); m.b.insert(m.b.end(), s.b.begin(), s.b.end());
            }
            // import section: env.memory (shared, max 65536 pages)
            {
                wasm_writer s;
                s.uleb(1);
                s.uleb(3); s.b.insert(s.b.end(), { 'e', 'n', 'v' });
                s.uleb(6); s.b.insert(s.b.end(), { 'm', 'e', 'm', 'o', 'r', 'y' });
                s.u8(0x02); // memory import
                s.u8(0x03); // limits: min+max, shared
                s.uleb(0);
                s.uleb(65536);
                m.u8(0x02); m.uleb(s.b.size()); m.b.insert(m.b.end(), s.b.begin(), s.b.end());
            }
            // function section
            {
                wasm_writer s;
                s.uleb(1); s.uleb(0);
                m.u8(0x03); m.uleb(s.b.size()); m.b.insert(m.b.end(), s.b.begin(), s.b.end());
            }
            // export section: "f"
            {
                wasm_writer s;
                s.uleb(1);
                s.uleb(1); s.u8('f');
                s.u8(0x00); s.uleb(0);
                m.u8(0x07); m.uleb(s.b.size()); m.b.insert(m.b.end(), s.b.begin(), s.b.end());
            }
            // code section
            {
                wasm_writer body;
                body.uleb(2); // two local groups
                body.uleb(9); body.u8(0x7F); // 9 x i32 (L_N..L_T3)
                body.uleb(1); body.u8(0x7E); // 1 x i64 (L_W64)
                body.b.insert(body.b.end(), w_.b.begin(), w_.b.end());
                // Safety net: validator requires the function to end with a
                // value or unreachable flow; emit a default exit.
                body.const_i32(count_);
                body.u8(op::END);

                wasm_writer s;
                s.uleb(1);
                s.uleb(body.b.size());
                s.b.insert(s.b.end(), body.b.begin(), body.b.end());
                m.u8(0x0A); m.uleb(s.b.size()); m.b.insert(m.b.end(), s.b.begin(), s.b.end());
            }

            std::int32_t reuse = -1;
            if (!g_free_indices.empty()) {
                reuse = g_free_indices.back();
                g_free_indices.pop_back();
            }

            const int idx = dyncom_wasmjit_install(m.b.data(), static_cast<int>(m.b.size()), reuse);
            if (idx < 0) {
                if (reuse >= 0) {
                    g_free_indices.push_back(reuse);
                }
                stat_rejected++;
                return -2;
            }

            stat_compiled++;
            if (stat_compiled <= 64 || (stat_compiled & 1023) == 1) {
                char idxs[160];
                int w = 0;
                for (unsigned k = 0; k < idx_trace_len_ && w < 140; k++) {
                    w += std::snprintf(idxs + w, sizeof(idxs) - w, "%u,", idx_trace_[k]);
                }
                std::printf("[jit] block #%u pc=0x%08X insts=%u thumb=%d idx=[%s]\n",
                    stat_compiled, pc_start_, count_, (inst_size_ == 2) ? 1 : 0, idxs);
            }
            // Bisect aid: dump the last allowed module so it can be replayed
            // offline (node) against a reference implementation.
            if (static_cast<int>(stat_compiled) == compile_limit) {
                std::printf("[jit] offsets Reg=%u N=%u Z=%u C=%u V=%u TF=%u\n",
                    (unsigned)offsetof(ARMul_State, Reg), (unsigned)offsetof(ARMul_State, NFlag),
                    (unsigned)offsetof(ARMul_State, ZFlag), (unsigned)offsetof(ARMul_State, CFlag),
                    (unsigned)offsetof(ARMul_State, VFlag), (unsigned)offsetof(ARMul_State, TFlag));
                std::printf("[jit] module #%u hex=", stat_compiled);
                for (std::uint8_t byte : m.b) {
                    std::printf("%02x", byte);
                }
                std::printf("\n");
            }
            return idx;
        }
    }

    std::int32_t try_compile(ARMul_State *cpu, std::uint32_t pc, std::size_t trans_ptr) {
        // Paranoia: if the translate-table scan failed, never compile.
        if (indices.cmp < 0 || indices.mov < 0 || indices.ldr < 0 || indices.bbl < 0) {
            return -2;
        }
        if (static_cast<int>(stat_compiled) >= compile_limit) {
            return -2;
        }
        block_compiler c(cpu, pc);
        return c.compile(trans_ptr);
    }
}

#endif // __EMSCRIPTEN__
