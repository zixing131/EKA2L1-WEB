#!/usr/bin/env python3
"""Execute the actual ROM export trampoline bytes with Unicorn ARM emulation.

Requires a C++17 compiler and Python unicorn. Checks both Thumb alignments,
ARM/Thumb targets and returns, stack arguments, register preservation and size.
"""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm_const import (
    UC_ARM_REG_R0, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_CPSR)

ROOT = Path(__file__).resolve().parents[1]


class ExportTrampolineTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        source = Path(cls.temp.name) / 'emit.cpp'
        source.write_text(r'''
#include <kernel/trampoline.h>
#include <cstdio>
int main() {
    for (unsigned entry : {0x1000u, 0x1001u, 0x1003u}) {
        for (unsigned target : {0x2000u, 0x2001u}) {
            unsigned char bytes[32];
            std::memset(bytes, 0xA5, sizeof(bytes));
            const auto size = eka2l1::hle::export_trampoline_size(entry);
            eka2l1::hle::write_export_trampoline(bytes + 4, entry, target);
            for (unsigned i = 0; i < 4; ++i) if (bytes[i] != 0xA5) return 1;
            for (unsigned i = 4 + size; i < sizeof(bytes); ++i) if (bytes[i] != 0xA5) return 2;
            std::printf("%x %x %zu ", entry, target, size);
            for (unsigned i = 0; i < size; ++i) std::printf("%02x", bytes[4 + i]);
            std::puts("");
        }
    }
}
'''.replace('#include <cstdio>', '#include <cstdio>\n#include <initializer_list>'))
        binary = Path(cls.temp.name) / 'emit'
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17',
                        '-I' + str(ROOT / 'src/emu/kernel/include'),
                        str(source), '-o', str(binary)], check=True)
        cls.cases = subprocess.check_output([str(binary)], text=True).splitlines()

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_calling_convention(self):
        for case in self.cases:
            entry, target, size, code = case.split()
            entry, target, size = int(entry, 16), int(target, 16), int(size)
            for thumb_return in (False, True):
                with self.subTest(entry=hex(entry), target=hex(target), thumb_return=thumb_return):
                    cpu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
                    cpu.mem_map(0x1000, 0x5000)
                    cpu.mem_write(entry & ~1, bytes.fromhex(code))
                    cpu.mem_write(0x2000, b'\x70\x47' if target & 1 else b'\x1e\xff\x2f\xe1')
                    regs = [0xAB000000 + n for n in range(13)]
                    for n, value in enumerate(regs):
                        cpu.reg_write(UC_ARM_REG_R0 + n, value)
                    cpu.reg_write(UC_ARM_REG_SP, 0x5000)
                    cpu.reg_write(UC_ARM_REG_LR, 0x3000 | thumb_return)
                    stack = struct.pack('<8I', *range(0x12340000, 0x12340008))
                    cpu.mem_write(0x5000, stack)
                    reached = []

                    def inspect(uc, pc, length, user):
                        if pc in (0x2000, 0x3000):
                            self.assertEqual([uc.reg_read(UC_ARM_REG_R0 + n) for n in range(13)], regs)
                            self.assertEqual(uc.reg_read(UC_ARM_REG_SP), 0x5000)
                            self.assertEqual(uc.reg_read(UC_ARM_REG_LR), 0x3000 | thumb_return)
                            self.assertEqual(bytes(uc.mem_read(0x5000, len(stack))), stack)
                            expected_thumb = bool(target & 1) if pc == 0x2000 else thumb_return
                            self.assertEqual(bool(uc.reg_read(UC_ARM_REG_CPSR) & 0x20), expected_thumb)
                            reached.append(pc)
                            if pc == 0x3000:
                                uc.emu_stop()

                    cpu.hook_add(UC_HOOK_CODE, inspect)
                    cpu.emu_start(entry, 0x4000, count=16)
                    self.assertEqual(reached, [0x2000, 0x3000])
                    self.assertEqual(size, {0x1000: 8, 0x1001: 12, 0x1003: 14}[entry])


if __name__ == '__main__':
    unittest.main()
