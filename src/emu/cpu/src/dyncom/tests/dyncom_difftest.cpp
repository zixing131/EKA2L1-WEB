/*
 * Copyright (c) 2026 EKA2L1 Team.
 *
 * Differential test harness for the dyncom interpreter.
 *
 * Standalone host tool (no Catch2 dependency) that runs randomized ARM
 * instruction cases through the dyncom interpreter and checks the guest-visible
 * result against:
 *   (a) an independent golden ALU model (data-processing semantics + the ARM
 *       barrel-shifter carry-out rules) -- the fixed reference that the
 *       semantic optimizations (shifter specialization, lazy flags, inline
 *       AddWithCarry) must keep matching, and
 *   (b) a second dyncom instance (self-A/B) -- so a dispatch-shape optimization
 *       gated behind a flag can be proven behaviour-preserving by toggling it
 *       on one side.
 *
 * A negative-control case deliberately perturbs the result to prove the
 * comparator actually detects a divergence (a harness that can never fail is
 * useless).
 *
 * Build:  cmake -DEKA2L1_BUILD_DYNCOM_DIFFTEST=ON -DEKA2L1_CPU_DYNCOM_ONLY=ON ...
 * Run:    scripts/cpu_difftest.sh   (exits non-zero on the first divergence)
 */

#include <cpu/dyncom/arm_dyncom.h>
#include <cpu/12l1r/exclusive_monitor.h>

#include <common/types.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

using namespace eka2l1;
using namespace eka2l1::arm;

namespace {

constexpr std::uint32_t MEM_SIZE = 0x10000; // 64 KB flat memory
constexpr std::size_t PAGE_BITS = 12;
constexpr std::uint32_t PAGE_MASK = (1u << PAGE_BITS) - 1;

// ---------------------------------------------------------------------------
// dyncom core over a flat memory buffer
// ---------------------------------------------------------------------------
struct diff_env {
    std::vector<std::uint8_t> mem;
    r12l1::exclusive_monitor monitor;
    diff_env()
        : mem(MEM_SIZE, 0)
        , monitor(1) {
    }
};

std::unique_ptr<dyncom_core> make_core(diff_env &env) {
    auto core = std::make_unique<dyncom_core>(&env.monitor, PAGE_BITS);
    std::uint8_t *base = env.mem.data();
    dyncom_core *cp = core.get();

    auto seed_tlb = [cp, base](std::uint32_t addr) {
        const std::uint32_t page = addr & ~PAGE_MASK;
        cp->set_tlb_page(page, base + page, prot_read_write);
    };

    core->read_code = [base](const address a, std::uint32_t *r) -> bool {
        if (a + 4 > MEM_SIZE)
            return false;
        std::memcpy(r, base + a, 4);
        return true;
    };
    core->read_8bit = [base, seed_tlb](const address a, std::uint8_t *r) -> bool {
        if (a >= MEM_SIZE)
            return false;
        *r = base[a];
        seed_tlb(a);
        return true;
    };
    core->read_16bit = [base, seed_tlb](const address a, std::uint16_t *r) -> bool {
        if (a + 2 > MEM_SIZE)
            return false;
        std::memcpy(r, base + a, 2);
        seed_tlb(a);
        return true;
    };
    core->read_32bit = [base, seed_tlb](const address a, std::uint32_t *r) -> bool {
        if (a + 4 > MEM_SIZE)
            return false;
        std::memcpy(r, base + a, 4);
        seed_tlb(a);
        return true;
    };
    core->read_64bit = [base, seed_tlb](const address a, std::uint64_t *r) -> bool {
        if (a + 8 > MEM_SIZE)
            return false;
        std::memcpy(r, base + a, 8);
        seed_tlb(a);
        return true;
    };
    core->write_8bit = [base, seed_tlb](const address a, std::uint8_t *v) -> bool {
        if (a >= MEM_SIZE)
            return false;
        base[a] = *v;
        seed_tlb(a);
        return true;
    };
    core->write_16bit = [base, seed_tlb](const address a, std::uint16_t *v) -> bool {
        if (a + 2 > MEM_SIZE)
            return false;
        std::memcpy(base + a, v, 2);
        seed_tlb(a);
        return true;
    };
    core->write_32bit = [base, seed_tlb](const address a, std::uint32_t *v) -> bool {
        if (a + 4 > MEM_SIZE)
            return false;
        std::memcpy(base + a, v, 4);
        seed_tlb(a);
        return true;
    };
    core->write_64bit = [base, seed_tlb](const address a, std::uint64_t *v) -> bool {
        if (a + 8 > MEM_SIZE)
            return false;
        std::memcpy(base + a, v, 8);
        seed_tlb(a);
        return true;
    };

    core->exception_handler = [](exception_type, const std::uint32_t) -> bool { return false; };
    core->system_call_handler = [](const std::uint32_t) {};

    return core;
}

// ---------------------------------------------------------------------------
// Guest-visible state snapshot
// ---------------------------------------------------------------------------
struct cpu_state {
    std::uint32_t reg[16];
    std::uint32_t cpsr;

    bool operator==(const cpu_state &o) const {
        return std::memcmp(this, &o, sizeof(cpu_state)) == 0;
    }
};

cpu_state read_state(dyncom_core &c) {
    cpu_state s{};
    for (std::size_t i = 0; i < 16; i++) {
        s.reg[i] = c.get_reg(i);
    }
    s.cpsr = c.get_cpsr();
    return s;
}

void write_state(dyncom_core &c, const cpu_state &s) {
    for (std::size_t i = 0; i < 16; i++) {
        c.set_reg(i, s.reg[i]);
    }
    c.set_cpsr(s.cpsr);
}

// ---------------------------------------------------------------------------
// Golden ALU model (independent reference) -- ARM data-processing
// ---------------------------------------------------------------------------
constexpr std::uint32_t N_BIT = 1u << 31;
constexpr std::uint32_t Z_BIT = 1u << 30;
constexpr std::uint32_t C_BIT = 1u << 29;
constexpr std::uint32_t V_BIT = 1u << 28;

bool cond_passed(std::uint32_t cond, std::uint32_t cpsr) {
    const bool n = cpsr & N_BIT, z = cpsr & Z_BIT, c = cpsr & C_BIT, v = cpsr & V_BIT;
    switch (cond) {
    case 0x0: return z;                       // EQ
    case 0x1: return !z;                      // NE
    case 0x2: return c;                       // CS
    case 0x3: return !c;                      // CC
    case 0x4: return n;                       // MI
    case 0x5: return !n;                      // PL
    case 0x6: return v;                       // VS
    case 0x7: return !v;                      // VC
    case 0x8: return c && !z;                 // HI
    case 0x9: return !c || z;                 // LS
    case 0xA: return n == v;                  // GE
    case 0xB: return n != v;                  // LT
    case 0xC: return !z && (n == v);          // GT
    case 0xD: return z || (n != v);           // LE
    case 0xE: return true;                    // AL
    default: return true;
    }
}

// Independent add-with-carry (64-bit; deliberately NOT the interpreter's
// __builtin version, so it cross-checks rather than mirrors a bug).
std::uint32_t golden_addc(std::uint32_t a, std::uint32_t b, std::uint32_t cin, bool *carry, bool *overflow) {
    const std::uint64_t usum = (std::uint64_t)a + (std::uint64_t)b + (std::uint64_t)cin;
    const std::int64_t ssum = (std::int64_t)(std::int32_t)a + (std::int64_t)(std::int32_t)b + (std::int64_t)cin;
    const std::uint32_t r = (std::uint32_t)usum;
    *carry = (usum >> 32) != 0;
    *overflow = ((std::int64_t)(std::int32_t)r != ssum);
    return r;
}

// Barrel shifter (immediate-specified shifts + the immediate-operand rotate).
// Returns the operand value and the shifter carry-out.
std::uint32_t golden_shifter(std::uint32_t op2_field, bool is_immediate, const std::uint32_t *regs,
    bool carry_in, bool *carry_out) {
    if (is_immediate) {
        const std::uint32_t imm8 = op2_field & 0xFF;
        const std::uint32_t rot = ((op2_field >> 8) & 0xF) * 2;
        const std::uint32_t val = (rot == 0) ? imm8 : ((imm8 >> rot) | (imm8 << (32 - rot)));
        *carry_out = (rot == 0) ? carry_in : ((val >> 31) & 1);
        return val;
    }

    const std::uint32_t rm = regs[op2_field & 0xF];
    const std::uint32_t shift_type = (op2_field >> 5) & 0x3;

    // Register-specified shift (bit 4 set): the amount is the low byte of Rs and
    // the >=32 cases have their own carry-out rules.
    if (op2_field & 0x10) {
        const std::uint32_t amt = regs[(op2_field >> 8) & 0xF] & 0xFF;
        if (amt == 0) {
            *carry_out = carry_in;
            return rm;
        }
        switch (shift_type) {
        case 0: // LSL
            if (amt < 32) { *carry_out = (rm >> (32 - amt)) & 1; return rm << amt; }
            if (amt == 32) { *carry_out = rm & 1; return 0; }
            *carry_out = false; return 0;
        case 1: // LSR
            if (amt < 32) { *carry_out = (rm >> (amt - 1)) & 1; return rm >> amt; }
            if (amt == 32) { *carry_out = (rm >> 31) & 1; return 0; }
            *carry_out = false; return 0;
        case 2: // ASR
            if (amt < 32) { *carry_out = (rm >> (amt - 1)) & 1; return (std::uint32_t)((std::int32_t)rm >> amt); }
            *carry_out = (rm >> 31) & 1; return (rm & N_BIT) ? 0xFFFFFFFF : 0;
        default: { // ROR
            const std::uint32_t a = amt & 0x1F;
            if (a == 0) { *carry_out = (rm >> 31) & 1; return rm; } // amount a non-zero multiple of 32
            *carry_out = (rm >> (a - 1)) & 1;
            return (rm >> a) | (rm << (32 - a));
        }
        }
    }

    const std::uint32_t amount = (op2_field >> 7) & 0x1F; // immediate shift amount

    switch (shift_type) {
    case 0: // LSL
        if (amount == 0) {
            *carry_out = carry_in;
            return rm;
        }
        *carry_out = (rm >> (32 - amount)) & 1;
        return rm << amount;
    case 1: // LSR (amount 0 means 32)
        if (amount == 0) {
            *carry_out = (rm >> 31) & 1;
            return 0;
        }
        *carry_out = (rm >> (amount - 1)) & 1;
        return rm >> amount;
    case 2: // ASR (amount 0 means 32)
        if (amount == 0) {
            *carry_out = (rm >> 31) & 1;
            return (rm & N_BIT) ? 0xFFFFFFFF : 0;
        }
        *carry_out = (rm >> (amount - 1)) & 1;
        return (std::uint32_t)((std::int32_t)rm >> amount);
    default:  // ROR / RRX (amount 0 means RRX)
        if (amount == 0) { // RRX
            *carry_out = rm & 1;
            return (rm >> 1) | (carry_in ? N_BIT : 0);
        }
        *carry_out = (rm >> (amount - 1)) & 1;
        return (rm >> amount) | (rm << (32 - amount));
    }
}

// Apply one data-processing instruction to a golden state. Returns the expected
// post-state. Mirrors ARM semantics for the 16 DP opcodes (no PC writes, no
// S+Rd==15 SPSR restore -- the generator never emits those).
cpu_state golden_data_processing(std::uint32_t inst, const cpu_state &in) {
    cpu_state out = in;
    out.reg[15] = in.reg[15] + 4; // PC advances one ARM instruction

    const std::uint32_t cond = inst >> 28;
    if (!cond_passed(cond, in.cpsr)) {
        return out; // condition failed: only PC advanced
    }

    const bool is_imm = (inst >> 25) & 1;
    const std::uint32_t opcode = (inst >> 21) & 0xF;
    const bool S = (inst >> 20) & 1;
    const std::uint32_t rn = (inst >> 16) & 0xF;
    const std::uint32_t rd = (inst >> 12) & 0xF;
    const std::uint32_t op2_field = inst & 0xFFF;

    const bool cin = (in.cpsr & C_BIT) != 0;
    bool shifter_carry = cin;
    const std::uint32_t b = golden_shifter(op2_field, is_imm, in.reg, cin, &shifter_carry);
    const std::uint32_t a = in.reg[rn];

    std::uint32_t result = 0;
    bool write_rd = true;
    bool logical = true;
    bool carry = cin, overflow = (in.cpsr & V_BIT) != 0;

    switch (opcode) {
    case 0x0: result = a & b; break;                                   // AND
    case 0x1: result = a ^ b; break;                                   // EOR
    case 0x2: result = golden_addc(a, ~b, 1, &carry, &overflow); logical = false; break;       // SUB
    case 0x3: result = golden_addc(~a, b, 1, &carry, &overflow); logical = false; break;       // RSB
    case 0x4: result = golden_addc(a, b, 0, &carry, &overflow); logical = false; break;        // ADD
    case 0x5: result = golden_addc(a, b, cin, &carry, &overflow); logical = false; break;      // ADC
    case 0x6: result = golden_addc(a, ~b, cin, &carry, &overflow); logical = false; break;     // SBC
    case 0x7: result = golden_addc(~a, b, cin, &carry, &overflow); logical = false; break;     // RSC
    case 0x8: result = a & b; write_rd = false; break;                 // TST
    case 0x9: result = a ^ b; write_rd = false; break;                 // TEQ
    case 0xA: result = golden_addc(a, ~b, 1, &carry, &overflow); logical = false; write_rd = false; break; // CMP
    case 0xB: result = golden_addc(a, b, 0, &carry, &overflow); logical = false; write_rd = false; break;  // CMN
    case 0xC: result = a | b; break;                                   // ORR
    case 0xD: result = b; break;                                       // MOV
    case 0xE: result = a & ~b; break;                                  // BIC
    case 0xF: result = ~b; break;                                      // MVN
    default: break;
    }

    if (write_rd) {
        out.reg[rd] = result;
    }

    if (S) {
        std::uint32_t cpsr = out.cpsr & ~(N_BIT | Z_BIT | C_BIT | V_BIT);
        if (result & N_BIT) cpsr |= N_BIT;
        if (result == 0) cpsr |= Z_BIT;
        if (logical) {
            if (shifter_carry) cpsr |= C_BIT;
            cpsr |= (in.cpsr & V_BIT); // V unchanged for logical
        } else {
            if (carry) cpsr |= C_BIT;
            if (overflow) cpsr |= V_BIT;
        }
        out.cpsr = cpsr;
    }

    return out;
}

// ---------------------------------------------------------------------------
// Random instruction + state generation
// ---------------------------------------------------------------------------
struct rng {
    std::mt19937 e;
    explicit rng(std::uint32_t seed)
        : e(seed) {
    }
    std::uint32_t u32() { return e(); }
    std::uint32_t range(std::uint32_t n) { return e() % n; }
    bool flip() { return e() & 1; }
};

// A random data-processing instruction. cond is AL most of the time but
// sometimes a real condition (to exercise the conditional path). Operand
// registers are kept out of R15 (PC) so there are no PC-as-operand / PC-write
// edge cases -- those belong in a dedicated edge corpus.
std::uint32_t gen_data_processing(rng &r) {
    const std::uint32_t cond = (r.range(4) == 0) ? r.range(14) : 0xE; // 0..13 or AL
    std::uint32_t opcode = r.range(16);
    const bool is_imm = r.flip();
    bool S = r.flip();
    if (opcode >= 0x8 && opcode <= 0xB) {
        S = true; // TST/TEQ/CMP/CMN always set flags
    }
    const std::uint32_t rn = r.range(15);  // 0..14
    const std::uint32_t rd = r.range(15);  // 0..14 (never PC)

    std::uint32_t op2;
    if (is_imm) {
        op2 = (r.range(16) << 8) | r.range(256); // rotate(4) + imm8(8)
    } else {
        const std::uint32_t rm = r.range(15);     // 0..14
        const std::uint32_t shtype = r.range(4);
        if (r.flip()) {
            // Register-specified shift: bit4 = 1, amount = Rs[11:8] (0..14).
            const std::uint32_t rs = r.range(15);
            op2 = (rs << 8) | (shtype << 5) | (1u << 4) | rm;
        } else {
            const std::uint32_t amount = r.range(32);
            op2 = (amount << 7) | (shtype << 5) | rm; // bit4 = 0 -> immediate shift
        }
    }

    return (cond << 28) | (0u << 26) | ((is_imm ? 1u : 0u) << 25) | (opcode << 21) | ((S ? 1u : 0u) << 20) | (rn << 16) | (rd << 12) | op2;
}

cpu_state gen_state(rng &r) {
    cpu_state s{};
    for (int i = 0; i < 15; i++) {
        s.reg[i] = r.u32();
    }
    s.reg[15] = 0; // PC at the instruction under test
    // USER mode, ARM state, IRQ/FIQ disabled; random NZCV.
    s.cpsr = 0x10 | (0xF0000000u & r.u32());
    return s;
}

// ---------------------------------------------------------------------------
// Load/store (single data transfer: LDR/STR/LDRB/STRB, immediate offset)
// ---------------------------------------------------------------------------
constexpr std::uint32_t LS_DATA_LO = 0x1000;       // data window (one page)
constexpr std::uint32_t LS_DATA_HI = 0x2000;
constexpr std::uint32_t LS_DATA_BASE = 0x1800;     // base register points here

// Generate a single-data-transfer instruction whose base register is pointed
// into the data window so the access always lands in mapped memory. Word
// accesses keep an aligned offset (unaligned word rotation belongs in a later
// edge corpus). Returns the encoding; sets init.reg[Rn] = base.
std::uint32_t gen_load_store(rng &r, cpu_state &init) {
    const std::uint32_t cond = (r.range(4) == 0) ? r.range(14) : 0xE;
    const std::uint32_t P = r.flip() ? 1 : 0;
    const std::uint32_t U = r.flip() ? 1 : 0;
    const std::uint32_t B = r.flip() ? 1 : 0;            // 1 = byte, 0 = word
    const std::uint32_t W = (P == 1) ? (r.flip() ? 1 : 0) : 0; // post-index: no W (would be LDRT)
    const std::uint32_t L = r.flip() ? 1 : 0;            // 1 = load, 0 = store

    const std::uint32_t rn = r.range(15);                // base, never PC
    std::uint32_t rd = r.range(15);
    while (rd == rn) {
        rd = r.range(15);                                // base==dest writeback is unpredictable
    }

    init.reg[rn] = LS_DATA_BASE;

    const bool reg_off = r.flip();
    if (reg_off) {
        // Scaled-register offset (I=1), LSL-scaled so a word access stays
        // aligned; Rm holds a small value (!= base) so the address stays in the
        // window. This is the classic [Rn, Rm, LSL #k] array-index form.
        std::uint32_t rm = r.range(15);
        while (rm == rn) {
            rm = r.range(15);
        }
        const std::uint32_t shamt = (B == 0) ? 2 : (r.flip() ? 0 : 1); // word: <<2 (aligned)
        init.reg[rm] = r.range(0x40);                    // 0..0x3F -> offset <= 0xFC
        const std::uint32_t off_field = (shamt << 7) | (0u << 5) | rm; // LSL, bit4=0
        return (cond << 28) | (0x1u << 26) | (1u << 25) | (P << 24) | (U << 23) | (B << 22) | (W << 21) | (L << 20) | (rn << 16) | (rd << 12) | off_field;
    }

    std::uint32_t offset = r.range(0x100);               // small immediate, stays in window
    if (B == 0) {
        offset &= ~0x3u;                                 // word: aligned
    }

    return (cond << 28) | (0x1u << 26) | (0u << 25) | (P << 24) | (U << 23) | (B << 22) | (W << 21) | (L << 20) | (rn << 16) | (rd << 12) | offset;
}

// Golden model for single data transfer, operating on a memory image.
cpu_state golden_load_store(std::uint32_t inst, const cpu_state &in, std::uint8_t *mem) {
    cpu_state out = in;
    out.reg[15] = in.reg[15] + 4;

    const std::uint32_t cond = inst >> 28;
    if (!cond_passed(cond, in.cpsr)) {
        return out;
    }

    const std::uint32_t P = (inst >> 24) & 1;
    const std::uint32_t U = (inst >> 23) & 1;
    const std::uint32_t B = (inst >> 22) & 1;
    const std::uint32_t W = (inst >> 21) & 1;
    const std::uint32_t L = (inst >> 20) & 1;
    const std::uint32_t rn = (inst >> 16) & 0xF;
    const std::uint32_t rd = (inst >> 12) & 0xF;

    std::uint32_t offset;
    if ((inst >> 25) & 1) {
        // Scaled-register offset (same shift forms as a data-processing
        // immediate shift, but no carry-out is needed for an address).
        const std::uint32_t rm = in.reg[inst & 0xF];
        const std::uint32_t shtype = (inst >> 5) & 0x3;
        const std::uint32_t shamt = (inst >> 7) & 0x1F;
        switch (shtype) {
        case 0: offset = shamt ? (rm << shamt) : rm; break;                                   // LSL (#0 = Rm)
        case 1: offset = shamt ? (rm >> shamt) : 0; break;                                     // LSR (#0 = #32 -> 0)
        case 2: offset = shamt ? (std::uint32_t)((std::int32_t)rm >> shamt)
                               : ((rm & N_BIT) ? 0xFFFFFFFF : 0); break;                       // ASR (#0 = #32)
        default: offset = shamt ? ((rm >> shamt) | (rm << (32 - shamt)))
                                : (((in.cpsr & C_BIT) ? N_BIT : 0) | (rm >> 1)); break;        // ROR / RRX
        }
    } else {
        offset = inst & 0xFFF;
    }

    const std::uint32_t base = in.reg[rn];
    const std::uint32_t off_applied = U ? (base + offset) : (base - offset);
    const std::uint32_t addr = P ? off_applied : base;

    if (L) {
        std::uint32_t val;
        if (B) {
            val = mem[addr];
        } else {
            std::memcpy(&val, mem + addr, 4);
        }
        out.reg[rd] = val;
    } else {
        if (B) {
            mem[addr] = static_cast<std::uint8_t>(in.reg[rd]);
        } else {
            std::uint32_t v = in.reg[rd];
            std::memcpy(mem + addr, &v, 4);
        }
    }

    if (!P) {
        out.reg[rn] = off_applied; // post-indexed always writes back
    } else if (W) {
        out.reg[rn] = off_applied; // pre-indexed with W
    }

    return out;
}

// ---------------------------------------------------------------------------
// Block transfer (LDM/STM) -- the heaviest user of the inline memory accessors
// ---------------------------------------------------------------------------
// Generate an LDM/STM whose base points into the data window; the register list
// is a non-empty subset of r0..r14 excluding the base (so writeback / base-in-
// list edge cases don't apply) and excluding PC (no branch). S (PSR/user-bank)
// is always 0.
std::uint32_t gen_ldm_stm(rng &r, cpu_state &init) {
    const std::uint32_t cond = (r.range(4) == 0) ? r.range(14) : 0xE;
    const std::uint32_t P = r.flip() ? 1 : 0;
    const std::uint32_t U = r.flip() ? 1 : 0;
    const std::uint32_t W = r.flip() ? 1 : 0;
    const std::uint32_t L = r.flip() ? 1 : 0;
    const std::uint32_t rn = r.range(15); // base, 0..14

    std::uint32_t list = 0;
    while (list == 0) {
        list = (r.u32() & 0x7FFF) & ~(1u << rn); // bits 0..14, exclude base
    }

    init.reg[rn] = LS_DATA_BASE;
    return (cond << 28) | (0x4u << 25) | (P << 24) | (U << 23) | (0u << 22) | (W << 21) | (L << 20) | (rn << 16) | list;
}

cpu_state golden_ldm_stm(std::uint32_t inst, const cpu_state &in, std::uint8_t *mem) {
    cpu_state out = in;
    out.reg[15] = in.reg[15] + 4;

    const std::uint32_t cond = inst >> 28;
    if (!cond_passed(cond, in.cpsr)) {
        return out;
    }

    const std::uint32_t P = (inst >> 24) & 1;
    const std::uint32_t U = (inst >> 23) & 1;
    const std::uint32_t W = (inst >> 21) & 1;
    const std::uint32_t L = (inst >> 20) & 1;
    const std::uint32_t rn = (inst >> 16) & 0xF;
    const std::uint32_t list = inst & 0x7FFF; // bits 0..14 (PC excluded by gen)
    const std::uint32_t n = static_cast<std::uint32_t>(__builtin_popcount(list));
    const std::uint32_t base = in.reg[rn];

    // Lowest-numbered register always goes to the lowest address.
    std::uint32_t addr = U ? (P ? base + 4 : base) : (P ? base - 4 * n : base - 4 * (n - 1));

    for (std::uint32_t i = 0; i < 15; i++) {
        if (!(list & (1u << i))) {
            continue;
        }
        if (L) {
            std::uint32_t v;
            std::memcpy(&v, mem + addr, 4);
            out.reg[i] = v;
        } else {
            std::uint32_t v = in.reg[i];
            std::memcpy(mem + addr, &v, 4);
        }
        addr += 4;
    }

    if (W) {
        out.reg[rn] = U ? base + 4 * n : base - 4 * n;
    }

    return out;
}

// ---------------------------------------------------------------------------
// Halfword / signed transfers (LDRH/STRH/LDRSB/LDRSH, immediate offset)
// ---------------------------------------------------------------------------
std::uint32_t gen_halfword(rng &r, cpu_state &init) {
    const std::uint32_t cond = (r.range(4) == 0) ? r.range(14) : 0xE;
    const std::uint32_t P = r.flip() ? 1 : 0;
    const std::uint32_t U = r.flip() ? 1 : 0;
    const std::uint32_t W = (P == 1) ? (r.flip() ? 1 : 0) : 0;

    std::uint32_t L, S, H;
    switch (r.range(4)) {
    case 0: L = 1; S = 0; H = 1; break; // LDRH
    case 1: L = 0; S = 0; H = 1; break; // STRH
    case 2: L = 1; S = 1; H = 1; break; // LDRSH
    default: L = 1; S = 1; H = 0; break; // LDRSB (byte)
    }

    const std::uint32_t rn = r.range(15);
    std::uint32_t rd = r.range(15);
    while (rd == rn) {
        rd = r.range(15);
    }

    std::uint32_t offset = r.range(0x40);
    if (H == 1) {
        offset &= ~1u; // halfword aligned
    }
    init.reg[rn] = LS_DATA_BASE;

    const std::uint32_t immH = (offset >> 4) & 0xF;
    const std::uint32_t immL = offset & 0xF;
    return (cond << 28) | (0u << 25) | (P << 24) | (U << 23) | (1u << 22) | (W << 21) | (L << 20) | (rn << 16) | (rd << 12) | (immH << 8) | (1u << 7) | (S << 6) | (H << 5) | (1u << 4) | immL;
}

cpu_state golden_halfword(std::uint32_t inst, const cpu_state &in, std::uint8_t *mem) {
    cpu_state out = in;
    out.reg[15] = in.reg[15] + 4;

    const std::uint32_t cond = inst >> 28;
    if (!cond_passed(cond, in.cpsr)) {
        return out;
    }

    const std::uint32_t P = (inst >> 24) & 1;
    const std::uint32_t U = (inst >> 23) & 1;
    const std::uint32_t W = (inst >> 21) & 1;
    const std::uint32_t L = (inst >> 20) & 1;
    const std::uint32_t S = (inst >> 6) & 1;
    const std::uint32_t H = (inst >> 5) & 1;
    const std::uint32_t rn = (inst >> 16) & 0xF;
    const std::uint32_t rd = (inst >> 12) & 0xF;
    const std::uint32_t offset = (((inst >> 8) & 0xF) << 4) | (inst & 0xF);

    const std::uint32_t base = in.reg[rn];
    const std::uint32_t off_applied = U ? (base + offset) : (base - offset);
    const std::uint32_t addr = P ? off_applied : base;

    if (L) {
        if (!H) { // LDRSB
            out.reg[rd] = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(mem[addr])));
        } else if (S) { // LDRSH
            std::uint16_t h;
            std::memcpy(&h, mem + addr, 2);
            out.reg[rd] = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(h)));
        } else { // LDRH
            std::uint16_t h;
            std::memcpy(&h, mem + addr, 2);
            out.reg[rd] = h;
        }
    } else { // STRH
        std::uint16_t h = static_cast<std::uint16_t>(in.reg[rd]);
        std::memcpy(mem + addr, &h, 2);
    }

    if (!P) {
        out.reg[rn] = off_applied;
    } else if (W) {
        out.reg[rn] = off_applied;
    }

    return out;
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------
void dump_state(const char *tag, const cpu_state &s) {
    std::printf("  %s:", tag);
    for (int i = 0; i < 16; i++) {
        std::printf(" r%d=%08X", i, s.reg[i]);
    }
    std::printf(" cpsr=%08X\n", s.cpsr);
}

bool report_mismatch(const char *what, std::uint32_t inst, std::uint32_t seed,
    const cpu_state &expect, const cpu_state &got) {
    std::printf("[DIVERGENCE] %s  inst=%08X seed=%u\n", what, inst, seed);
    dump_state("expect", expect);
    dump_state("got   ", got);
    return false;
}

} // namespace

int main(int argc, char **argv) {
    std::uint32_t base_seed = 1;
    std::uint32_t count = 200000;
    if (argc > 1) base_seed = static_cast<std::uint32_t>(std::strtoul(argv[1], nullptr, 0));
    if (argc > 2) count = static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 0));

    std::printf("dyncom_difftest: data-processing + load/store + ldm/stm + halfword, %u cases from seed %u\n", count, base_seed);

    diff_env env_a, env_b;
    auto core_a = make_core(env_a);
    auto core_b = make_core(env_b);
    std::vector<std::uint8_t> golden_mem(MEM_SIZE, 0);

    std::uint32_t failures = 0;
    auto window_eq = [](const std::vector<std::uint8_t> &x, const std::vector<std::uint8_t> &y) {
        return std::memcmp(x.data() + LS_DATA_LO, y.data() + LS_DATA_LO, LS_DATA_HI - LS_DATA_LO) == 0;
    };

    for (std::uint32_t i = 0; i < count; i++) {
        const std::uint32_t seed = base_seed + i;
        rng r(seed);

        cpu_state init = gen_state(r);
        const std::uint32_t kind = r.range(5); // 0,1 data-proc; 2 single; 3 ldm/stm; 4 halfword
        const bool touches_mem = (kind >= 2);
        std::uint32_t inst;
        const char *kind_name;
        if (kind < 2) {
            inst = gen_data_processing(r);
            kind_name = "dyncom != golden";
        } else if (kind == 2) {
            inst = gen_load_store(r, init);
            kind_name = "dyncom != golden (load/store)";
        } else if (kind == 3) {
            inst = gen_ldm_stm(r, init);
            kind_name = "dyncom != golden (ldm/stm)";
        } else {
            inst = gen_halfword(r, init);
            kind_name = "dyncom != golden (halfword)";
        }

        // Place the instruction at PC 0 in both envs and seed the data window
        // identically in A, B and the golden image (memory-touching cases only).
        std::memcpy(env_a.mem.data(), &inst, 4);
        std::memcpy(env_b.mem.data(), &inst, 4);
        if (touches_mem) {
            for (std::uint32_t a = LS_DATA_LO; a < LS_DATA_HI; a += 4) {
                const std::uint32_t w = r.u32();
                std::memcpy(env_a.mem.data() + a, &w, 4);
                std::memcpy(env_b.mem.data() + a, &w, 4);
                std::memcpy(golden_mem.data() + a, &w, 4);
            }
        }

        core_a->imb_range(0, 8);
        core_b->imb_range(0, 8);

        write_state(*core_a, init);
        write_state(*core_b, init);

        core_a->set_pc(0);
        core_b->set_pc(0);
        core_a->run(1);
        core_b->run(1);

        const cpu_state got_a = read_state(*core_a);
        const cpu_state got_b = read_state(*core_b);
        cpu_state golden;
        if (kind < 2) {
            golden = golden_data_processing(inst, init);
        } else if (kind == 2) {
            golden = golden_load_store(inst, init, golden_mem.data());
        } else if (kind == 3) {
            golden = golden_ldm_stm(inst, init, golden_mem.data());
        } else {
            golden = golden_halfword(inst, init, golden_mem.data());
        }

        // (a) dyncom vs the independent golden model (registers + memory).
        if (!(got_a == golden) || (touches_mem && !window_eq(env_a.mem, golden_mem))) {
            report_mismatch(kind_name, inst, seed, golden, got_a);
            if (++failures >= 20) break;
            continue;
        }
        // (b) self-A/B (determinism today; a flag-gated optimization later).
        if (!(got_a == got_b) || (touches_mem && !window_eq(env_a.mem, env_b.mem))) {
            report_mismatch("core A != core B", inst, seed, got_a, got_b);
            if (++failures >= 20) break;
        }
    }

    // Negative control: prove the comparator catches a deliberate divergence.
    {
        cpu_state x{}, y{};
        y.reg[3] = 1;
        if (x == y) {
            std::printf("[HARNESS BUG] comparator failed to detect an injected divergence\n");
            failures++;
        }
    }

    if (failures == 0) {
        std::printf("dyncom_difftest: PASS (%u cases, golden + self-A/B + negative control)\n", count);
        return 0;
    }
    std::printf("dyncom_difftest: FAIL (%u divergences)\n", failures);
    return 1;
}
