/*
 * TC397 CAN0 node0, functional Bosch M_CAN subset with external CAN transport.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Register source: Infineon TC39x-B IfxCan_reg{,def}.h, iLLD
 * ac8fb805633894b89819b953516b4e94387056fd. No bit-level arbitration model.
 * Only an external transmit acknowledgement produces a successful TX event.
 */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/net/tc397_can.h"
#include "chardev/char-fe.h"
#include "migration/vmstate.h"
#include "net/can_emu.h"
#include "qapi/error.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/timer.h"

OBJECT_DECLARE_SIMPLE_TYPE(TC397CanState, TC397_CAN)

enum {
    CLC = 0x000, MCR = 0x030, STARTADR = 0x108, ENDADR = 0x10c,
    GRINT1 = 0x114, GRINT2 = 0x118, NPCR = 0x140,
    CREL = 0x200, ENDN = 0x204, DBTP = 0x20c, TEST = 0x210,
    CCCR = 0x218, NBTP = 0x21c, TSCC = 0x220, TSCV = 0x224,
    ECR = 0x240, PSR = 0x244, TDCR = 0x248, IR = 0x250, IE = 0x254,
    GFC = 0x280, SIDFC = 0x284, XIDFC = 0x288, XIDAM = 0x290,
    RXF0C = 0x2a0, RXF0S = 0x2a4, RXF0A = 0x2a8, RXBC = 0x2ac,
    RXF1C = 0x2b0, RXF1S = 0x2b4, RXF1A = 0x2b8, RXESC = 0x2bc,
    TXBC = 0x2c0, TXFQS = 0x2c4, TXESC = 0x2c8, TXBRP = 0x2cc,
    TXBAR = 0x2d0, TXBCR = 0x2d4, TXBTO = 0x2d8, TXBCF = 0x2dc,
    TXBTIE = 0x2e0, TXBCIE = 0x2e4, TXEFC = 0x2f0,
    TXEFS = 0x2f4, TXEFA = 0x2f8,
};

#define R(s, reg) ((s)->regs[(reg) / 4])
#define RAM_SIZE 32768
#define WIRE_SIZE 92
#define WIRE_QUEUE 64
#define IR_MRAF BIT(17)
#define IR_BO BIT(25)
#define IR_TEFN BIT(12)
#define IR_TEFL BIT(15)
#define CCCR_INIT BIT(0)
#define CCCR_CCE BIT(1)
#define CCCR_MON BIT(5)
#define CCCR_TEST BIT(7)
#define CCCR_FDOE BIT(8)
#define CCCR_BRSE BIT(9)
#define W_IDE BIT(0)
#define W_RTR BIT(1)
#define W_FDF BIT(2)
#define W_BRS BIT(3)
#define W_ESI BIT(4)

struct TC397CanState {
    SysBusDevice parent_obj;
    MemoryRegion regs_region, ram_region;
    uint8_t *ram;
    uint32_t regs[0x400 / 4];
    qemu_irq irq[16];
    CharFrontend chr;
    uint32_t epoch, next_token, tokens[32], tx_header[32][2];
    int64_t deadlines[32];
    uint8_t rx_get[2], rx_put[2], rx_fill[2];
    uint8_t event_get, event_put, event_fill;
    uint8_t input[WIRE_SIZE], queue[WIRE_QUEUE][WIRE_SIZE];
    unsigned input_used, queue_get, queue_fill, queue_pos;
    guint watch;
    bool connected;
    QEMUTimer *ack_timer;
    uint32_t ack_timeout_ms;
};

static uint32_t wire_crc(const uint8_t *p, unsigned size)
{
    uint32_t crc = UINT32_MAX;

    for (unsigned i = 0; i < size; i++) {
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
        }
    }
    return ~crc;
}

static void can_irq_update(TC397CanState *s)
{
    /* TC3x routes interrupt groups through GRINT, not generic ILS/ILE. */
    static const uint8_t groups[30] = {
        12, 2, 10, 3, 11, 2, 9, 3, 1, 15, 15, 14, 0, 2, 0, 3,
        3, 4, 13, 8, 4, 4, 5, 3, 3, 6, 4, 7, 7, 4,
    };
    uint32_t pending = R(s, IR) & R(s, IE);
    uint16_t lines = 0;

    for (unsigned bit = 0; bit < ARRAY_SIZE(groups); bit++) {
        if (pending & BIT(bit)) {
            unsigned group = groups[bit];
            unsigned reg = group < 8 ? GRINT1 : GRINT2;
            unsigned line = extract32(R(s, reg), (group % 8) * 4, 4);

            lines |= BIT(line);
        }
    }
    for (unsigned line = 0; line < 16; line++) {
        if (lines & BIT(line)) {
            qemu_irq_pulse(s->irq[line]);
        }
    }
}

static void can_raise(TC397CanState *s, uint32_t flags)
{
    R(s, IR) |= flags;
    can_irq_update(s);
}

static bool ram_range(TC397CanState *s, unsigned offset, unsigned size)
{
    if (offset > RAM_SIZE || size > RAM_SIZE - offset) {
        can_raise(s, IR_MRAF);
        return false;
    }
    return true;
}

static void can_cancel_watch(TC397CanState *s)
{
    if (s->watch) {
        g_source_remove(s->watch);
        s->watch = 0;
    }
}

static gboolean can_transmit(void *unused, GIOCondition cond, void *opaque)
{
    TC397CanState *s = opaque;

    s->watch = 0;
    while (s->connected && s->queue_fill) {
        int written = qemu_chr_fe_write(&s->chr,
            s->queue[s->queue_get] + s->queue_pos, WIRE_SIZE - s->queue_pos);

        if (written <= 0) {
            if (!(cond & G_IO_HUP)) {
                s->watch = qemu_chr_fe_add_watch(&s->chr, G_IO_OUT | G_IO_HUP,
                                                 can_transmit, s);
            }
            return G_SOURCE_REMOVE;
        }
        s->queue_pos += written;
        if (s->queue_pos == WIRE_SIZE) {
            s->queue_pos = 0;
            s->queue_get = (s->queue_get + 1) % WIRE_QUEUE;
            s->queue_fill--;
        }
    }
    return G_SOURCE_REMOVE;
}

static bool can_enqueue(TC397CanState *s, uint8_t *frame)
{
    if (!s->connected || s->queue_fill == WIRE_QUEUE) {
        return false;
    }
    memcpy(frame, "CAN1", 4);
    stl_le_p(frame + 8, s->epoch);
    stl_le_p(frame + 88, wire_crc(frame, 88));
    memcpy(s->queue[(s->queue_get + s->queue_fill) % WIRE_QUEUE],
           frame, WIRE_SIZE);
    s->queue_fill++;
    if (!s->watch) {
        can_transmit(NULL, G_IO_OUT, s);
    }
    return true;
}

static bool can_running(TC397CanState *s)
{
    return !(R(s, CLC) & 1) && !(R(s, CCCR) & CCCR_INIT) &&
           !(R(s, PSR) & BIT(7));
}

static void can_control(TC397CanState *s)
{
    uint8_t frame[WIRE_SIZE] = { 0 };

    frame[4] = 4;
    stl_le_p(frame + 20, can_running(s) ? 1 : 2);
    can_enqueue(s, frame);
}

static void can_cancel_pending(TC397CanState *s)
{
    R(s, TXBCF) |= R(s, TXBRP);
    R(s, TXBRP) = 0;
    memset(s->tokens, 0, sizeof(s->tokens));
    timer_del(s->ack_timer);
}

static void can_new_epoch(TC397CanState *s)
{
    can_cancel_pending(s);
    can_cancel_watch(s);
    s->queue_fill = s->queue_get = s->queue_pos = 0;
    s->input_used = 0;
    if (++s->epoch == 0) {
        ++s->epoch;
    }
}

static void can_bus_error(TC397CanState *s)
{
    bool already_off = R(s, PSR) & BIT(7);

    can_new_epoch(s);
    R(s, PSR) = BIT(7) | 3; /* Bus-off; ACK error indication. */
    R(s, ECR) = 255;
    R(s, CCCR) |= CCCR_INIT;
    if (!already_off) {
        can_raise(s, IR_BO);
    }
    can_control(s);
}

static void can_ack_schedule(TC397CanState *s)
{
    int64_t next = INT64_MAX;

    timer_del(s->ack_timer);
    for (unsigned i = 0; i < 32; i++) {
        if (s->tokens[i]) {
            next = MIN(next, s->deadlines[i]);
        }
    }
    if (next != INT64_MAX) {
        timer_mod(s->ack_timer, next);
    }
}

static void can_ack_timeout(void *opaque)
{
    can_bus_error(opaque);
}

static unsigned element_bytes(unsigned esc)
{
    static const uint8_t sizes[] = { 8, 12, 16, 20, 24, 32, 48, 64 };

    return sizes[esc & 7] + 8;
}

static bool frame_valid(const uint8_t *f)
{
    unsigned flags = f[5], dlc = f[6], length = f[7];
    uint32_t id = ldl_le_p(f + 16);

    if (flags & ~31u || dlc > 15 ||
        id > (flags & W_IDE ? QEMU_CAN_EFF_MASK : QEMU_CAN_SFF_MASK) ||
        (!(flags & W_FDF) && (dlc > 8 || (flags & (W_BRS | W_ESI)))) ||
        ((flags & W_FDF) && (flags & W_RTR))) {
        return false;
    }
    return length == (flags & W_RTR ? 0 : can_dlc2len(dlc));
}

static int can_filter(TC397CanState *s, const uint8_t *frame,
                      unsigned *filter_index)
{
    bool extended = frame[5] & W_IDE;
    uint32_t id = ldl_le_p(frame + 16);
    uint32_t config = R(s, extended ? XIDFC : SIDFC);
    unsigned count = MIN(extract32(config, 16, 8), extended ? 64 : 128);
    unsigned size = extended ? 8 : 4;

    if ((frame[5] & W_RTR) &&
        (R(s, GFC) & (extended ? BIT(0) : BIT(1)))) {
        return -1;
    }
    for (unsigned i = 0; i < count; i++) {
        unsigned address = (config & 0xfffc) + i * size;
        uint32_t a, b, first, second, type, action, match_id;
        bool match;

        if (!ram_range(s, address, size)) {
            return -1;
        }
        a = ldl_le_p(s->ram + address);
        b = extended ? ldl_le_p(s->ram + address + 4) : a;
        first = extended ? a & 0x1fffffff : extract32(a, 16, 11);
        second = extended ? b & 0x1fffffff : a & 0x7ff;
        type = b >> 30;
        action = extended ? a >> 29 : extract32(a, 27, 3);
        if (!action || (!extended && type == 3)) {
            continue;
        }
        match_id = extended && type != 3 ? id & R(s, XIDAM) : id;
        match = type == 1 ? (match_id == first || match_id == second) :
                type == 2 ? ((match_id & second) == (first & second)) :
                (match_id >= first && match_id <= second);
        if (match) {
            *filter_index = i;
            return action == 1 || action == 5 ? 0 :
                   action == 2 || action == 6 ? 1 : -1;
        }
    }
    *filter_index = 0x80; /* Accepted non-matching frame. */
    return extract32(R(s, GFC), extended ? 2 : 4, 2) < 2 ?
           extract32(R(s, GFC), extended ? 2 : 4, 2) : -1;
}

static void can_receive_frame(TC397CanState *s, const uint8_t *frame)
{
    unsigned filter_index, count, base, bytes, offset, flags = frame[5];
    uint32_t id = ldl_le_p(frame + 16), h0, h1, config;
    int fifo;

    if (!can_running(s) || ((flags & W_FDF) && !(R(s, CCCR) & CCCR_FDOE))) {
        return;
    }
    fifo = can_filter(s, frame, &filter_index);
    if (fifo < 0) {
        return;
    }
    config = R(s, fifo ? RXF1C : RXF0C);
    count = MIN(extract32(config, 16, 7), 64);
    base = config & 0xfffc;
    bytes = element_bytes(R(s, RXESC) >> (fifo * 4));
    if (!count) {
        return;
    }
    if (s->rx_fill[fifo] == count) {
        can_raise(s, BIT(fifo * 4 + 3));
        if (!(config & BIT(31))) {
            return;
        }
        s->rx_get[fifo] = (s->rx_get[fifo] + 1) % count;
        s->rx_fill[fifo]--;
    }
    offset = base + s->rx_put[fifo] * bytes;
    if (!ram_range(s, offset, bytes)) {
        return;
    }
    h0 = flags & W_IDE ? id | BIT(30) : id << 18;
    h0 |= flags & W_RTR ? BIT(29) : 0;
    h0 |= flags & W_ESI ? BIT(31) : 0;
    h1 = (uint32_t)frame[6] << 16 | (filter_index & 0x7f) << 24;
    h1 |= filter_index & 0x80 ? BIT(31) : 0;
    h1 |= flags & W_BRS ? BIT(20) : 0;
    h1 |= flags & W_FDF ? BIT(21) : 0;
    memset(s->ram + offset, 0, bytes);
    stl_le_p(s->ram + offset, h0);
    stl_le_p(s->ram + offset + 4, h1);
    memcpy(s->ram + offset + 8, frame + 24, MIN(frame[7], bytes - 8));
    s->rx_put[fifo] = (s->rx_put[fifo] + 1) % count;
    s->rx_fill[fifo]++;
    can_raise(s, BIT(fifo * 4) |
                 (s->rx_fill[fifo] == count ? BIT(fifo * 4 + 2) : 0));
}

static void can_tx_success(TC397CanState *s, unsigned index)
{
    unsigned count = MIN(extract32(R(s, TXEFC), 16, 6), 32);
    unsigned offset = (R(s, TXEFC) & 0xfffc) + s->event_put * 8;

    s->tokens[index] = 0;
    R(s, TXBRP) &= ~BIT(index);
    R(s, TXBTO) |= BIT(index);
    if (R(s, TXBTIE) & BIT(index)) {
        can_raise(s, BIT(9));
    }
    if (s->tx_header[index][1] & BIT(23)) {
        if (!count || s->event_fill >= count || !ram_range(s, offset, 8)) {
            can_raise(s, IR_TEFL);
        } else {
            stl_le_p(s->ram + offset, s->tx_header[index][0]);
            stl_le_p(s->ram + offset + 4,
                      (s->tx_header[index][1] & 0xff3f0000) | BIT(22));
            s->event_put = (s->event_put + 1) % count;
            s->event_fill++;
            can_raise(s, IR_TEFN);
        }
    }
    can_ack_schedule(s);
}

static void can_submit(TC397CanState *s, unsigned index)
{
    unsigned count = MIN(extract32(R(s, TXBC), 16, 6) +
                         extract32(R(s, TXBC), 24, 6), 32);
    unsigned bytes = element_bytes(R(s, TXESC));
    unsigned offset = (R(s, TXBC) & 0xfffc) + index * bytes;
    uint8_t frame[WIRE_SIZE] = { 0 };
    uint32_t h0, h1;

    if (!can_running(s) || (R(s, CCCR) & CCCR_MON) ||
        (R(s, TXBRP) & BIT(index))) {
        return;
    }
    if (index >= count || !ram_range(s, offset, bytes)) {
        can_raise(s, IR_MRAF);
        return;
    }
    h0 = ldl_le_p(s->ram + offset);
    h1 = ldl_le_p(s->ram + offset + 4);
    frame[4] = 1;
    frame[5] = ((h0 & BIT(30)) ? W_IDE : 0) |
               ((h0 & BIT(29)) ? W_RTR : 0) |
               ((h0 & BIT(31)) ? W_ESI : 0) |
               ((h1 & BIT(21)) ? W_FDF : 0) |
               ((h1 & BIT(20)) ? W_BRS : 0);
    frame[6] = extract32(h1, 16, 4);
    frame[7] = frame[5] & W_RTR ? 0 : can_dlc2len(frame[6]);
    stl_le_p(frame + 16, h0 & BIT(30) ? h0 & 0x1fffffff :
             extract32(h0, 18, 11));
    if (!frame_valid(frame) || frame[7] > bytes - 8 ||
        ((frame[5] & W_FDF) && !(R(s, CCCR) & CCCR_FDOE)) ||
        ((frame[5] & W_BRS) && !(R(s, CCCR) & CCCR_BRSE))) {
        can_raise(s, IR_MRAF);
        return;
    }
    memcpy(frame + 24, s->ram + offset + 8, frame[7]);
    if (++s->next_token == 0) {
        ++s->next_token;
    }
    stl_le_p(frame + 12, s->next_token);
    s->tokens[index] = s->next_token;
    s->tx_header[index][0] = h0;
    s->tx_header[index][1] = h1;
    s->deadlines[index] = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                          s->ack_timeout_ms;
    R(s, TXBRP) |= BIT(index);
    R(s, TXBTO) &= ~BIT(index);
    R(s, TXBCF) &= ~BIT(index);
    if ((R(s, CCCR) & CCCR_TEST) && (R(s, TEST) & BIT(4))) {
        /* Explicit internal loopback, never external-delivery proof. */
        can_receive_frame(s, frame);
        can_tx_success(s, index);
    } else if (!can_enqueue(s, frame)) {
        can_bus_error(s);
    } else {
        can_ack_schedule(s);
    }
}

static void can_handle_wire(TC397CanState *s, const uint8_t *f)
{
    uint32_t token = ldl_le_p(f + 12), status = ldl_le_p(f + 20);

    if (ldl_le_p(f + 8) != s->epoch) {
        return;
    }
    if (f[4] == 2 && !token && !status && frame_valid(f)) {
        can_receive_frame(s, f);
    } else if (f[4] == 3 && token && !f[5] && !f[6] && !f[7] &&
               !ldl_le_p(f + 16) && status >= 1 && status <= 4) {
        for (unsigned i = 0; i < 32; i++) {
            if (s->tokens[i] == token) {
                if (status == 1) {
                    can_tx_success(s, i);
                } else {
                    can_bus_error(s);
                }
                break;
            }
        }
    }
}

static int can_can_receive(void *opaque)
{
    return 1024;
}

static void can_receive(void *opaque, const uint8_t *buf, int size)
{
    TC397CanState *s = opaque;

    for (int i = 0; i < size; i++) {
        s->input[s->input_used++] = buf[i];
        while (s->input_used &&
               memcmp(s->input, "CAN1", MIN(s->input_used, 4))) {
            memmove(s->input, s->input + 1, --s->input_used);
        }
        if (s->input_used == WIRE_SIZE) {
            if (ldl_le_p(s->input + 88) == wire_crc(s->input, 88)) {
                uint8_t frame[WIRE_SIZE];

                memcpy(frame, s->input, sizeof(frame));
                s->input_used = 0;
                can_handle_wire(s, frame);
            } else {
                memmove(s->input, s->input + 1, --s->input_used);
            }
        }
    }
}

static void can_event(void *opaque, QEMUChrEvent event)
{
    TC397CanState *s = opaque;

    if (event == CHR_EVENT_OPENED) {
        s->connected = true;
        can_new_epoch(s);
        can_control(s);
    } else if (event == CHR_EVENT_CLOSED) {
        s->connected = false;
        if (can_running(s) || R(s, TXBRP)) {
            can_bus_error(s);
        } else {
            can_new_epoch(s);
        }
    }
}

static uint64_t can_read(void *opaque, hwaddr offset, unsigned size)
{
    TC397CanState *s = opaque;

    switch (offset) {
    case RXF0S:
    case RXF1S: {
        unsigned fifo = offset == RXF1S;
        unsigned count = MIN(extract32(R(s, fifo ? RXF1C : RXF0C), 16, 7), 64);

        return s->rx_fill[fifo] | s->rx_get[fifo] << 8 | s->rx_put[fifo] << 16 |
               (count && s->rx_fill[fifo] == count ? BIT(24) : 0);
    }
    case TXEFS: {
        unsigned count = MIN(extract32(R(s, TXEFC), 16, 6), 32);

        return s->event_fill | s->event_get << 8 | s->event_put << 16 |
               (count && s->event_fill == count ? BIT(24) : 0);
    }
    case TXFQS: {
        unsigned count = MIN(extract32(R(s, TXBC), 16, 6) +
                             extract32(R(s, TXBC), 24, 6), 32);
        unsigned used = ctpop32(R(s, TXBRP));
        unsigned free = count > used ? count - used : 0;
        unsigned put = free ? ctz32(~R(s, TXBRP)) : 0;

        return free | put << 16 | (!free ? BIT(21) : 0);
    }
    case TSCV:
    case TXBAR:
    case TXBCR:
    case RXF0A:
    case RXF1A:
    case TXEFA:
        return 0;
    case 0x258: /* TC3x has no ILS/ILE registers at these offsets. */
    case 0x25c:
        qemu_log_mask(LOG_UNIMP, "tc397-can: no generic ILS/ILE\n");
        return 0;
    default:
        return s->regs[offset / 4];
    }
}

static bool can_config_register(hwaddr offset)
{
    switch (offset) {
    case DBTP: case NBTP: case TDCR: case TEST: case TSCC:
    case GFC: case SIDFC: case XIDFC: case XIDAM: case RXF0C:
    case RXF1C: case RXBC: case RXESC: case TXBC: case TXESC: case TXEFC:
        return true;
    default:
        return false;
    }
}

static void can_write(void *opaque, hwaddr offset, uint64_t value,
                      unsigned size)
{
    TC397CanState *s = opaque;
    uint32_t data = value;

    if (can_config_register(offset)) {
        if ((R(s, CCCR) & (CCCR_INIT | CCCR_CCE)) == (CCCR_INIT | CCCR_CCE)) {
            s->regs[offset / 4] = data;
        }
        return;
    }
    switch (offset) {
    case CLC:
        R(s, CLC) = (data & 1) ? 3 : 0;
        if (data & 1) {
            can_new_epoch(s);
        }
        can_control(s);
        break;
    case MCR:
        R(s, MCR) = data & ~BIT(28); /* Fixed clock; RAM never busy. */
        break;
    case STARTADR: case ENDADR: case NPCR:
        s->regs[offset / 4] = data;
        break;
    case CCCR: {
        uint32_t old = R(s, CCCR);
        uint32_t writable = CCCR_INIT | BIT(4);

        if (old & CCCR_INIT) {
            writable |= CCCR_CCE;
            if (old & CCCR_CCE) {
                writable |= 0xffe4;
            }
        }
        R(s, CCCR) = (old & ~writable) | (data & writable);
        if (data & BIT(4)) {
            R(s, CCCR) |= BIT(3) | CCCR_INIT;
        } else {
            R(s, CCCR) &= ~BIT(3);
        }
        if (!(R(s, CCCR) & CCCR_INIT)) {
            R(s, CCCR) &= ~CCCR_CCE;
            if (s->connected || ((R(s, CCCR) & CCCR_TEST) &&
                                 (R(s, TEST) & BIT(4)))) {
                R(s, PSR) = R(s, ECR) = 0;
            }
        }
        if ((old ^ R(s, CCCR)) & CCCR_INIT) {
            can_new_epoch(s);
            can_control(s);
        }
        break;
    }
    case IE: case GRINT1: case GRINT2:
        s->regs[offset / 4] = data;
        can_irq_update(s);
        break;
    case IR:
        R(s, IR) &= ~data;
        can_irq_update(s);
        break;
    case TXBTIE: case TXBCIE:
        s->regs[offset / 4] = data;
        break;
    case TXBAR:
        for (unsigned i = 0; i < 32; i++) {
            if (data & BIT(i)) {
                can_submit(s, i);
            }
        }
        break;
    case TXBCR:
        R(s, TXBCF) |= R(s, TXBRP) & data;
        R(s, TXBRP) &= ~data;
        for (unsigned i = 0; i < 32; i++) {
            if (data & BIT(i)) {
                s->tokens[i] = 0;
            }
        }
        can_ack_schedule(s);
        if (R(s, TXBCIE) & data & R(s, TXBCF)) {
            can_raise(s, BIT(10));
        }
        break;
    case RXF0A:
    case RXF1A: {
        unsigned fifo = offset == RXF1A;
        unsigned count = MIN(extract32(R(s, fifo ? RXF1C : RXF0C), 16, 7), 64);

        if (count && s->rx_fill[fifo] && (data & 63) == s->rx_get[fifo]) {
            s->rx_get[fifo] = (s->rx_get[fifo] + 1) % count;
            s->rx_fill[fifo]--;
        }
        break;
    }
    case TXEFA: {
        unsigned count = MIN(extract32(R(s, TXEFC), 16, 6), 32);

        if (count && s->event_fill && (data & 31) == s->event_get) {
            s->event_get = (s->event_get + 1) % count;
            s->event_fill--;
        }
        break;
    }
    default:
        qemu_log_mask(LOG_UNIMP, "tc397-can: write unsupported register 0x%"
                      HWADDR_PRIx "\n", offset);
        break;
    }
}

static const MemoryRegionOps can_ops = {
    .read = can_read,
    .write = can_write,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void can_reset(DeviceState *dev)
{
    TC397CanState *s = TC397_CAN(dev);

    can_new_epoch(s);
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->rx_get, 0, sizeof(s->rx_get));
    memset(s->rx_put, 0, sizeof(s->rx_put));
    memset(s->rx_fill, 0, sizeof(s->rx_fill));
    s->event_get = s->event_put = s->event_fill = 0;
    R(s, CCCR) = CCCR_INIT;
    R(s, ENDN) = 0x87654321;
    R(s, CREL) = 0x32000000; /* M_CAN 3.2 functional register interface. */
    R(s, XIDAM) = 0x1fffffff;
    for (unsigned i = 0; i < 16; i++) {
        qemu_irq_lower(s->irq[i]);
    }
    can_control(s);
}

static void can_realize(DeviceState *dev, Error **errp)
{
    TC397CanState *s = TC397_CAN(dev);

    if (!s->ack_timeout_ms) {
        error_setg(errp, "tc397-can ack-timeout-ms must be nonzero");
        return;
    }
    if (!memory_region_init_ram(&s->ram_region, OBJECT(s), "tc397-can.mram",
                                RAM_SIZE, errp)) {
        return;
    }
    s->ram = memory_region_get_ram_ptr(&s->ram_region);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->ram_region);
    qemu_chr_fe_set_handlers(&s->chr, can_can_receive, can_receive,
                             can_event, NULL, s, NULL, true);
}

static void can_init(Object *obj)
{
    TC397CanState *s = TC397_CAN(obj);

    memory_region_init_io(&s->regs_region, obj, &can_ops, s,
                          "tc397-can.regs", 0x400);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->regs_region);
    for (unsigned i = 0; i < 16; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[i]);
    }
    s->ack_timer = timer_new_ms(QEMU_CLOCK_REALTIME, can_ack_timeout, s);
}

static void can_finalize(Object *obj)
{
    TC397CanState *s = TC397_CAN(obj);

    can_cancel_watch(s);
    timer_free(s->ack_timer);
}

static const Property can_properties[] = {
    DEFINE_PROP_CHR("chardev", TC397CanState, chr),
    DEFINE_PROP_UINT32("ack-timeout-ms", TC397CanState, ack_timeout_ms, 5000),
};

static const VMStateDescription can_vmstate = {
    .name = "tc397-can",
    .unmigratable = true,
};

static void can_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = can_realize;
    dc->vmsd = &can_vmstate;
    device_class_set_legacy_reset(dc, can_reset);
    device_class_set_props(dc, can_properties);
}

static const TypeInfo can_info = {
    .name = TYPE_TC397_CAN,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TC397CanState),
    .instance_init = can_init,
    .instance_finalize = can_finalize,
    .class_init = can_class_init,
};

static void can_register_types(void)
{
    type_register_static(&can_info);
}

type_init(can_register_types)
