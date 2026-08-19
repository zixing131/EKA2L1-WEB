#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace eka2l1::hle {
    struct j9_jni_symbol {
        std::string name;
        std::uint32_t fn = 0;
    };

    // Walk a {name_ptr, fn_ptr} table in a mapped codeseg image. name_ptr/fn
    // must both fall inside [run_addr, run_addr+bytes). Used at codeseg attach
    // and by host tests — same pairing rules as the emulator.
    inline std::size_t j9_scan_jni_export_table(const std::uint8_t *mem, std::uint32_t run_addr,
        std::uint32_t bytes, std::vector<j9_jni_symbol> &out) {
        if (!mem || (bytes < 12)) {
            return 0;
        }
        const std::uint32_t lo = run_addr;
        const std::uint32_t hi = run_addr + bytes;
        std::size_t added = 0;
        for (std::uint32_t off = 0; off + 8 <= bytes; off += 4) {
            std::uint32_t name_ga = 0;
            std::uint32_t fn_ga = 0;
            std::memcpy(&name_ga, mem + off, 4);
            std::memcpy(&fn_ga, mem + off + 4, 4);
            if ((name_ga < lo) || (name_ga + 8 >= hi) || (fn_ga < lo) || (fn_ga >= hi)) {
                continue;
            }
            const char *s = reinterpret_cast<const char *>(mem + (name_ga - run_addr));
            if (!s || (std::memcmp(s, "Java_", 5) != 0)) {
                continue;
            }
            std::size_t n = 0;
            while ((s[n] != 0) && (n < 200) && ((name_ga - run_addr + n) < bytes)) {
                ++n;
            }
            if (n < 8) {
                continue;
            }
            j9_jni_symbol ent;
            ent.name.assign(s, n);
            ent.fn = fn_ga | 1u;
            out.push_back(std::move(ent));
            ++added;
        }
        return added;
    }

    inline std::uint32_t j9_lookup_jni_symbol(const std::vector<j9_jni_symbol> &table, const char *name) {
        if (!name || !name[0]) {
            return 0;
        }
        for (const auto &ent : table) {
            if (ent.name == name) {
                return ent.fn;
            }
        }
        const std::size_t nlen = std::strlen(name);
        if ((nlen >= 3) && (nlen < 80) && ((nlen < 5) || (std::memcmp(name, "Java_", 5) != 0))) {
            for (const auto &ent : table) {
                if ((ent.name.size() > nlen) && (ent.name.compare(ent.name.size() - nlen, nlen, name) == 0)
                    && (ent.name[ent.name.size() - nlen - 1] == '_')) {
                    return ent.fn;
                }
            }
            if (name[0] == '_') {
                const std::string escaped = std::string("__1") + (name + 1);
                for (const auto &ent : table) {
                    if ((ent.name.size() > escaped.size())
                        && (ent.name.compare(ent.name.size() - escaped.size(), escaped.size(), escaped) == 0)) {
                        return ent.fn;
                    }
                }
            } else {
                const std::string escaped = std::string("__1") + name;
                for (const auto &ent : table) {
                    if ((ent.name.size() > escaped.size())
                        && (ent.name.compare(ent.name.size() - escaped.size(), escaped.size(), escaped) == 0)) {
                        return ent.fn;
                    }
                }
            }
        }
        return 0;
    }

    // ARM guest sl_lookup stand-in for the JXE-only call site.
    // Thumb stub has `push {r0}` so [sp+0] is portLib and [sp+4] is func_out.
    // Hit: *func_out = fn|1, drop saved port, r0 = 0, return to BLX.
    // Miss: restore port into r0 and tail to orig_wrap (Thumb).
    inline std::vector<std::uint32_t> j9_emit_jni_walker(std::uint32_t pairs_va, std::uint32_t orig_wrap) {
        std::vector<std::uint32_t> w;
        enum L : int {
            L_loop,
            L_t1,
            L_try5,
            L_t2,
            L_tryq,
            L_t3,
            L_next,
            L_found,
            L_pairs,
            L_orig,
            L_n
        };
        int lab[L_n];
        for (int i = 0; i < L_n; ++i) {
            lab[i] = -1;
        }
        struct Fix {
            std::size_t at;
            L to;
            int kind; // 0=b, 1=bne, 2=beq, 3=ldr r4 lit, 4=ldr pc lit
        };
        std::vector<Fix> fixes;
        auto emit = [&](std::uint32_t x) { w.push_back(x); };
        auto mark = [&](L l) { lab[l] = static_cast<int>(w.size()); };
        auto b = [&](L to) {
            fixes.push_back({ w.size(), to, 0 });
            emit(0);
        };
        auto bne = [&](L to) {
            fixes.push_back({ w.size(), to, 1 });
            emit(0);
        };

        emit(0xE92D41FF); // push {r0-r8, lr}
        fixes.push_back({ w.size(), L_pairs, 3 });
        emit(0); // ldr r4, pairs
        emit(0xE5145004); // ldr r5, [r4, #-4]
        emit(0xE59D802C); // ldr r8, [sp, #44]  ; Thumb-saved port then func_out

        mark(L_loop);
        emit(0xE5946000); // ldr r6, [r4]
        emit(0xE1A07002); // mov r7, r2
        mark(L_t1);
        emit(0xE4D70001); // ldrb r0, [r7], #1
        emit(0xE4D61001); // ldrb r1, [r6], #1
        emit(0xE1500001); // cmp r0, r1
        bne(L_try5);
        emit(0xE3500000); // cmp r0, #0
        bne(L_t1);
        b(L_found);

        mark(L_try5);
        emit(0xE5946000); // ldr r6, [r4]
        emit(0xE5D60000); // ldrb r0, [r6]
        emit(0xE350004A); // cmp r0, #'J'
        bne(L_tryq);
        emit(0xE5D60001); // ldrb r0, [r6, #1]
        emit(0xE3500061); // cmp r0, #'a'
        bne(L_tryq);
        emit(0xE2866005); // add r6, r6, #5
        emit(0xE1A07002); // mov r7, r2
        mark(L_t2);
        emit(0xE4D70001); // ldrb r0, [r7], #1
        emit(0xE4D61001); // ldrb r1, [r6], #1
        emit(0xE1500001); // cmp r0, r1
        bne(L_tryq);
        emit(0xE3500000); // cmp r0, #0
        bne(L_t2);
        b(L_found);

        // Query may be Java_* while the table has the unprefixed JCL name
        // (java_lang_Object_getClass).
        mark(L_tryq);
        emit(0xE5D20000); // ldrb r0, [r2]
        emit(0xE350004A); // cmp r0, #'J'
        bne(L_next);
        emit(0xE5D20001); // ldrb r0, [r2, #1]
        emit(0xE3500061); // cmp r0, #'a'
        bne(L_next);
        emit(0xE5946000); // ldr r6, [r4]
        emit(0xE2827005); // add r7, r2, #5
        mark(L_t3);
        emit(0xE4D70001); // ldrb r0, [r7], #1
        emit(0xE4D61001); // ldrb r1, [r6], #1
        emit(0xE1500001); // cmp r0, r1
        bne(L_next);
        emit(0xE3500000); // cmp r0, #0
        bne(L_t3);
        b(L_found);

        mark(L_next);
        emit(0xE2844008); // add r4, r4, #8
        emit(0xE2555001); // subs r5, r5, #1
        bne(L_loop);
        emit(0xE1200070); // bkpt #0 — host logs the miss name in r2
        emit(0xE8BD41FF); // pop {r0-r8, lr}
        emit(0xE49D0004); // pop r0  ; port
        fixes.push_back({ w.size(), L_orig, 4 });
        emit(0); // ldr pc, orig_wrap|1

        mark(L_found);
        emit(0xE1200070); // bkpt #0 — host logs the hit name/fn
        emit(0xE5940004); // ldr r0, [r4, #4]  ; keep ARM/Thumb bit
        emit(0xE3580000); // cmp r8, #0
        emit(0x15880000); // strne r0, [r8]
        emit(0xE8BD41FF); // pop {r0-r8, lr}
        emit(0xE28DD004); // add sp, #4
        emit(0xE3A00000); // mov r0, #0
        emit(0xE12FFF1E); // bx lr

        if ((w.size() & 1u) != 0) {
            emit(0);
        }
        mark(L_pairs);
        emit(pairs_va);
        mark(L_orig);
        emit(orig_wrap | 1u);

        for (const Fix &f : fixes) {
            const int to = lab[f.to];
            const int from = static_cast<int>(f.at);
            if ((to < 0) || (from < 0)) {
                w.clear();
                return w;
            }
            if ((f.kind == 3) || (f.kind == 4)) {
                const int imm = to * 4 - from * 4 - 8;
                if ((imm < 0) || (imm > 0xFFF)) {
                    w.clear();
                    return w;
                }
                const std::uint32_t rd = (f.kind == 3) ? 4u : 15u;
                w[static_cast<std::size_t>(from)] = 0xE59F0000u | (rd << 12) | static_cast<std::uint32_t>(imm);
            } else {
                const int imm24 = to - from - 2;
                const std::uint32_t cond = (f.kind == 0) ? 0xEAu : ((f.kind == 1) ? 0x1Au : 0x0Au);
                w[static_cast<std::size_t>(from)] = (cond << 24) | (static_cast<std::uint32_t>(imm24) & 0x00FFFFFFu);
            }
        }
        return w;
    }
}
