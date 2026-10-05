TriCore System emulator
=======================

Minimal TC397B machine
----------------------

``KIT_AURIX_TC397B_TRB`` provides a single-core functional subset for
freestanding firmware development. It uses the ``tc397`` CPU model, based on
TriCore 1.6.2, with maskable interrupt entry/return and WAIT support. The
existing ``tc27x`` and ``tc37x`` CPU models retain their previous behavior.

The implemented address map is:

========================== ============ ================================
Device                     Address      Extent / local alias
========================== ============ ================================
CPU0 DSPR                  0x70000000   240 KiB; alias 0xd0000000
CPU0 PSPR                  0x70100000   64 KiB; alias 0xc0000000
PFLASH0 (read-only)         0x80000000   3 MiB; alias 0xa0000000
ASCLIN0 UART               0xf0000600   0x100 bytes
ASCLIN1 UART               0xf0000700   0x100 bytes
ASCLIN2 UART               0xf0000800   0x100 bytes
STM0                       0xf0001000   0x100 bytes; fixed 50 MHz
Interrupt router status    0xf0037000   0x1000 bytes
Service request registers  0xf0038000   0x1000-byte window; 512 sources
PORT0 GPIO                 0xf003a000   0x100 bytes; 16 functional pins
CAN0 message RAM           0xf0200000   32 KiB
CAN0 module/node0          0xf0208000   0x400-byte functional subset
========================== ============ ================================

ASCLIN0 TX/RX/error use SRC indices 0x14/0x15/0x16; ASCLIN1 uses
0x17/0x18/0x19; ASCLIN2 uses 0x1a/0x1b/0x1c. The first three
``-serial`` backends are ASCLIN0, ASCLIN1 and ASCLIN2 respectively,
allowing independent AP, shell and Safety Island links.
STM0 SR0/SR1 use
0xc0/0xc1. Interrupt routing supports CPU0 only, selects the highest enabled
nonzero priority, and uses the lowest SRC index to break equal-priority ties.
UART transmission has no wire-level baud timing. STM uses QEMU virtual time;
this machine does not model oscillator or PLL startup.

Boot a TriCore ELF directly::

  qemu-system-tricore -M KIT_AURIX_TC397B_TRB -display none \
      -monitor none -serial stdio -kernel firmware.elf

Firmware must initialize its own stack, CSA free list, BIV, BTV and ISP.
The ELF entry point is restored on machine reset, including QMP
``system_reset``. Without an ELF, reset starts at 0xa0000000. A freestanding
functional probe and compiler instructions are available in
``tests/tcg/tricore/tc397/``.

PORT0 GPIO link
---------------

PORT0 implements OUT (offset 0x00), OMR (0x04), IOCR0/4/8/12
(0x10/0x14/0x18/0x1c) and IN (0x24), with aligned 32-bit accesses.
OMR supports set, clear and simultaneous set/clear (toggle). Each IOCR
byte supports floating input (0x00), pull-down (0x08), pull-up (0x10),
general-purpose push-pull output (0x80) and open-drain output (0xc0).
Other modes retain register values but do not drive a pin. IN reports
external levels or input pull state; enabled output drivers take precedence.
Floating, externally undriven inputs read low. Pad drive strength, analog
behavior, pin mux alternatives, emergency stop and GPIO interrupts are absent.

An optional independent chardev carries pin levels between processes::

  -chardev socket,id=tc397gpio,host=127.0.0.1,port=12345,server=on,wait=off \
  -global tricore-port.chardev=tc397gpio

Frames are eight bytes: ``G``, ``P``, version 1, direction ``O`` (output)
or ``I`` (input), uint16 little-endian levels, uint16 little-endian drive
mask. Input snapshots replace the external levels and mask. Output snapshots
carry the actual driven levels and mask, independent of every ASCLIN UART.
The bounded parser accepts split frames and resynchronizes invalid headers.
The model sends a snapshot on connection, output changes, reset and every
100 ms of host realtime. Periodic snapshots represent transport liveness,
not firmware heartbeat; output levels persist while guest execution is paused.
Output backpressure preserves a partially transmitted frame and coalesces
subsequent changes into the latest full snapshot. This is a level transport,
not an edge timing or physical wire simulation.

Disconnect releases external input drives. Machine reset clears OUT/IOCR
and releases output drivers; connected external input levels persist.
Reconnect immediately sends the current output snapshot. Without a chardev,
the port remains usable through MMIO and QEMU ``pin-in``/``pin-out`` GPIO
lines, with no periodic transport activity. Socket transport does not invoke SoC reset
itself; the receiving platform owns the meaning of connected pins.

Exercise all three UARTs and PORT0 using bounded qtest traffic::

  python3 tests/tcg/tricore/tc397/verify-peripherals.py \
      --qemu /path/to/qemu-system-tricore --output /path/to/evidence

The report distinguishes this device-level check from guest boot. The checks
include input/output traffic, SRC requests, GPIO direction, OMR behavior,
fragmented frames, backpressure, disconnect/reconnect and QMP reset.

CAN0 node0 and external CAN transport
------------------------------------

CAN0 node0 implements a functional Bosch M_CAN 3.2 register subset, with
the TC3x integration registers at their Infineon addresses. Module CLC is
at 0xf0208000, MCR at 0xf0208030, node STARTADR/ENDADR at 0xf0208108/0xf020810c,
GRINT1/GRINT2 at 0xf0208114/0xf0208118, and the M_CAN core at 0xf0208200.
The sixteen CAN0 interrupt outputs use SRC indices 0x16c through 0x17b.
The TC3x integration routes interrupt groups through GRINT; generic M_CAN
ILS/ILE registers are absent. A guest integration driver must adapt these
operations to GRINT and IE instead of accessing fictitious registers.

The subset supports classic CAN (DLC 0..8), CAN FD (up to 64 bytes),
standard and extended IDs, RTR for classic frames, BRS, standard/extended
filter lists, receive FIFO0/FIFO1, up to 32 pending transmit buffers and
the transmit event FIFO. Message RAM and FIFO accesses are bounded.
IR is write-one-to-clear; enabled pending events generate real TC397
service requests. Internal loopback is explicitly selected by CCCR.TEST
and TEST.LBCK and is separate from external delivery validation.

Attach a separate process using an optional chardev::

  -chardev socket,id=vmcucan,host=127.0.0.1,port=12346,server=on,wait=off \
  -global tc397-can.chardev=vmcucan

The CAN1 transport uses fixed 92-byte records, all multibyte values
little-endian:

=========== =============================================================
Byte offset Meaning
=========== =============================================================
0           Four bytes ``CAN1``
4           Type: TX=1, RX=2, ACK=3, CONTROL=4
5           Flags: IDE=1, RTR=2, FDF=4, BRS=8, ESI=16
6           DLC: classic 0..8, FD 0..15
7           Payload length: canonical DLC-to-length mapping; RTR is zero
8           uint32 controller epoch, nonzero
12          uint32 transaction token; nonzero for TX/ACK, zero for RX/CONTROL
16          uint32 CAN identifier, excluding flag bits
20          uint32 status: ACK transmitted=1/canceled=2/error=3/full=4;
            CONTROL start=1/stop=2; TX/RX zero
24          64-byte payload, unused bytes zero
88          CRC32 IEEE of bytes 0..87, reflected, initial/final XOR ffffffff
=========== =============================================================

An external bridge owns CAN network delivery. Enqueuing or writing a
socket record never completes a guest transmit. Only a matching successful
ACK emits TXBTO and a transmit event/interrupt. Controller reset, INIT
transitions and reconnection invalidate old epochs and pending tokens.
Malformed, stale and duplicate acknowledgements cannot complete a new
transmit. The stream parser and output queue have fixed capacities.

Negative ACK, disconnect and missing ACK produce functional bus-off/error
status and cancel pending requests. The default host-time acknowledgement
deadline is 5000 ms, configurable through ``tc397-can.ack-timeout-ms``.
This maps transport failure into a guest-visible failure; it does not model
physical CAN error confinement. An external SIL Kit adapter must send success
only from its actual frame-transmit callback and guard old callbacks across
connection and epoch changes.

Bit timing registers are retained, but this model does not emulate oscillator
timing, electrical arbitration, bit stuffing, error counters progressing
through real bus faults, TTCAN, dedicated receive buffers, CAN nodes1..3,
DMA, timestamp accuracy or complete access protection. STARTADR/ENDADR are
retained while all message accesses are checked against the physical 32 KiB
RAM. A bridge with fixed network bitrates must advertise that limitation.

Run the hardware functional tests using a mock peer::

  python3 tests/tcg/tricore/tc397/verify-can.py \
      --qemu /path/to/qemu-system-tricore --output /path/to/evidence

This test proves MMIO/IRQ/FIFO and acknowledgement semantics. Real guest
driver execution and actual SIL Kit participant delivery require separate
integration evidence.

Limitations
-----------

This is not a complete TC397 or TriBoard emulation. BootROM, UCB/BMHD, HSM,
flash programming, SCU/PLL, watchdogs, other CAN nodes, QSPI, I2C, Ethernet,
other GPIO, SMU,
PMIC, additional CPU cores, lockstep and MPU protection are not implemented.
There is no catch-all SFR model. Unimplemented addresses do not imply
working hardware. Migration and snapshots are unsupported.

Direct bare-metal ELF boot does not qualify vendor startup software,
AUTOSAR, Zephyr, hardware safety behavior, or physical timing. QMP machine
reset does not represent an SCU reset or a PMIC power cycle. Apollo QBox,
TriCore libqemu and SIL Kit connections are separate integration work.
