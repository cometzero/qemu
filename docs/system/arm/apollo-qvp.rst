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
DW_apb_i2s 0/1          0x30200000 / 0x30210000  356 / 357
Arm DMA-350 0/1         0x31000000 / 0x31010000  279 / 358
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
DT omits these devices. It also omits PCIe/ITS, SMMU, I2C, SPI,
watchdog, and physical power/reset controllers. GIC multiview, firmware
boot sequences, cycle timing, and physical hardware parity are not modeled.
An externally supplied ``-dtb`` must describe this subset; the deployed
full-platform Apollo DTB is not suitable without adjustment.

I2S loopback and DMA
--------------------

Two native QEMU DW_apb_i2s devices are connected bidirectionally, using the
register/FIFO behavior of the QBox SystemC models. Controller 0 is the clock
master; controller 1 is the slave. Each has one stereo lane and 16-frame
FIFOs. DMA-350 controller 1 connects requests 0/1 to I2S0 TX/RX and requests
2/3 to I2S1 TX/RX. Each DMA controller has eight channels, with their IRQs
combined onto the SPI listed above. Controller 0 has no peripheral requests
connected in this machine. The generated DT follows Linux apollo-qvp.dts,
including the 1.536 MHz fixed clock and playback/capture simple-audio-card
links; it omits pinctrl because the standalone machine has fixed routes.

The default DT selects DMAengine PCM. ``-machine apollo-qvp,i2s-dma=off``
omits the I2S DMA properties to select the same Linux driver's interrupt-driven
PIO path. Both DMA controllers remain present. This option affects only the
machine-generated DT, not an external ``-dtb``.

Audio uses QBox's functional pacing: empty TX waits for data and a full active
peer RX FIFO backpressures transmission. This supports sample integrity tests,
not physical I2S underrun/overrun timing or real-time clock qualification.
The default frame period is 20,833 ns. No host audio backend is required.
Hardware command links, security attribution, cycle accuracy and migration
are outside the supported model contract. Linux cyclic DMA uses period IRQs
and software channel rearming, matching the current QBox model.
