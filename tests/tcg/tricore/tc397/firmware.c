/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Functional TC397 subset probe. Not a vendor MCAL or safety qualification. */
typedef unsigned int u32;
/* MMIO accesses must occur exactly as issued by the guest. */
#define REG(addr) (*(volatile unsigned int *)(addr))
#define UART(off) REG(0xf0000600u + (off))
#define STM(off) REG(0xf0001000u + (off))
#define SRC(index) REG(0xf0038000u + 4u * (index))
#define SRC_ENABLE (1u << 10)
#define SRC_CLEAR (1u << 25)
#define SRC_SET (1u << 26)
#define STM_SRC 0xc0
#define AUX_SRC 0xc1
#define RX_SRC 0x15
#define TIMER_STEP 500000u /* 10 ms at the board's fixed 50 MHz. */

static volatile u32 order[8]; /* ISR writes; main verifies service order. */
static volatile u32 order_count; /* ISR writes; main polls completion. */
static volatile u32 timer_count; /* STM ISR writes; main polls completion. */
static volatile u32 rx_byte; /* UART ISR writes; main verifies host input. */
static volatile u32 rx_count; /* UART ISR writes; main checks wake-up. */

static void puts_uart(const char *s)
{
    while (*s) {
        UART(0x44) = (unsigned char)*s++;
    }
}

static void fail(const char *name)
{
    puts_uart("TC397:FAIL:");
    puts_uart(name);
    puts_uart("\n");
    for (;;) {
        /* Preserve the failing state until the host terminates QEMU. */
    }
}

static void check(int ok, const char *name)
{
    if (!ok) {
        fail(name);
    }
    puts_uart("TC397:PASS:");
    puts_uart(name);
    puts_uart("\n");
}

static void icr(u32 value)
{
    __asm__ volatile("mtcr $icr,%0\n\tisync" : : "d"(value) : "memory");
}

static u32 read_icr(void)
{
    u32 value;
    __asm__ volatile("mfcr %0,$icr" : "=d"(value));
    return value;
}

static void delay(void)
{
    u32 start = STM(0x10);
    while ((u32)(STM(0x10) - start) < 50000u) {
        /* Poll hardware while interrupt delivery is deliberately masked. */
    }
}

/* The ISR updates count asynchronously with respect to this polling loop. */
static void await_count(volatile u32 *count, u32 expected)
{
    u32 start = STM(0x10);
    while (*count < expected) {
        if ((u32)(STM(0x10) - start) > 50000000u) {
            fail("interrupt-timeout");
        }
    }
}

void tc397_irq(u32 priority)
{
    if (priority == 7) {
        STM(0x40) = 1; /* CMP0IRR: deassert peripheral before SRC ack. */
        SRC(STM_SRC) = 7 | SRC_ENABLE | SRC_CLEAR;
        ++timer_count;
        if (timer_count < 5) {
            STM(0x30) = STM(0x10) + TIMER_STEP;
        } else {
            STM(0x3c) = 0;
        }
    } else if (priority == 11) {
        rx_byte = UART(0x48) & 255;
        UART(0x3c) = 1u << 28;
        SRC(RX_SRC) = 11 | SRC_ENABLE | SRC_CLEAR;
        ++rx_count;
    } else {
        if (order_count < 8) {
            order[order_count++] = priority;
        }
        if (priority == 9) {
            SRC(AUX_SRC) = 9 | SRC_ENABLE | SRC_CLEAR;
        } else {
            SRC(STM_SRC) = 3 | SRC_ENABLE | SRC_CLEAR;
        }
    }
}

int main(void)
{
    u32 initial_uart_flags = UART(0x34);
    u32 initial_timer_icr = STM(0x3c);
    u32 initial_src0 = SRC(STM_SRC);
    u32 initial_src1 = SRC(AUX_SRC);
    u32 initial_rx_src = SRC(RX_SRC);
    u32 initial_rx_fill = (UART(0x10) >> 16) & 31;
    u32 start;

    UART(0x4c) = 1;
    puts_uart("TC397:BOOT\n");
    check(initial_uart_flags == 0 && initial_timer_icr == 0 &&
          initial_src0 == 0 && initial_src1 == 0 && initial_rx_src == 0 &&
          initial_rx_fill == 0, "reset-state");
    check(REG(0xd000f010) == 0 && REG(0xd000f014) == 0 &&
          REG(0xd000f018) == 0, "cpu-reset-state");

    REG(0x7000f000) = 0x12345678;
    check(REG(0xd000f000) == 0x12345678, "dspr-alias");
    REG(0x70100000) = 0x87654321;
    check(REG(0xc0000000) == 0x87654321, "pspr-alias");
    check(REG(0x80000100) == REG(0xa0000100), "flash-alias");
    start = REG(0x80000100);
    REG(0x80000100) = ~start;
    check(REG(0xa0000100) == start, "flash-readonly");
    start = STM(0x10);
    delay();
    check((u32)(STM(0x10) - start) >= 50000u, "stm-counter");

    icr(0);
    SRC(STM_SRC) = 3 | SRC_ENABLE | SRC_SET;
    SRC(AUX_SRC) = 9 | SRC_ENABLE | SRC_SET;
    delay();
    check(order_count == 0, "global-mask");
    icr(0x00ff0000); /* Attempt to overwrite read-only PIPN while IE=0. */
    check(((read_icr() >> 16) & 255) == 9, "pipn-readonly");
    icr(0x8000 | 5);
    await_count(&order_count, 1);
    delay();
    check(order_count == 1 && order[0] == 9, "priority-threshold");
    icr(0x8000);
    await_count(&order_count, 2);
    check(order_count == 2 && order[1] == 3, "pending-low-priority");

    SRC(STM_SRC) = 3 | SRC_SET;
    delay();
    check(order_count == 2, "source-disable");
    SRC(STM_SRC) = 3 | SRC_ENABLE;
    await_count(&order_count, 3);
    check(order_count == 3 && order[2] == 3, "source-reenable");
    SRC(STM_SRC) = SRC_ENABLE | SRC_SET;
    delay();
    check(order_count == 3, "priority-zero-masked");
    SRC(STM_SRC) = SRC_CLEAR;

    /* Simultaneous requests must arbitrate by SRPN, not source index. */
    icr(0);
    SRC(STM_SRC) = 3 | SRC_ENABLE | SRC_SET;
    SRC(AUX_SRC) = 9 | SRC_ENABLE | SRC_SET;
    icr(0x8000);
    await_count(&order_count, 5);
    check(order_count == 5 && order[3] == 9 && order[4] == 3,
          "priority-order");
    SRC(AUX_SRC) = SRC_CLEAR;

    SRC(STM_SRC) = 7 | SRC_ENABLE | SRC_CLEAR;
    STM(0x38) = 31;
    STM(0x30) = STM(0x10) + TIMER_STEP;
    STM(0x3c) = 1;
    await_count(&timer_count, 5);
    check(timer_count == 5, "stm-five-irqs");
    delay();
    check(timer_count == 5, "stm-disable");

    UART(0x10) = 2; /* RX FIFO ENI. */
    UART(0x40) = 1u << 28;
    SRC(RX_SRC) = 11 | SRC_ENABLE | SRC_CLEAR;
    puts_uart("TC397:READY:RX\n");
    do {
        __asm__ volatile("wait" : : : "memory");
    } while (!rx_count);
    check(rx_count == 1 && rx_byte == 'V', "uart-rx-irq-wait");
    puts_uart("TC397:ECHO:");
    UART(0x44) = rx_byte;
    puts_uart("\n");
    check(rx_count == 1, "uart-tx-echo");

    /* Leave non-default state behind so QMP reset is not a no-op test. */
    icr(0);
    SRC(AUX_SRC) = 9 | SRC_SET;
    puts_uart("TC397:DONE\n");
    for (;;) {
        __asm__ volatile("wait" : : : "memory");
    }
}
