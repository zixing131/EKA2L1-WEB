.syntax unified
.thumb
.section .text

.global patched_get_interface
.thumb_func
patched_get_interface:
    push {lr}
    cmp r1, #8
    beq interface_surface
    cmp r1, #6
    beq interface_orientation
    cmp r1, #5
    beq interface_fast_blit
    cmp r1, #1
    beq interface_scaling

interface_unsupported:
    movs r0, #5
    negs r0, r0
    pop {pc}

interface_orientation:
    adds r3, r0, #4
    b interface_ok

interface_fast_blit:
    adds r3, r0, #0
    adds r3, #20
    b interface_ok

interface_scaling:
    adds r3, r0, #0
    adds r3, #56
    b interface_ok

interface_surface:
    bl surface_extension
    pop {pc}

interface_ok:
    movs r0, #0
    str r3, [r2]
    pop {pc}

/* The original 32-bit screen-device diagnostic strings start 0x2b94 bytes
 * after GetInterface. The binary patch redirects that space to this
 * position-independent MSurfaceId implementation. */
.org 0x2b94

.global surface_extension
.thumb_func
surface_extension:
    push {r4, r5, lr}
    adds r5, r0, #0
    adr r3, surface_object
    adr r4, surface_vtable
    str r4, [r3]
    str r5, [r3, #4]
    movs r0, #1
    str r0, [r3, #8]

    adr r0, surface_get
    adds r0, #1
    str r0, [r4]

    adr r0, surface_orientations_available
    adds r0, #1
    str r0, [r4, #4]

    adr r0, surface_set_orientation
    adds r0, #1
    str r0, [r4, #8]

    adr r0, surface_device_orientation
    adds r0, #1
    str r0, [r4, #12]

    str r3, [r2]
    movs r0, #0
    pop {r4, r5, pc}

.align 2
.thumb_func
surface_get:
    movs r2, #0
    str r2, [r1]
    str r2, [r1, #4]
    ldr r2, surface_pixel_format
    str r2, [r1, #8]
    movs r2, #1
    lsls r2, r2, #24
    str r2, [r1, #12]
    bx lr

.align 2
.thumb_func
surface_orientations_available:
    movs r0, #15
    bx lr

.align 2
.thumb_func
surface_set_orientation:
    push {r4, r5, lr}
    adds r4, r0, #0
    adds r5, r1, #0
    cmp r1, #1
    beq surface_orientation_normal
    cmp r1, #2
    beq surface_orientation_90
    cmp r1, #4
    beq surface_orientation_180
    cmp r1, #8
    bne surface_orientation_invalid
    movs r1, #3
    b surface_orientation_apply

surface_orientation_normal:
    movs r1, #0
    b surface_orientation_apply

surface_orientation_90:
    movs r1, #1
    b surface_orientation_apply

surface_orientation_180:
    movs r1, #2

surface_orientation_apply:
    ldr r0, [r4, #4]
    str r1, [r0, #16]
    movs r0, #1
    str r5, [r4, #8]

surface_orientation_done:
    pop {r4, r5, pc}

surface_orientation_invalid:
    movs r0, #0
    b surface_orientation_done

.align 2
.thumb_func
surface_device_orientation:
    ldr r0, [r0, #8]
    bx lr

.align 2
surface_object:
    .space 12
surface_vtable:
    .space 16
surface_pixel_format:
    .word 0x1027549a
