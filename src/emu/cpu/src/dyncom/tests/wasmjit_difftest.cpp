/* Copyright (c) 2026 EKA2L1 Team.
 * Actual ARM/Thumb -> WASM differential tests, run by Node or a browser.
 * Reuse the host harness's flat-memory fixture and independent golden model.
 */
#define EKA2L1_DYNCOM_DIFFTEST_FIXTURES_ONLY
#include "dyncom_difftest.cpp"
#include <cpu/dyncom/arm_dyncom_jit.h>
#include <algorithm>
#include <cstdlib>

namespace {
constexpr std::uint32_t code_base = 0x1000;
constexpr std::uint32_t return_pc = 0x2000;

bool run_alu_case(std::uint32_t arm_inst, std::uint16_t thumb_inst, std::mt19937 &rng,
    diff_env &interp_env, diff_env &jit_env, dyncom_core &interp, dyncom_core &jit,
    std::uint64_t &checked) {
    const bool thumb = thumb_inst != 0;
    const std::uint32_t code = thumb ? (std::uint32_t(0x4770) << 16) | thumb_inst : arm_inst;
    const std::uint32_t bx_lr = 0xE12FFF1E;
    for (auto *env : {&interp_env, &jit_env}) {
        std::memcpy(env->mem.data() + code_base, &code, 4);
        std::memcpy(env->mem.data() + code_base + 4, &bx_lr, 4);
    }
    interp.clear_instruction_cache();
    jit.clear_instruction_cache();
    const auto compiled_before = dyncom_jit::stat_compiled;
    constexpr std::uint32_t edges[] = {0, 1, 2, 0x7FFFFFFE, 0x7FFFFFFF,
        0x80000000, 0x80000001, 0xFFFFFFFE, 0xFFFFFFFF};
    // Warm the exact block, then exercise all NZCV combinations, boundary pairs
    // and random operands. A test that never enters JIT is an explicit failure.
    for (unsigned i = 0; i < 32 + 16 * 81 + 128; ++i) {
        cpu_state input{};
        for (auto &reg : input.reg) reg = rng();
        const unsigned j = i >= 32 ? i - 32 : i;
        if (j < 16 * 81) {
            input.reg[1] = edges[(j / 9) % 9];
            input.reg[2] = edges[j % 9];
        }
        input.reg[14] = return_pc | (thumb ? 1 : 0);
        input.reg[15] = code_base;
        input.cpsr = ((j / 81) % 16 << 28) | (thumb ? 0x30 : 0x10);
        write_state(interp, input);
        write_state(jit, input);
        dyncom_jit::enabled_default = 0;
        interp.run(2);
        dyncom_jit::enabled_default = 1;
        const auto jit_instrs_before = dyncom_jit::stat_jit_instrs;
        jit.run(2);
        auto expected = golden_data_processing(arm_inst, input);
        expected.reg[15] = return_pc;
        const auto actual = read_state(jit);
        const auto interpreted = read_state(interp);
        if (!(interpreted == expected)) {
            report_mismatch("WASM harness interpreter ALU", arm_inst, i, expected, interpreted);
            return false;
        }
        if (!(actual == expected)) {
            report_mismatch("WASM JIT ALU", arm_inst, i, expected, actual);
            return false;
        }
        if (i >= 32 && dyncom_jit::stat_jit_instrs == jit_instrs_before) {
            std::printf("FAIL: instruction %08X did not execute compiled WASM\n", arm_inst);
            return false;
        }
        ++checked;
    }
    if (dyncom_jit::stat_compiled == compiled_before) {
        std::printf("FAIL: instruction %08X never compiled\n", arm_inst);
        return false;
    }
    return true;
}

cpu_state benchmark(dyncom_core &core, diff_env &env, bool jit, double &median_ms) {
    // Flag-heavy, data-dependent ALU loop. Consume carry with ADC so producing
    // flags is observable, and finish via an interpreter SVC stop callback.
    std::vector<std::uint32_t> code;
    for (unsigned i = 0; i < 8; ++i) {
        code.push_back(0xE0900001); // ADDS r0,r0,r1
        code.push_back(0xE0500002); // SUBS r0,r0,r2
        code.push_back(0xE1530000); // CMP r3,r0
        code.push_back(0xE2A55000); // ADC r5,r5,#0
    }
    code.push_back(0xE2588001); // SUBS r8,r8,#1
    const std::int32_t branch_words = -static_cast<std::int32_t>(code.size()) - 2;
    code.push_back(0x1A000000 | (static_cast<std::uint32_t>(branch_words) & 0xFFFFFF));
    code.push_back(0xEF000000); // SVC #0
    std::memcpy(env.mem.data() + code_base, code.data(), code.size() * 4);
    core.clear_instruction_cache();
    bool finished = false;
    core.system_call_handler = [&](std::uint32_t) { finished = true; core.stop(); };
    dyncom_jit::enabled_default = jit;
    cpu_state last{};
    std::vector<double> samples;
    for (unsigned sample = 0; sample < 8; ++sample) {
        cpu_state input{};
        input.reg[0] = 0x71234567;
        input.reg[1] = 0x60000001;
        input.reg[2] = 0x90000003;
        input.reg[3] = 0x80000000;
        input.reg[8] = 100000;
        input.reg[15] = code_base;
        input.cpsr = 0x10;
        write_state(core, input);
        finished = false;
        const auto start = std::chrono::steady_clock::now();
        unsigned watchdog = 0;
        while (!finished && ++watchdog < 100000) core.run(65536);
        if (!finished) { std::printf("FAIL: benchmark failed to stop\n"); std::exit(1); }
        const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (sample) samples.push_back(elapsed);
        last = read_state(core);
    }
    std::sort(samples.begin(), samples.end());
    median_ms = samples[samples.size() / 2];
    core.system_call_handler = [](std::uint32_t) {};
    return last;
}
}

int main() {
    diff_env interp_env, jit_env;
    auto interp = make_core(interp_env), jit = make_core(jit_env);
    std::mt19937 rng(0xA32);
    std::uint64_t checked = 0;
    for (unsigned opcode : {2, 3, 4, 5, 6, 7, 10, 11}) {
        for (unsigned cond = 0; cond < 15; ++cond) {
            for (unsigned s = 0; s < 2; ++s) {
                if (opcode >= 10 && !s) continue;
                const auto base = (cond << 28) | (opcode << 21) | (s << 20);
                // Independent operands, destination aliases, equal operands,
                // immediate rotations, and the supported immediate shifts.
                for (std::uint32_t operands : {
                         0x10002u, 0x11002u, 0x12002u, 0x20002u,
                         0x10082u, 0x10F82u, // LSL #1/#31
                         0x10022u, 0x100A2u, 0x10FA2u, // LSR #32/#1/#31
                         0x10042u, 0x100C2u, 0x10FC2u, // ASR #32/#1/#31
                         0x2010000u, 0x2010001u, 0x20104FFu, 0x2010102u}) {
                    if (opcode >= 10 && (operands & 0xF000)) continue; // CMP/CMN require Rd == 0.
                    const auto inst = base | operands;
                    if (!run_alu_case(inst, 0, rng, interp_env, jit_env, *interp, *jit, checked)) return 1;
                }
            }
        }
    }
    // Thumb instructions normalized into the same ARM data-processing stream.
    for (auto pair : {std::pair<std::uint32_t, std::uint16_t>{0xE0910002, 0x1888},
             {0xE0510002, 0x1A88}, {0xE1510002, 0x4291}, {0xE1710002, 0x42D1}}) {
        if (!run_alu_case(pair.first, pair.second, rng, interp_env, jit_env, *interp, *jit, checked)) return 1;
    }
    double interp_ms = 0, jit_ms = 0;
    const auto interp_result = benchmark(*interp, interp_env, false, interp_ms);
    const auto jit_result = benchmark(*jit, jit_env, true, jit_ms);
    if (!(interp_result == jit_result)) {
        report_mismatch("WASM JIT loop benchmark", 0, 0, interp_result, jit_result);
        return 1;
    }
    std::printf("PASS: %llu ARM/Thumb cases (JIT, interpreter, independent golden flags)\n",
        static_cast<unsigned long long>(checked));
    std::printf("BENCH: 3.4M ARM instructions, median of 7 warm runs: interpreter=%.3fms JIT=%.3fms speedup=%.2fx\n", interp_ms, jit_ms, interp_ms / jit_ms);
    return 0;
}
