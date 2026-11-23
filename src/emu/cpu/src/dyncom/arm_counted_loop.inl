// ARM register-only decrement-and-branch loops. Batch independent affine
// updates and, when present, a comparison whose carry feeds an accumulator.
struct arm_counted_loop {
    static constexpr unsigned no_register = 15;
    static constexpr unsigned max_body_words = 128;

    struct operand {
        std::uint32_t constant = 0;
        unsigned reg = no_register;

        std::uint32_t value(const std::array<std::uint32_t, 16> &regs) const {
            return reg == no_register ? constant : regs[reg];
        }
    };

    std::array<std::int8_t, no_register> delta_coeff{};
    std::uint32_t delta_constant = 0;
    operand comparison;
    operand carry_operand;
    std::uint32_t decrement = 0;
    unsigned induction = no_register;
    unsigned counter = no_register;
    unsigned accumulator = no_register;
    unsigned repetitions = 1;
    unsigned instruction_count = 0;
    bool induction_is_left = false;
};

static bool decode_unshifted_arm_operand(std::uint32_t word, arm_counted_loop::operand &out) {
    out = {};
    if (BIT(word, 25)) {
        out.constant = std::rotr(word & 0xff, static_cast<int>((word >> 7) & 30));
        return true;
    }
    if ((word & 0xff0) || (word & 15) == arm_counted_loop::no_register)
        return false;
    out.reg = word & 15;
    return true;
}

static bool analyze_arm_counted_loop(ARMul_State *cpu, std::uint32_t start,
    std::uint32_t branch_pc, arm_counted_loop &loop) {
    using plan = arm_counted_loop;
    const unsigned words = (branch_pc - start) / 4;
    if (words < 2 || words > plan::max_body_words)
        return false;

    // The last arithmetic instruction must be SUBS counter,counter,#imm.
    const auto tail = cpu->ReadCode(branch_pc - 4);
    loop.counter = BITS(tail, 12, 15);
    if ((tail & 0xfff00000) != 0xe2500000 || BITS(tail, 16, 19) != loop.counter
        || loop.counter == plan::no_register)
        return false;
    plan::operand decrement;
    decode_unshifted_arm_operand(tail, decrement);
    if (!decrement.constant)
        return false;
    loop.decrement = decrement.constant;
    loop.instruction_count = words + 1;

    std::array<std::uint32_t, plan::max_body_words> body;
    for (unsigned i = 0; i < words - 1; ++i)
        body[i] = cpu->ReadCode(start + i * 4);

    const unsigned body_words = words - 1;
    std::uint32_t invariant_registers = 0;
    auto mark_invariant = [&](const plan::operand &operand) {
        if (operand.reg != plan::no_register)
            invariant_registers |= 1u << operand.reg;
    };

    unsigned i = 0;
    for (; i < body_words; ++i) {
        const auto word = body[i];
        const auto op = BITS(word, 21, 24);
        if (op != 2 && op != 4) // SUB or ADD.
            break;
        const unsigned dest = BITS(word, 12, 15);
        plan::operand operand;
        if ((word & 0xfc000000) != 0xe0000000 || dest != BITS(word, 16, 19)
            || dest == plan::no_register || (i && dest != loop.induction)
            || !decode_unshifted_arm_operand(word, operand))
            return false;
        loop.induction = dest;
        const int sign = op == 4 ? 1 : -1;
        if (operand.reg == plan::no_register)
            loop.delta_constant += sign == 1 ? operand.constant : -operand.constant;
        else
            loop.delta_coeff[operand.reg] += sign;
        mark_invariant(operand);
    }

    if (loop.induction == plan::no_register || loop.induction == loop.counter)
        return false;

    if (i != body_words) {
        // CMP induced, invariant (or its reverse), followed by ADC acc,acc,x.
        // Only CMP's carry is observable until the counter update sets NZCV.
        if (body_words - i < 2)
            return false;
        const auto cmp = body[i++];
        const auto adc = body[i++];
        const unsigned lhs = BITS(cmp, 16, 19);
        plan::operand rhs;
        if ((cmp & 0xfdf0f000) != 0xe1500000 || lhs == plan::no_register
            || !decode_unshifted_arm_operand(cmp, rhs)
            || ((lhs == loop.induction) == (rhs.reg == loop.induction)))
            return false;
        loop.induction_is_left = lhs == loop.induction;
        loop.comparison = loop.induction_is_left ? rhs : plan::operand{ 0, lhs };
        mark_invariant(loop.comparison);

        loop.accumulator = BITS(adc, 12, 15);
        if ((adc & 0xfdf00000) != 0xe0a00000 || BITS(adc, 16, 19) != loop.accumulator
            || loop.accumulator == plan::no_register || loop.accumulator == loop.induction
            || loop.accumulator == loop.counter || !decode_unshifted_arm_operand(adc, loop.carry_operand))
            return false;
        mark_invariant(loop.carry_operand);

        if (body_words % i)
            return false;
        for (unsigned word = i; word < body_words; ++word) {
            if (body[word] != body[word % i])
                return false;
        }
        loop.repetitions = body_words / i;
    }

    const std::uint32_t writes = (1u << loop.induction) | (1u << loop.counter) | (1u << loop.accumulator);
    return !(invariant_registers & writes);
}

static std::uint32_t run_arm_counted_loop(ARMul_State *cpu, const arm_counted_loop &loop,
    std::uint32_t budget) {
    auto &regs = cpu->Reg;
    const auto counter = regs[loop.counter];
    const auto count = std::min(budget / loop.instruction_count, counter / loop.decrement);
    if (count < 2 || counter % loop.decrement)
        return 0;

    auto delta = loop.delta_constant;
    for (unsigned r = 0; r < arm_counted_loop::no_register; ++r)
        delta += regs[r] * static_cast<std::int32_t>(loop.delta_coeff[r]);

    auto induced = regs[loop.induction];
    if (loop.accumulator == arm_counted_loop::no_register) {
        induced += delta * count;
    } else {
        const auto compare = loop.comparison.value(regs);
        const auto steps = count * loop.repetitions;
        auto carries = std::uint32_t{0};
        if (loop.induction_is_left) {
            for (unsigned i = 0; i < steps; ++i) {
                induced += delta;
                carries += induced >= compare;
            }
        } else {
            for (unsigned i = 0; i < steps; ++i) {
                induced += delta;
                carries += compare >= induced;
            }
        }
        regs[loop.accumulator] += steps * loop.carry_operand.value(regs) + carries;
    }
    regs[loop.induction] = induced;

    bool carry, overflow;
    const auto before_last = counter - (count - 1) * loop.decrement;
    const auto after_last = AddWithCarry(before_last, ~loop.decrement, 1, &carry, &overflow);
    regs[loop.counter] = after_last;
    cpu->NFlag = after_last >> 31;
    cpu->ZFlag = after_last == 0;
    cpu->CFlag = carry;
    cpu->VFlag = overflow;
    if (after_last == 0)
        regs[15] += loop.instruction_count * 4;
    return count * loop.instruction_count;
}
