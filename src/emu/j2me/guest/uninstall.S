// SPDX-License-Identifier: GPL-3.0-or-later
// Position-independent ARM bootstrap for the ROM SWInst uninstall API.
// Assembled words are embedded by run_guest_swinst() in interface.cpp.
// For SilentInstall the host replaces uid with a filename descriptor,
// replaces options with TInstallOptionsPckg and omits the MIME argument.
// clang -target armv6-none-eabi -c uninstall.S -o uninstall.o
// llvm-objcopy -O binary uninstall.o uninstall.bin
// llvm-nm -n uninstall.o lists the import/data offsets used by the host.
.syntax unified
.arm
.Lstart:
 sub sp, sp, #128
 mov r0, #0
 ldr r12, heap
 blx r12
 cmp r0, #0
 bne finish
 ldr r12, init
 blx r12
 ldr r12, cleanup_new
 blx r12
 cmp r0, #0
 mvneq r0, #3
 beq finish
 mov r6, r0
 mov r0, sp
 ldr r12, ctor
 blx r12
 mov r0, sp
 ldr r12, connect
 blx r12
 cmp r0, #0
 bne finish
 mov r0, sp
 adr r1, uid
 adr r2, options
 adr r3, mime
 ldr r12, uninstall
 blx r12
 mov r4, r0
 mov r0, sp
 ldr r12, close
 blx r12
 mov r0, r6
 ldr r12, cleanup_delete
 blx r12
 mov r0, r4
finish:
 ldr r12, exit
 blx r12
 b .
heap: .word 0
init: .word 0
ctor: .word 0
connect: .word 0
uninstall: .word 0
close: .word 0
exit: .word 0
cleanup_new: .word 0
cleanup_delete: .word 0
uid: .word 0
options: .word 8, 0, 1
mime: .word 24
.asciz "application/java-archive"
.balign 4
