Apollo application-processor Linux platform
==========================================

The ``apollo-qvp`` machine boots an arm64 Linux Image directly using the
application-processor physical addresses from the Apollo QBox platform.
It uses TCG with one to sixteen Cortex-A720AE CPUs (four by default),
native PSCI through SMC, and 125 MHz architectural timers. Linux starts
at non-secure EL2. QEMU generates a device tree for the implemented devices.

Implemented hardware
--------------------

======================= ======================= ===========
Device                  Address                 GIC SPI
======================= ======================= ===========
PL011 UART              0x1a400000               52
GICv3 distributor       0x20800000               --
GICv3 redistributors    0x20880000 + CPU*0x40000  --
virtio-mmio block       0x30020000               257
virtio-mmio network     0x30060000               261
virtio-mmio RNG         0x30080000               263
PL031 RTC               0x300d0000               268
======================= ======================= ===========

The three virtio transports are ``virtio-mmio-bus.0`` (block),
``virtio-mmio-bus.1`` (network), and ``virtio-mmio-bus.2`` (RNG).
Attach endpoints explicitly with ``-device`` and the corresponding ``bus``.
The first 2032 MiB of RAM start at 0x80000000. Remaining RAM starts at
0x20000000000; the maximum and default total RAM size is 4080 MiB.
The minimum RAM size is 256 MiB. Kernel, initrd, and DTB loading use the
contiguous low RAM bank.

Example::

  qemu-system-aarch64 -machine apollo-qvp -accel tcg -smp 4 -m 4080M \
    -kernel Image -initrd nexios-bsp-initramfs.cpio.gz \
    -append 'console=ttyAMA0 earlycon=pl011,0x1a400000 rdinit=/init' \
    -drive if=none,id=rootfs,file=disk.wic,format=raw \
    -device virtio-blk-device,drive=rootfs,bus=virtio-mmio-bus.0 \
    -netdev user,id=net0 \
    -device virtio-net-device,netdev=net0,bus=virtio-mmio-bus.1 \
    -device virtio-rng-device,bus=virtio-mmio-bus.2 -nographic

This Linux boot platform does not execute SI or RSE firmware and does not
implement their remoteproc, RPMsg, SCMI, PFDI, or secure services. The generated
DT omits these devices. It also omits PCIe/ITS, SMMU, platform DMA, I2C, SPI,
I2S, watchdog, and physical power/reset controllers. GIC multiview, firmware
boot sequences, cycle timing, and physical hardware parity are not modeled.
An externally supplied ``-dtb`` must describe this subset; the deployed
full-platform Apollo DTB is not suitable without adjustment.
