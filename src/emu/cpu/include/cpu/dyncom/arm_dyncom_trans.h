#pragma once
#ifdef _MSC_VER
// nonstandard extension used: zero-sized array in struct/union
#pragma warning(disable : 4200)
#endif

#include <common/types.h>
#include <cstddef>

struct ARMul_State;

// Shifter-operand evaluators and load/store address generators used to be
// stored as function pointers in the translated instruction structs. Indirect
// calls on these per-instruction paths are expensive (especially WASM
// call_indirect), so the translator stores a small index instead and the
// interpreter dispatches through a local switch with inlined bodies.
enum shtop_index : unsigned int {
    SHTOP_IMMEDIATE = 0,
    SHTOP_REGISTER,
    SHTOP_LSL_IMM,
    SHTOP_LSL_REG,
    SHTOP_LSR_IMM,
    SHTOP_LSR_REG,
    SHTOP_ASR_IMM,
    SHTOP_ASR_REG,
    SHTOP_ROR_IMM,
    SHTOP_ROR_REG,
    SHTOP_INVALID,
};

enum addr_mode_index : unsigned int {
    ADDRMODE_LNSW_IMM_OFFSET = 0,
    ADDRMODE_LNSW_REG_OFFSET,
    ADDRMODE_LNSW_SCALED_REG_OFFSET,
    ADDRMODE_LNSW_IMM_PRE,
    ADDRMODE_LNSW_REG_PRE,
    ADDRMODE_LNSW_SCALED_REG_PRE,
    ADDRMODE_LNSW_IMM_POST,
    ADDRMODE_LNSW_REG_POST,
    ADDRMODE_LNSW_SCALED_REG_POST,
    ADDRMODE_MLNS_IMM_OFFSET,
    ADDRMODE_MLNS_REG_OFFSET,
    ADDRMODE_MLNS_IMM_PRE,
    ADDRMODE_MLNS_REG_PRE,
    ADDRMODE_MLNS_IMM_POST,
    ADDRMODE_MLNS_REG_POST,
    ADDRMODE_LDNSTM_INC_AFTER,
    ADDRMODE_LDNSTM_INC_BEFORE,
    ADDRMODE_LDNSTM_DEC_AFTER,
    ADDRMODE_LDNSTM_DEC_BEFORE,
    ADDRMODE_INVALID,
};

enum class TransExtData {
    COND = (1 << 0),
    NON_BRANCH = (1 << 1),
    DIRECT_BRANCH = (1 << 2),
    INDIRECT_BRANCH = (1 << 3),
    CALL = (1 << 4),
    RET = (1 << 5),
    END_OF_PAGE = (1 << 6),
    THUMB = (1 << 7),
    SINGLE_STEP = (1 << 8)
};

struct arm_inst {
    unsigned int idx;
    unsigned int cond;
    TransExtData br;
    char component[0];
};

struct generic_arm_inst {
    std::uint32_t Ra;
    std::uint32_t Rm;
    std::uint32_t Rn;
    std::uint32_t Rd;
    std::uint8_t op1;
    std::uint8_t op2;
};

struct adc_inst {
    unsigned int I;
    unsigned int S;
    unsigned int Rn;
    unsigned int Rd;
    unsigned int shifter_operand;
    unsigned int shtop_idx;
};

struct add_inst {
    unsigned int I;
    unsigned int S;
    unsigned int Rn;
    unsigned int Rd;
    unsigned int shifter_operand;
    unsigned int shtop_idx;
};

struct orr_inst {
    unsigned int I;
    unsigned int S;
    unsigned int Rn;
    unsigned int Rd;
    unsigned int shifter_operand;
    unsigned int shtop_idx;
};

struct and_inst {
    unsigned int I;
    unsigned int S;
    unsigned int Rn;
    unsigned int Rd;
    unsigned int shifter_operand;
    unsigned int shtop_idx;
};

struct eor_inst {
    unsigned int I;
    unsigned int S;
    unsigned int Rn;
    unsigned int Rd;
    unsigned int shifter_operand;
    unsigned int shtop_idx;
};

struct bbl_inst {
    unsigned int L;
    int signed_immed_24;
    unsigned int next_addr;
    unsigned int jmp_addr;
};

struct bx_inst {
    unsigned int Rm;
};

struct blx_inst {
    union {
        std::int32_t signed_immed_24;
        std::uint32_t Rm;
    } val;
    unsigned int inst;
};

struct clz_inst {
    unsigned int Rm;
    unsigned int Rd;
};

struct cps_inst {
    unsigned int imod0;
    unsigned int imod1;
    unsigned int mmod;
    unsigned int A, I, F;
    unsigned int mode;
};

struct clrex_inst {};

struct cpy_inst {
    unsigned int Rm;
    unsigned int Rd;
};

struct bic_inst {
    unsigned int I;
    unsigned int S;
    unsigned int Rn;
    unsigned int Rd;
    unsigned int shifter_operand;
    unsigned int shtop_idx;
};

struct sub_inst {
    unsigned int I;
    unsigned int S;
    unsigned int Rn;
    unsigned int Rd;
    unsigned int shifter_operand;
    unsigned int shtop_idx;
};

struct tst_inst {
    unsigned int I;
    unsigned int S;
    unsigned int Rn;
    unsigned int Rd;
    unsigned int shifter_operand;
    unsigned int shtop_idx;
};

struct cmn_inst {
    unsigned int I;
    unsigned int Rn;
    unsigned int shifter_operand;
    unsigned int shtop_idx;
};

struct teq_inst {
    unsigned int I;
    unsigned int Rn;
    unsigned int shifter_operand;
    unsigned int shtop_idx;
};

struct stm_inst {
    unsigned int inst;
};

struct bkpt_inst {
    std::uint32_t imm;
};

struct stc_inst {};

struct ldc_inst {};

struct swi_inst {
    unsigned int num;
};

struct cmp_inst {
    unsigned int I;
    unsigned int Rn;
    unsigned int shifter_operand;
    unsigned int shtop_idx;
};

struct mov_inst {
    unsigned int I;
    unsigned int S;
    unsigned int Rd;
    unsigned int shifter_operand;
    unsigned int shtop_idx;
};

struct mvn_inst {
    unsigned int I;
    unsigned int S;
    unsigned int Rd;
    unsigned int shifter_operand;
    unsigned int shtop_idx;
};

struct rev_inst {
    unsigned int Rd;
    unsigned int Rm;
    unsigned int op1;
    unsigned int op2;
};

struct rsb_inst {
    unsigned int I;
    unsigned int S;
    unsigned int Rn;
    unsigned int Rd;
    unsigned int shifter_operand;
    unsigned int shtop_idx;
};

struct rsc_inst {
    unsigned int I;
    unsigned int S;
    unsigned int Rn;
    unsigned int Rd;
    unsigned int shifter_operand;
    unsigned int shtop_idx;
};

struct sbc_inst {
    unsigned int I;
    unsigned int S;
    unsigned int Rn;
    unsigned int Rd;
    unsigned int shifter_operand;
    unsigned int shtop_idx;
};

struct mul_inst {
    unsigned int S;
    unsigned int Rd;
    unsigned int Rs;
    unsigned int Rm;
};

struct smul_inst {
    unsigned int Rd;
    unsigned int Rs;
    unsigned int Rm;
    unsigned int x;
    unsigned int y;
};

struct umull_inst {
    unsigned int S;
    unsigned int RdHi;
    unsigned int RdLo;
    unsigned int Rs;
    unsigned int Rm;
};

struct smlad_inst {
    unsigned int m;
    unsigned int Rm;
    unsigned int Rd;
    unsigned int Ra;
    unsigned int Rn;
    unsigned int op1;
    unsigned int op2;
};

struct smla_inst {
    unsigned int x;
    unsigned int y;
    unsigned int Rm;
    unsigned int Rd;
    unsigned int Rs;
    unsigned int Rn;
};

struct smlalxy_inst {
    unsigned int x;
    unsigned int y;
    unsigned int RdLo;
    unsigned int RdHi;
    unsigned int Rm;
    unsigned int Rn;
};

struct ssat_inst {
    unsigned int Rn;
    unsigned int Rd;
    unsigned int imm5;
    unsigned int sat_imm;
    unsigned int shift_type;
};

struct umaal_inst {
    unsigned int Rn;
    unsigned int Rm;
    unsigned int RdHi;
    unsigned int RdLo;
};

struct umlal_inst {
    unsigned int S;
    unsigned int Rm;
    unsigned int Rs;
    unsigned int RdHi;
    unsigned int RdLo;
};

struct smlal_inst {
    unsigned int S;
    unsigned int Rm;
    unsigned int Rs;
    unsigned int RdHi;
    unsigned int RdLo;
};

struct smlald_inst {
    unsigned int RdLo;
    unsigned int RdHi;
    unsigned int Rm;
    unsigned int Rn;
    unsigned int swap;
    unsigned int op1;
    unsigned int op2;
};

struct mla_inst {
    unsigned int S;
    unsigned int Rn;
    unsigned int Rd;
    unsigned int Rs;
    unsigned int Rm;
};

struct mrc_inst {
    unsigned int opcode_1;
    unsigned int opcode_2;
    unsigned int cp_num;
    unsigned int crn;
    unsigned int crm;
    unsigned int Rd;
    unsigned int inst;
};

struct mcr_inst {
    unsigned int opcode_1;
    unsigned int opcode_2;
    unsigned int cp_num;
    unsigned int crn;
    unsigned int crm;
    unsigned int Rd;
    unsigned int inst;
};

struct mcrr_inst {
    unsigned int opcode_1;
    unsigned int cp_num;
    unsigned int crm;
    unsigned int rt;
    unsigned int rt2;
};

struct mrs_inst {
    unsigned int R;
    unsigned int Rd;
};

struct msr_inst {
    unsigned int field_mask;
    unsigned int R;
    unsigned int inst;
};

struct pld_inst {};

struct sxtb_inst {
    unsigned int Rd;
    unsigned int Rm;
    unsigned int rotate;
};

struct sxtab_inst {
    unsigned int Rd;
    unsigned int Rn;
    unsigned int Rm;
    unsigned rotate;
};

struct sxtah_inst {
    unsigned int Rd;
    unsigned int Rn;
    unsigned int Rm;
    unsigned int rotate;
};

struct sxth_inst {
    unsigned int Rd;
    unsigned int Rm;
    unsigned int rotate;
};

struct uxtab_inst {
    unsigned int Rn;
    unsigned int Rd;
    unsigned int rotate;
    unsigned int Rm;
};

struct uxtah_inst {
    unsigned int Rn;
    unsigned int Rd;
    unsigned int rotate;
    unsigned int Rm;
};

struct uxth_inst {
    unsigned int Rd;
    unsigned int Rm;
    unsigned int rotate;
};

struct cdp_inst {
    unsigned int opcode_1;
    unsigned int CRn;
    unsigned int CRd;
    unsigned int cp_num;
    unsigned int opcode_2;
    unsigned int CRm;
    unsigned int inst;
};

struct uxtb_inst {
    unsigned int Rd;
    unsigned int Rm;
    unsigned int rotate;
};

struct swp_inst {
    unsigned int Rn;
    unsigned int Rd;
    unsigned int Rm;
};

struct setend_inst {
    unsigned int set_bigend;
};

struct b_2_thumb {
    unsigned int imm;
};
struct b_cond_thumb {
    unsigned int imm;
    unsigned int cond;
};

struct bl_1_thumb {
    unsigned int imm;
};
struct bl_2_thumb {
    unsigned int imm;
};
struct blx_1_thumb {
    unsigned int imm;
    unsigned int instr;
};

struct pkh_inst {
    unsigned int Rm;
    unsigned int Rn;
    unsigned int Rd;
    unsigned char imm;
};

// Floating point VFPv3 structures
#define VFP_INTERPRETER_STRUCT
#include <cpu/dyncom/vfp/vfpinstr.h>
#undef VFP_INTERPRETER_STRUCT

struct ldst_inst {
    unsigned int inst;
    unsigned int addr_mode; // addr_mode_index
};

typedef arm_inst *ARM_INST_PTR;
typedef ARM_INST_PTR (*transop_fp_t)(ARMul_State *, unsigned int, int);

extern const transop_fp_t arm_instruction_trans[];
extern const std::size_t arm_instruction_trans_len;
