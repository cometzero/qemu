# TC397 CPU0 functional probe

This freestanding probe boots on `KIT_AURIX_TC397B_TRB` using the `tc397`
CPU. It deliberately has no Infineon startup, BootROM, MCAL or SCU dependency.
Its scope is the documented minimal model, not TC397 silicon qualification.
The ELF entry is `0xa0000100`, deliberately distinct from the default reset PC,
so both boots exercise ELF entry restoration.

Compile with the TriCore GCC 9.4 toolchain pinned by
`tests/docker/dockerfiles/debian-tricore-cross.docker`:

```sh
tricore-gcc -mtc162 -Os -ffreestanding -fno-builtin -nostdlib \
    -msmall=1 -mabs=1 -Wall -Wextra -Werror \
    -Wl,-T,tests/tcg/tricore/tc397/link.ld -Wl,--build-id=none \
    -o tc397-minimal.elf tests/tcg/tricore/tc397/boot.S \
    tests/tcg/tricore/tc397/firmware.c
qemu-system-tricore -M KIT_AURIX_TC397B_TRB -display none \
    -monitor none -serial stdio -kernel tc397-minimal.elf
```

The firmware initializes the CPU0 CSA, stack and interrupt vectors, then tests
RAM/flash aliases and flash write protection; an advancing 50 MHz STM counter;
global/source masks, read-only PIPN, CCPN threshold, SRPN order and
zero-priority exclusion;
five real STM compare interrupts; ASCLIN host RX, interrupt entry/return and
WAIT wake-up; and UART TX echo. When `TC397:READY:RX` appears, send one `V`
byte. The terminal must deliver the byte without waiting for a newline.

`TC397:DONE` leaves a pending disabled source and non-default peripheral state.
A QMP `system_reset` must boot again and pass reset-state checks and the whole
suite. The Apollo workspace runner `scripts/test/verify_qemu_tc397.py` builds
this probe, supplies the byte, issues QMP reset, checks both boots, and writes
bounded execution logs and JSON evidence. This directory remains separate from
the existing `tricore_testboard` ISA tests.
