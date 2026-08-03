/*
 * ARM GICv3 emulation: Redistributor
 *
 * Copyright (c) 2015 Huawei.
 * Copyright (c) 2016 Linaro Limited.
 * Written by Shlomo Pongratz, Peter Maydell
 *
 * This code is licensed under the GPL, version 2 or (at your option)
 * any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "trace.h"
#include "gicv3_internal.h"

static uint32_t mask_group(GICv3CPUState *cs, MemTxAttrs attrs)
{
    /* Return a 32-bit mask which should be applied for this set of 32
     * interrupts; each bit is 1 if access is permitted by the
     * combination of attrs.secure and GICR_GROUPR. (GICR_NSACR does
     * not affect config register accesses, unlike GICD_NSACR.)
     */
    if (!attrs.secure && !(cs->gic->gicd_ctlr & GICD_CTLR_DS)) {
        /* bits for Group 0 or Secure Group 1 interrupts are RAZ/WI */
        return cs->gicr_igroupr0;
    }
    return 0xFFFFFFFFU;
}

static int gicr_ns_access(GICv3CPUState *cs, int irq)
{
    /* Return the 2 bit NSACR.NS_access field for this SGI */
    assert(irq < 16);
    return extract32(cs->gicr_nsacr, irq * 2, 2);
}

static void gicr_write_bitmap_reg(GICv3CPUState *cs, MemTxAttrs attrs,
                                  uint32_t *reg, uint32_t val)
{
    /* Helper routine to implement writing to a "set" register */
    val &= mask_group(cs, attrs);
    *reg = val;
    gicv3_redist_update(cs);
}

static void gicr_write_set_bitmap_reg(GICv3CPUState *cs, MemTxAttrs attrs,
                                      uint32_t *reg, uint32_t val)
{
    /* Helper routine to implement writing to a "set-bitmap" register */
    val &= mask_group(cs, attrs);
    *reg |= val;
    gicv3_redist_update(cs);
}

static void gicr_write_clear_bitmap_reg(GICv3CPUState *cs, MemTxAttrs attrs,
                                        uint32_t *reg, uint32_t val)
{
    /* Helper routine to implement writing to a "clear-bitmap" register */
    val &= mask_group(cs, attrs);
    *reg &= ~val;
    gicv3_redist_update(cs);
}

static uint32_t gicr_read_bitmap_reg(GICv3CPUState *cs, MemTxAttrs attrs,
                                     uint32_t reg)
{
    reg &= mask_group(cs, attrs);
    return reg;
}

static bool gicr_eppi_word_index(GICv3CPUState *cs, hwaddr offset,
                                 hwaddr base, unsigned int *word)
{
    *word = (offset - base) / 4 - 1;
    return *word < BITS_TO_U32S(cs->gic->num_eppi);
}

static uint32_t gicr_eppi_group_mask(GICv3CPUState *cs, MemTxAttrs attrs,
                                     unsigned int word)
{
    if (!attrs.secure && !(cs->gic->gicd_ctlr & GICD_CTLR_DS)) {
        return cs->eppi_group[word];
    }
    return UINT32_MAX;
}

static uint32_t gicr_eppi_read_bitmap(GICv3CPUState *cs, MemTxAttrs attrs,
                                      uint32_t value, unsigned int word)
{
    return value & gicr_eppi_group_mask(cs, attrs, word);
}

static void gicr_eppi_write_set_bitmap(GICv3CPUState *cs,
                                       MemTxAttrs attrs, uint32_t *reg,
                                       uint32_t value, unsigned int word)
{
    *reg |= value & gicr_eppi_group_mask(cs, attrs, word);
    gicv3_redist_update(cs);
}

static void gicr_eppi_write_clear_bitmap(GICv3CPUState *cs,
                                         MemTxAttrs attrs, uint32_t *reg,
                                         uint32_t value, unsigned int word)
{
    *reg &= ~(value & gicr_eppi_group_mask(cs, attrs, word));
    gicv3_redist_update(cs);
}

static bool vpendbaser_uses_vpeid(GICv3CPUState *cs)
{
    return cs->gic->gicv4_1 && cs->gic->rvpeid;
}

static bool vcpu_resident(GICv3CPUState *cs, uint64_t vptaddr,
                          uint32_t vpeid)
{
    /*
     * Return true if a vCPU is resident, which is defined by
     * whether the GICR_VPENDBASER register is marked VALID and
     * has the right virtual pending table address.
     */
    if (!FIELD_EX64(cs->gicr_vpendbaser, GICR_VPENDBASER, VALID)) {
        return false;
    }
    if (vpendbaser_uses_vpeid(cs)) {
        return vpeid == FIELD_EX64(cs->gicr_vpendbaser,
                                   GICR_VPENDBASER_4_1, VPEID);
    }
    return vptaddr == (cs->gicr_vpendbaser & R_GICR_VPENDBASER_PHYADDR_MASK);
}

static void gicr_vpendbaser_mark_dirty(GICv3CPUState *cs)
{
    if (cs->gic->vpend_valid_dirty &&
        FIELD_EX64(cs->gicr_vpendbaser, GICR_VPENDBASER, VALID)) {
        cs->gicr_vpendbaser = FIELD_DP64(cs->gicr_vpendbaser,
                                         GICR_VPENDBASER, DIRTY, 1);
    }
}

/**
 * update_for_one_lpi: Update pending information if this LPI is better
 *
 * @cs: GICv3CPUState
 * @irq: interrupt to look up in the LPI Configuration table
 * @ctbase: physical address of the LPI Configuration table to use
 * @ds: true if priority value should not be shifted
 * @hpp: points to pending information to update
 *
 * Look up @irq in the Configuration table specified by @ctbase
 * to see if it is enabled and what its priority is. If it is an
 * enabled interrupt with a higher priority than that currently
 * recorded in @hpp, update @hpp.
 */
static void update_for_one_lpi(GICv3CPUState *cs, int irq,
                               uint64_t ctbase, bool ds, PendingIrq *hpp)
{
    uint8_t lpite;
    uint8_t prio;

    address_space_read(&cs->gic->dma_as,
                       ctbase + ((irq - GICV3_LPI_INTID_START) * sizeof(lpite)),
                       MEMTXATTRS_UNSPECIFIED, &lpite, sizeof(lpite));

    if (!(lpite & LPI_CTE_ENABLED)) {
        return;
    }

    if (ds) {
        prio = lpite & LPI_PRIORITY_MASK;
    } else {
        prio = ((lpite & LPI_PRIORITY_MASK) >> 1) | 0x80;
    }

    if ((prio < hpp->prio) ||
        ((prio == hpp->prio) && (irq <= hpp->irq))) {
        hpp->irq = irq;
        hpp->prio = prio;
        hpp->nmi = false;
        /* LPIs and vLPIs are always non-secure Grp1 interrupts */
        hpp->grp = GICV3_G1NS;
    }
}

/**
 * update_for_all_lpis: Fully scan LPI tables and find best pending LPI
 *
 * @cs: GICv3CPUState
 * @ptbase: physical address of LPI Pending table
 * @ctbase: physical address of LPI Configuration table
 * @ptsizebits: size of tables, specified as number of interrupt ID bits minus 1
 * @ds: true if priority value should not be shifted
 * @hpp: points to pending information to set
 *
 * Recalculate the highest priority pending enabled LPI from scratch,
 * and set @hpp accordingly.
 *
 * We scan the LPI pending table @ptbase; for each pending LPI, we read the
 * corresponding entry in the LPI configuration table @ctbase to extract
 * the priority and enabled information.
 *
 * We take @ptsizebits in the form idbits-1 because this is the way that
 * LPI table sizes are architecturally specified in GICR_PROPBASER.IDBits
 * and in the VMAPP command's VPT_size field.
 */
static void update_for_all_lpis(GICv3CPUState *cs, uint64_t ptbase,
                                uint64_t ctbase, unsigned ptsizebits,
                                bool ds, PendingIrq *hpp)
{
    AddressSpace *as = &cs->gic->dma_as;
    uint8_t pend;
    uint32_t pendt_size = (1ULL << (ptsizebits + 1));
    int i, bit;

    hpp->prio = 0xff;
    hpp->nmi = false;

    for (i = GICV3_LPI_INTID_START / 8; i < pendt_size / 8; i++) {
        address_space_read(as, ptbase + i, MEMTXATTRS_UNSPECIFIED, &pend, 1);
        while (pend) {
            bit = ctz32(pend);
            update_for_one_lpi(cs, i * 8 + bit, ctbase, ds, hpp);
            pend &= ~(1 << bit);
        }
    }
}

/**
 * set_lpi_pending_bit: Set or clear pending bit for an LPI
 *
 * @cs: GICv3CPUState
 * @ptbase: physical address of LPI Pending table
 * @irq: LPI to change pending state for
 * @level: false to clear pending state, true to set
 *
 * Returns true if we needed to do something, false if the pending bit
 * was already at @level.
 */
static bool set_pending_table_bit(GICv3CPUState *cs, uint64_t ptbase,
                                  int irq, bool level)
{
    AddressSpace *as = &cs->gic->dma_as;
    uint64_t addr = ptbase + irq / 8;
    uint8_t pend;

    address_space_read(as, addr, MEMTXATTRS_UNSPECIFIED, &pend, 1);
    if (extract32(pend, irq % 8, 1) == level) {
        /* Bit already at requested state, no action required */
        return false;
    }
    pend = deposit32(pend, irq % 8, 1, level ? 1 : 0);
    address_space_write(as, addr, MEMTXATTRS_UNSPECIFIED, &pend, 1);
    return true;
}

static uint8_t gicr_read_ipriorityr(GICv3CPUState *cs, MemTxAttrs attrs,
                                    int irq)
{
    /* Read the value of GICR_IPRIORITYR<n> for the specified interrupt,
     * honouring security state (these are RAZ/WI for Group 0 or Secure
     * Group 1 interrupts).
     */
    uint32_t prio;

    prio = cs->gicr_ipriorityr[irq];

    if (!attrs.secure && !(cs->gic->gicd_ctlr & GICD_CTLR_DS)) {
        if (!(cs->gicr_igroupr0 & (1U << irq))) {
            /* Fields for Group 0 or Secure Group 1 interrupts are RAZ/WI */
            return 0;
        }
        /* NS view of the interrupt priority */
        prio = (prio << 1) & 0xff;
    }
    return prio;
}

static void gicr_write_ipriorityr(GICv3CPUState *cs, MemTxAttrs attrs, int irq,
                                  uint8_t value)
{
    /* Write the value of GICD_IPRIORITYR<n> for the specified interrupt,
     * honouring security state (these are RAZ/WI for Group 0 or Secure
     * Group 1 interrupts).
     */
    if (!attrs.secure && !(cs->gic->gicd_ctlr & GICD_CTLR_DS)) {
        if (!(cs->gicr_igroupr0 & (1U << irq))) {
            /* Fields for Group 0 or Secure Group 1 interrupts are RAZ/WI */
            return;
        }
        /* NS view of the interrupt priority */
        value = 0x80 | (value >> 1);
    }
    cs->gicr_ipriorityr[irq] = value;
}

static uint8_t gicr_read_eppi_priority(GICv3CPUState *cs, MemTxAttrs attrs,
                                       unsigned int irq)
{
    uint8_t priority = cs->eppi_priority[irq];

    if (!attrs.secure && !(cs->gic->gicd_ctlr & GICD_CTLR_DS)) {
        if (!test_bit32(irq, cs->eppi_group)) {
            return 0;
        }
        priority = (priority << 1) & 0xff;
    }
    return priority;
}

static void gicr_write_eppi_priority(GICv3CPUState *cs, MemTxAttrs attrs,
                                     unsigned int irq, uint8_t value)
{
    if (!attrs.secure && !(cs->gic->gicd_ctlr & GICD_CTLR_DS)) {
        if (!test_bit32(irq, cs->eppi_group)) {
            return;
        }
        value = 0x80 | (value >> 1);
    }
    cs->eppi_priority[irq] = value;
}

static void gicv3_redist_update_vlpi_only(GICv3CPUState *cs)
{
    uint64_t ptbase, ctbase, idbits;

    if (!FIELD_EX64(cs->gicr_vpendbaser, GICR_VPENDBASER, VALID)) {
        cs->hppvlpi.prio = 0xff;
        cs->hppvlpi.nmi = false;
        return;
    }

    if (vpendbaser_uses_vpeid(cs)) {
        ptbase = cs->gicr_vpend_vptaddr;
        if (!ptbase) {
            cs->hppvlpi.prio = 0xff;
            cs->hppvlpi.nmi = false;
            return;
        }
    } else {
        ptbase = cs->gicr_vpendbaser & R_GICR_VPENDBASER_PHYADDR_MASK;
    }
    ctbase = vpendbaser_uses_vpeid(cs) && cs->gicr_vpend_vconfaddr ?
        cs->gicr_vpend_vconfaddr :
        cs->gicr_vpropbaser & R_GICR_VPROPBASER_PHYADDR_MASK;
    idbits = FIELD_EX64(cs->gicr_vpropbaser, GICR_VPROPBASER, IDBITS);

    update_for_all_lpis(cs, ptbase, ctbase, idbits, true, &cs->hppvlpi);
}

static void gicv3_redist_update_vlpi(GICv3CPUState *cs)
{
    gicv3_redist_update_vlpi_only(cs);
    gicv3_cpuif_virt_irq_fiq_update(cs);
}

static void gicr_write_vpendbaser(GICv3CPUState *cs, uint64_t newval)
{
    /* Write @newval to GICR_VPENDBASER, handling its effects */
    bool oldvalid = FIELD_EX64(cs->gicr_vpendbaser, GICR_VPENDBASER, VALID);
    bool newvalid = FIELD_EX64(newval, GICR_VPENDBASER, VALID);
    bool pendinglast;
    uint64_t writable_mask;

    if (vpendbaser_uses_vpeid(cs)) {
        uint32_t vpeid = FIELD_EX64(newval, GICR_VPENDBASER_4_1, VPEID);

        writable_mask = R_GICR_VPENDBASER_4_1_VPEID_MASK |
            R_GICR_VPENDBASER_4_1_PENDINGLAST_MASK |
            R_GICR_VPENDBASER_4_1_VALID_MASK;
        if (vpeid >= (1ULL << cs->gic->vpeid_bits)) {
            return;
        }
    } else {
        if (newval & (MAKE_64BIT_MASK(0, 7) |
                      MAKE_64BIT_MASK(12, 4))) {
            return;
        }
        writable_mask = R_GICR_VPENDBASER_INNERCACHE_MASK |
            R_GICR_VPENDBASER_SHAREABILITY_MASK |
            R_GICR_VPENDBASER_PHYADDR_MASK |
            R_GICR_VPENDBASER_OUTERCACHE_MASK |
            R_GICR_VPENDBASER_PENDINGLAST_MASK |
            R_GICR_VPENDBASER_IDAI_MASK |
            R_GICR_VPENDBASER_VALID_MASK;
    }
    newval &= writable_mask;
    if (cs->gic->vpend_valid_dirty && oldvalid && newvalid) {
        newval |= cs->gicr_vpendbaser & R_GICR_VPENDBASER_DIRTY_MASK;
    }

    if (oldvalid && newvalid) {
        /*
         * Changing other fields while VALID is 1 is UNPREDICTABLE;
         * we choose to log and ignore the write.
         */
        if (cs->gicr_vpendbaser ^ newval) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: Changing GICR_VPENDBASER when VALID=1 "
                          "is UNPREDICTABLE\n", __func__);
        }
        return;
    }
    if (!oldvalid && !newvalid) {
        cs->gicr_vpendbaser = newval;
        cs->gicr_vpend_vptaddr = 0;
        cs->gicr_vpend_vconfaddr = 0;
        return;
    }

    if (newvalid) {
        /*
         * Valid going from 0 to 1: update hppvlpi from tables.
         * If IDAI is 0 we are allowed to use the info we cached in
         * the IMPDEF area of the table.
         * PendingLast is RES1 when we make this transition.
         */
        pendinglast = true;
    } else {
        /*
         * Valid going from 1 to 0:
         * Set PendingLast if there was a pending enabled interrupt
         * for the vPE that was just descheduled.
         * If we cache info in the IMPDEF area, write it out here.
         */
        pendinglast = cs->hppvlpi.prio != 0xff;
        cs->gicr_vpend_vptaddr = 0;
        cs->gicr_vpend_vconfaddr = 0;
    }

    newval = FIELD_DP64(newval, GICR_VPENDBASER, PENDINGLAST, pendinglast);
    cs->gicr_vpendbaser = newval;
    gicv3_redist_update_vlpi(cs);
}

static void gicr_write_lpir(GICv3CPUState *cs, uint64_t value, int level)
{
    if (cs->gic->direct_lpi) {
        gicv3_redist_process_lpi(cs, (uint32_t)value, level);
    }
}

static MemTxResult gicr_readb(GICv3CPUState *cs, hwaddr offset,
                              uint64_t *data, MemTxAttrs attrs)
{
    switch (offset) {
    case GICR_IPRIORITYR ... GICR_IPRIORITYR + 0x5f:
    {
        unsigned int irq = offset - GICR_IPRIORITYR;

        if (irq < GIC_INTERNAL) {
            *data = gicr_read_ipriorityr(cs, attrs, irq);
        } else if (irq - GIC_INTERNAL < cs->gic->num_eppi) {
            *data = gicr_read_eppi_priority(cs, attrs, irq - GIC_INTERNAL);
        } else {
            *data = 0;
        }
        return MEMTX_OK;
    }
    default:
        return MEMTX_ERROR;
    }
}

static MemTxResult gicr_writeb(GICv3CPUState *cs, hwaddr offset,
                               uint64_t value, MemTxAttrs attrs)
{
    switch (offset) {
    case GICR_IPRIORITYR ... GICR_IPRIORITYR + 0x5f:
    {
        unsigned int irq = offset - GICR_IPRIORITYR;

        if (irq < GIC_INTERNAL) {
            gicr_write_ipriorityr(cs, attrs, irq, value);
        } else if (irq - GIC_INTERNAL < cs->gic->num_eppi) {
            gicr_write_eppi_priority(cs, attrs, irq - GIC_INTERNAL, value);
        }
        gicv3_redist_update(cs);
        return MEMTX_OK;
    }
    default:
        return MEMTX_ERROR;
    }
}

static MemTxResult gicr_readl(GICv3CPUState *cs, hwaddr offset,
                              uint64_t *data, MemTxAttrs attrs)
{
    switch (offset) {
    case GICR_CTLR:
        *data = cs->gicr_ctlr;
        return MEMTX_OK;
    case GICR_IIDR:
        *data = gicv3_iidr();
        return MEMTX_OK;
    case GICR_TYPER:
        *data = extract64(cs->gicr_typer, 0, 32);
        return MEMTX_OK;
    case GICR_TYPER + 4:
        *data = extract64(cs->gicr_typer, 32, 32);
        return MEMTX_OK;
    case GICR_STATUSR:
        /* RAZ/WI for us (this is an optional register and our implementation
         * does not track RO/WO/reserved violations to report them to the guest)
         */
        *data = 0;
        return MEMTX_OK;
    case GICR_WAKER:
        *data = cs->gicr_waker;
        return MEMTX_OK;
    case GICR_PROPBASER:
        *data = extract64(cs->gicr_propbaser, 0, 32);
        return MEMTX_OK;
    case GICR_PROPBASER + 4:
        *data = extract64(cs->gicr_propbaser, 32, 32);
        return MEMTX_OK;
    case GICR_PENDBASER:
        *data = extract64(cs->gicr_pendbaser, 0, 32);
        return MEMTX_OK;
    case GICR_PENDBASER + 4:
        *data = extract64(cs->gicr_pendbaser, 32, 32);
        return MEMTX_OK;
    case GICR_IGROUPR0:
        if (!attrs.secure && !(cs->gic->gicd_ctlr & GICD_CTLR_DS)) {
            *data = 0;
            return MEMTX_OK;
        }
        *data = cs->gicr_igroupr0;
        return MEMTX_OK;
    case GICR_IGROUPR0 + 4 ... GICR_IGROUPR0 + 8:
    {
        unsigned int word;

        if (!gicr_eppi_word_index(cs, offset, GICR_IGROUPR0, &word) ||
            (!attrs.secure && !(cs->gic->gicd_ctlr & GICD_CTLR_DS))) {
            *data = 0;
        } else {
            *data = cs->eppi_group[word];
        }
        return MEMTX_OK;
    }
    case GICR_ISENABLER0:
    case GICR_ICENABLER0:
        *data = gicr_read_bitmap_reg(cs, attrs, cs->gicr_ienabler0);
        return MEMTX_OK;
    case GICR_ISPENDR0:
    case GICR_ICPENDR0:
    {
        /* The pending register reads as the logical OR of the pending
         * latch and the input line level for level-triggered interrupts.
         */
        uint32_t val = cs->gicr_ipendr0 | (~cs->edge_trigger & cs->level);
        *data = gicr_read_bitmap_reg(cs, attrs, val);
        return MEMTX_OK;
    }
    case GICR_ISACTIVER0:
    case GICR_ICACTIVER0:
        *data = gicr_read_bitmap_reg(cs, attrs, cs->gicr_iactiver0);
        return MEMTX_OK;
    case GICR_ISENABLER0 + 4 ... GICR_ISENABLER0 + 8:
    case GICR_ICENABLER0 + 4 ... GICR_ICENABLER0 + 8:
    case GICR_ISPENDR0 + 4 ... GICR_ISPENDR0 + 8:
    case GICR_ICPENDR0 + 4 ... GICR_ICPENDR0 + 8:
    case GICR_ISACTIVER0 + 4 ... GICR_ISACTIVER0 + 8:
    case GICR_ICACTIVER0 + 4 ... GICR_ICACTIVER0 + 8:
    {
        unsigned int word;
        hwaddr base = offset & ~0x7f;
        uint32_t value;

        if (!gicr_eppi_word_index(cs, offset, base, &word)) {
            *data = 0;
            return MEMTX_OK;
        }
        switch (base) {
        case GICR_ISENABLER0:
        case GICR_ICENABLER0:
            value = cs->eppi_enabled[word];
            break;
        case GICR_ISPENDR0:
        case GICR_ICPENDR0:
            value = cs->eppi_pending[word] |
                (~cs->eppi_edge_trigger[word] & cs->eppi_level[word]);
            break;
        case GICR_ISACTIVER0:
        case GICR_ICACTIVER0:
            value = cs->eppi_active[word];
            break;
        default:
            g_assert_not_reached();
        }
        *data = gicr_eppi_read_bitmap(cs, attrs, value, word);
        return MEMTX_OK;
    }
    case GICR_IPRIORITYR ... GICR_IPRIORITYR + 0x5f:
    {
        int i, irq = offset - GICR_IPRIORITYR;
        uint32_t value = 0;

        for (i = irq + 3; i >= irq; i--) {
            value <<= 8;
            if (i < GIC_INTERNAL) {
                value |= gicr_read_ipriorityr(cs, attrs, i);
            } else if (i - GIC_INTERNAL < cs->gic->num_eppi) {
                value |= gicr_read_eppi_priority(cs, attrs,
                                                 i - GIC_INTERNAL);
            }
        }
        *data = value;
        return MEMTX_OK;
    }
    case GICR_INMIR0:
        *data = cs->gic->nmi_support ?
                gicr_read_bitmap_reg(cs, attrs, cs->gicr_inmir0) : 0;
        return MEMTX_OK;
    case GICR_ICFGR0:
    case GICR_ICFGR1:
    {
        /* Our edge_trigger bitmap is one bit per irq; take the correct
         * half of it, and spread it out into the odd bits.
         */
        uint32_t value;

        value = cs->edge_trigger & mask_group(cs, attrs);
        value = extract32(value, (offset == GICR_ICFGR1) ? 16 : 0, 16);
        value = half_shuffle32(value) << 1;
        *data = value;
        return MEMTX_OK;
    }
    case GICR_ICFGR0 + 8 ... GICR_ICFGR0 + 20:
    {
        unsigned int irq = ((offset - GICR_ICFGR0) / 4 - 2) * 16;
        unsigned int word = irq / 32;
        uint32_t value;

        if (irq >= cs->gic->num_eppi) {
            *data = 0;
            return MEMTX_OK;
        }
        value = cs->eppi_edge_trigger[word] &
            gicr_eppi_group_mask(cs, attrs, word);
        value = extract32(value, irq % 32, 16);
        *data = half_shuffle32(value) << 1;
        return MEMTX_OK;
    }
    case GICR_IGRPMODR0:
        if ((cs->gic->gicd_ctlr & GICD_CTLR_DS) || !attrs.secure) {
            /* RAZ/WI if security disabled, or if
             * security enabled and this is an NS access
             */
            *data = 0;
            return MEMTX_OK;
        }
        *data = cs->gicr_igrpmodr0;
        return MEMTX_OK;
    case GICR_IGRPMODR0 + 4 ... GICR_IGRPMODR0 + 8:
    {
        unsigned int word;

        if (!gicr_eppi_word_index(cs, offset, GICR_IGRPMODR0, &word) ||
            (cs->gic->gicd_ctlr & GICD_CTLR_DS) || !attrs.secure) {
            *data = 0;
        } else {
            *data = cs->eppi_grpmod[word];
        }
        return MEMTX_OK;
    }
    case GICR_NSACR:
        if ((cs->gic->gicd_ctlr & GICD_CTLR_DS) || !attrs.secure) {
            /* RAZ/WI if security disabled, or if
             * security enabled and this is an NS access
             */
            *data = 0;
            return MEMTX_OK;
        }
        *data = cs->gicr_nsacr;
        return MEMTX_OK;
    case GICR_IDREGS ... GICR_IDREGS + 0x2f:
        *data = gicv3_idreg(cs->gic, offset - GICR_IDREGS, GICV3_PIDR0_REDIST);
        return MEMTX_OK;
        /*
         * VLPI frame registers. We don't need a version check for
         * VPROPBASER and VPENDBASER because gicv3_redist_size() will
         * prevent pre-v4 GIC from passing us offsets this high.
         */
    case GICR_VPROPBASER:
        *data = extract64(cs->gicr_vpropbaser, 0, 32);
        return MEMTX_OK;
    case GICR_VPROPBASER + 4:
        *data = extract64(cs->gicr_vpropbaser, 32, 32);
        return MEMTX_OK;
    case GICR_VPENDBASER:
        *data = extract64(cs->gicr_vpendbaser, 0, 32);
        return MEMTX_OK;
    case GICR_VPENDBASER + 4:
        *data = extract64(cs->gicr_vpendbaser, 32, 32);
        return MEMTX_OK;
    default:
        return MEMTX_ERROR;
    }
}

static MemTxResult gicr_writel(GICv3CPUState *cs, hwaddr offset,
                               uint64_t value, MemTxAttrs attrs)
{
    switch (offset) {
    case GICR_CTLR:
        /* For our implementation, GICR_TYPER.DPGS is 0 and so all
         * the DPG bits are RAZ/WI. We don't do anything asynchronously,
         * so UWP and RWP are RAZ/WI. GICR_TYPER.LPIS is 1 (we
         * implement LPIs) so Enable_LPIs is programmable.
         */
        if (cs->gicr_typer & GICR_TYPER_PLPIS) {
            if (value & GICR_CTLR_ENABLE_LPIS) {
                cs->gicr_ctlr |= GICR_CTLR_ENABLE_LPIS;
                /* Check for any pending interr in pending table */
                gicv3_redist_update_lpi(cs);
            } else {
                cs->gicr_ctlr &= ~GICR_CTLR_ENABLE_LPIS;
                /* cs->hppi might have been an LPI; recalculate */
                gicv3_redist_update(cs);
            }
        }
        return MEMTX_OK;
    case GICR_STATUSR:
        /* RAZ/WI for our implementation */
        return MEMTX_OK;
    case GICR_SETLPIR:
        gicr_write_lpir(cs, value, 1);
        return MEMTX_OK;
    case GICR_CLRLPIR:
        gicr_write_lpir(cs, value, 0);
        return MEMTX_OK;
    case GICR_WAKER:
        /* Only the ProcessorSleep bit is writable. When the guest sets
         * it, it requests that we transition the channel between the
         * redistributor and the cpu interface to quiescent, and that
         * we set the ChildrenAsleep bit once the interface has reached the
         * quiescent state.
         * Setting the ProcessorSleep to 0 reverses the quiescing, and
         * ChildrenAsleep is cleared once the transition is complete.
         * Since our interface is not asynchronous, we complete these
         * transitions instantaneously, so we set ChildrenAsleep to the
         * same value as ProcessorSleep here.
         */
        value &= GICR_WAKER_ProcessorSleep;
        if (value & GICR_WAKER_ProcessorSleep) {
            value |= GICR_WAKER_ChildrenAsleep;
        }
        cs->gicr_waker = value;
        return MEMTX_OK;
    case GICR_PROPBASER:
        cs->gicr_propbaser = deposit64(cs->gicr_propbaser, 0, 32, value);
        return MEMTX_OK;
    case GICR_PROPBASER + 4:
        cs->gicr_propbaser = deposit64(cs->gicr_propbaser, 32, 32, value);
        return MEMTX_OK;
    case GICR_PENDBASER:
        cs->gicr_pendbaser = deposit64(cs->gicr_pendbaser, 0, 32, value);
        return MEMTX_OK;
    case GICR_PENDBASER + 4:
        cs->gicr_pendbaser = deposit64(cs->gicr_pendbaser, 32, 32, value);
        return MEMTX_OK;
    case GICR_IGROUPR0:
        if (!attrs.secure && !(cs->gic->gicd_ctlr & GICD_CTLR_DS)) {
            return MEMTX_OK;
        }
        cs->gicr_igroupr0 = value;
        gicv3_redist_update(cs);
        return MEMTX_OK;
    case GICR_IGROUPR0 + 4 ... GICR_IGROUPR0 + 8:
    {
        unsigned int word;

        if (gicr_eppi_word_index(cs, offset, GICR_IGROUPR0, &word) &&
            (attrs.secure || (cs->gic->gicd_ctlr & GICD_CTLR_DS))) {
            cs->eppi_group[word] = value;
            gicv3_redist_update(cs);
        }
        return MEMTX_OK;
    }
    case GICR_ISENABLER0:
        gicr_write_set_bitmap_reg(cs, attrs, &cs->gicr_ienabler0, value);
        return MEMTX_OK;
    case GICR_ICENABLER0:
        gicr_write_clear_bitmap_reg(cs, attrs, &cs->gicr_ienabler0, value);
        return MEMTX_OK;
    case GICR_ISPENDR0:
        gicr_write_set_bitmap_reg(cs, attrs, &cs->gicr_ipendr0, value);
        return MEMTX_OK;
    case GICR_ICPENDR0:
        gicr_write_clear_bitmap_reg(cs, attrs, &cs->gicr_ipendr0, value);
        return MEMTX_OK;
    case GICR_ISACTIVER0:
        gicr_write_set_bitmap_reg(cs, attrs, &cs->gicr_iactiver0, value);
        return MEMTX_OK;
    case GICR_ICACTIVER0:
        gicr_write_clear_bitmap_reg(cs, attrs, &cs->gicr_iactiver0, value);
        return MEMTX_OK;
    case GICR_ISENABLER0 + 4 ... GICR_ISENABLER0 + 8:
    case GICR_ICENABLER0 + 4 ... GICR_ICENABLER0 + 8:
    case GICR_ISPENDR0 + 4 ... GICR_ISPENDR0 + 8:
    case GICR_ICPENDR0 + 4 ... GICR_ICPENDR0 + 8:
    case GICR_ISACTIVER0 + 4 ... GICR_ISACTIVER0 + 8:
    case GICR_ICACTIVER0 + 4 ... GICR_ICACTIVER0 + 8:
    {
        unsigned int word;
        hwaddr base = offset & ~0x7f;
        uint32_t *reg;

        if (!gicr_eppi_word_index(cs, offset, base, &word)) {
            return MEMTX_OK;
        }
        switch (base) {
        case GICR_ISENABLER0:
        case GICR_ICENABLER0:
            reg = &cs->eppi_enabled[word];
            break;
        case GICR_ISPENDR0:
        case GICR_ICPENDR0:
            reg = &cs->eppi_pending[word];
            break;
        case GICR_ISACTIVER0:
        case GICR_ICACTIVER0:
            reg = &cs->eppi_active[word];
            break;
        default:
            g_assert_not_reached();
        }
        if ((base & 0x80) == 0) {
            gicr_eppi_write_set_bitmap(cs, attrs, reg, value, word);
        } else {
            gicr_eppi_write_clear_bitmap(cs, attrs, reg, value, word);
        }
        return MEMTX_OK;
    }
    case GICR_IPRIORITYR ... GICR_IPRIORITYR + 0x5f:
    {
        int i, irq = offset - GICR_IPRIORITYR;

        for (i = irq; i < irq + 4; i++, value >>= 8) {
            if (i < GIC_INTERNAL) {
                gicr_write_ipriorityr(cs, attrs, i, value);
            } else if (i - GIC_INTERNAL < cs->gic->num_eppi) {
                gicr_write_eppi_priority(cs, attrs, i - GIC_INTERNAL,
                                         value);
            }
        }
        gicv3_redist_update(cs);
        return MEMTX_OK;
    }
    case GICR_INMIR0:
        if (cs->gic->nmi_support) {
            gicr_write_bitmap_reg(cs, attrs, &cs->gicr_inmir0, value);
        }
        return MEMTX_OK;

    case GICR_ICFGR0:
        /* Register is all RAZ/WI or RAO/WI bits */
        return MEMTX_OK;
    case GICR_ICFGR1:
    {
        uint32_t mask;

        /* Since our edge_trigger bitmap is one bit per irq, our input
         * 32-bits will compress down into 16 bits which we need
         * to write into the bitmap.
         */
        value = half_unshuffle32(value >> 1) << 16;
        mask = mask_group(cs, attrs) & 0xffff0000U;

        cs->edge_trigger &= ~mask;
        cs->edge_trigger |= (value & mask);

        gicv3_redist_update(cs);
        return MEMTX_OK;
    }
    case GICR_ICFGR0 + 8 ... GICR_ICFGR0 + 20:
    {
        unsigned int irq = ((offset - GICR_ICFGR0) / 4 - 2) * 16;
        unsigned int word = irq / 32;
        uint32_t mask;

        if (irq >= cs->gic->num_eppi) {
            return MEMTX_OK;
        }
        value = half_unshuffle32(value >> 1) << (irq % 32);
        mask = (0xffffU << (irq % 32)) &
            gicr_eppi_group_mask(cs, attrs, word);
        cs->eppi_edge_trigger[word] &= ~mask;
        cs->eppi_edge_trigger[word] |= value & mask;
        gicv3_redist_update(cs);
        return MEMTX_OK;
    }
    case GICR_IGRPMODR0:
        if ((cs->gic->gicd_ctlr & GICD_CTLR_DS) || !attrs.secure) {
            /* RAZ/WI if security disabled, or if
             * security enabled and this is an NS access
             */
            return MEMTX_OK;
        }
        cs->gicr_igrpmodr0 = value;
        gicv3_redist_update(cs);
        return MEMTX_OK;
    case GICR_IGRPMODR0 + 4 ... GICR_IGRPMODR0 + 8:
    {
        unsigned int word;

        if (gicr_eppi_word_index(cs, offset, GICR_IGRPMODR0, &word) &&
            !(cs->gic->gicd_ctlr & GICD_CTLR_DS) && attrs.secure) {
            cs->eppi_grpmod[word] = value;
            gicv3_redist_update(cs);
        }
        return MEMTX_OK;
    }
    case GICR_NSACR:
        if ((cs->gic->gicd_ctlr & GICD_CTLR_DS) || !attrs.secure) {
            /* RAZ/WI if security disabled, or if
             * security enabled and this is an NS access
             */
            return MEMTX_OK;
        }
        cs->gicr_nsacr = value;
        /* no update required as this only affects access permission checks */
        return MEMTX_OK;
    case GICR_IIDR:
    case GICR_TYPER:
    case GICR_IDREGS ... GICR_IDREGS + 0x2f:
        /* RO registers, ignore the write */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: invalid guest write to RO register at offset "
                      HWADDR_FMT_plx "\n", __func__, offset);
        return MEMTX_OK;
        /*
         * VLPI frame registers. We don't need a version check for
         * VPROPBASER and VPENDBASER because gicv3_redist_size() will
         * prevent pre-v4 GIC from passing us offsets this high.
         */
    case GICR_VPROPBASER:
        cs->gicr_vpropbaser = deposit64(cs->gicr_vpropbaser, 0, 32, value);
        return MEMTX_OK;
    case GICR_VPROPBASER + 4:
        cs->gicr_vpropbaser = deposit64(cs->gicr_vpropbaser, 32, 32, value);
        return MEMTX_OK;
    case GICR_VPENDBASER:
        gicr_write_vpendbaser(cs, deposit64(cs->gicr_vpendbaser, 0, 32, value));
        return MEMTX_OK;
    case GICR_VPENDBASER + 4:
        gicr_write_vpendbaser(cs, deposit64(cs->gicr_vpendbaser, 32, 32, value));
        return MEMTX_OK;
    default:
        return MEMTX_ERROR;
    }
}

static MemTxResult gicr_readll(GICv3CPUState *cs, hwaddr offset,
                               uint64_t *data, MemTxAttrs attrs)
{
    switch (offset) {
    case GICR_TYPER:
        *data = cs->gicr_typer;
        return MEMTX_OK;
    case GICR_PROPBASER:
        *data = cs->gicr_propbaser;
        return MEMTX_OK;
    case GICR_PENDBASER:
        *data = cs->gicr_pendbaser;
        return MEMTX_OK;
        /*
         * VLPI frame registers. We don't need a version check for
         * VPROPBASER and VPENDBASER because gicv3_redist_size() will
         * prevent pre-v4 GIC from passing us offsets this high.
         */
    case GICR_VPROPBASER:
        *data = cs->gicr_vpropbaser;
        return MEMTX_OK;
    case GICR_VPENDBASER:
        *data = cs->gicr_vpendbaser;
        return MEMTX_OK;
    default:
        return MEMTX_ERROR;
    }
}

static MemTxResult gicr_writell(GICv3CPUState *cs, hwaddr offset,
                                uint64_t value, MemTxAttrs attrs)
{
    switch (offset) {
    case GICR_PROPBASER:
        cs->gicr_propbaser = value;
        return MEMTX_OK;
    case GICR_PENDBASER:
        cs->gicr_pendbaser = value;
        return MEMTX_OK;
    case GICR_SETLPIR:
        gicr_write_lpir(cs, value, 1);
        return MEMTX_OK;
    case GICR_CLRLPIR:
        gicr_write_lpir(cs, value, 0);
        return MEMTX_OK;
    case GICR_TYPER:
        /* RO register, ignore the write */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: invalid guest write to RO register at offset "
                      HWADDR_FMT_plx "\n", __func__, offset);
        return MEMTX_OK;
        /*
         * VLPI frame registers. We don't need a version check for
         * VPROPBASER and VPENDBASER because gicv3_redist_size() will
         * prevent pre-v4 GIC from passing us offsets this high.
         */
    case GICR_VPROPBASER:
        cs->gicr_vpropbaser = value;
        return MEMTX_OK;
    case GICR_VPENDBASER:
        gicr_write_vpendbaser(cs, value);
        return MEMTX_OK;
    default:
        return MEMTX_ERROR;
    }
}

MemTxResult gicv3_redist_read(void *opaque, hwaddr offset, uint64_t *data,
                              unsigned size, MemTxAttrs attrs)
{
    GICv3RedistRegion *region = opaque;
    GICv3State *s = region->gic;
    GICv3CPUState *cs;
    MemTxResult r;
    int cpuidx;

    assert((offset & (size - 1)) == 0);

    /*
     * There are (for GICv3) two 64K redistributor pages per CPU.
     * In some cases the redistributor pages for all CPUs are not
     * contiguous (eg on the virt board they are split into two
     * parts if there are too many CPUs to all fit in the same place
     * in the memory map); if so then the GIC has multiple MemoryRegions
     * for the redistributors.
     */
    cpuidx = region->cpuidx + offset / gicv3_redist_size(s);
    offset %= gicv3_redist_size(s);

    cs = &s->cpu[cpuidx];

    switch (size) {
    case 1:
        r = gicr_readb(cs, offset, data, attrs);
        break;
    case 4:
        r = gicr_readl(cs, offset, data, attrs);
        break;
    case 8:
        r = gicr_readll(cs, offset, data, attrs);
        break;
    default:
        r = MEMTX_ERROR;
        break;
    }

    if (r != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: invalid guest read at offset " HWADDR_FMT_plx
                      " size %u\n", __func__, offset, size);
        trace_gicv3_redist_badread(gicv3_redist_affid(cs), offset,
                                   size, attrs.secure);
        /* The spec requires that reserved registers are RAZ/WI;
         * so use MEMTX_ERROR returns from leaf functions as a way to
         * trigger the guest-error logging but don't return it to
         * the caller, or we'll cause a spurious guest data abort.
         */
        r = MEMTX_OK;
        *data = 0;
    } else {
        trace_gicv3_redist_read(gicv3_redist_affid(cs), offset, *data,
                                size, attrs.secure);
    }
    return r;
}

MemTxResult gicv3_redist_write(void *opaque, hwaddr offset, uint64_t data,
                               unsigned size, MemTxAttrs attrs)
{
    GICv3RedistRegion *region = opaque;
    GICv3State *s = region->gic;
    GICv3CPUState *cs;
    MemTxResult r;
    int cpuidx;

    assert((offset & (size - 1)) == 0);

    /*
     * There are (for GICv3) two 64K redistributor pages per CPU.
     * In some cases the redistributor pages for all CPUs are not
     * contiguous (eg on the virt board they are split into two
     * parts if there are too many CPUs to all fit in the same place
     * in the memory map); if so then the GIC has multiple MemoryRegions
     * for the redistributors.
     */
    cpuidx = region->cpuidx + offset / gicv3_redist_size(s);
    offset %= gicv3_redist_size(s);

    cs = &s->cpu[cpuidx];

    switch (size) {
    case 1:
        r = gicr_writeb(cs, offset, data, attrs);
        break;
    case 4:
        r = gicr_writel(cs, offset, data, attrs);
        break;
    case 8:
        r = gicr_writell(cs, offset, data, attrs);
        break;
    default:
        r = MEMTX_ERROR;
        break;
    }

    if (r != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: invalid guest write at offset " HWADDR_FMT_plx
                      " size %u\n", __func__, offset, size);
        trace_gicv3_redist_badwrite(gicv3_redist_affid(cs), offset, data,
                                    size, attrs.secure);
        /* The spec requires that reserved registers are RAZ/WI;
         * so use MEMTX_ERROR returns from leaf functions as a way to
         * trigger the guest-error logging but don't return it to
         * the caller, or we'll cause a spurious guest data abort.
         */
        r = MEMTX_OK;
    } else {
        trace_gicv3_redist_write(gicv3_redist_affid(cs), offset, data,
                                 size, attrs.secure);
    }
    return r;
}

static void gicv3_redist_check_lpi_priority(GICv3CPUState *cs, int irq)
{
    uint64_t lpict_baddr = cs->gicr_propbaser & R_GICR_PROPBASER_PHYADDR_MASK;

    update_for_one_lpi(cs, irq, lpict_baddr,
                       cs->gic->gicd_ctlr & GICD_CTLR_DS,
                       &cs->hpplpi);
}

void gicv3_redist_update_lpi_only(GICv3CPUState *cs)
{
    /*
     * This function scans the LPI pending table and for each pending
     * LPI, reads the corresponding entry from LPI configuration table
     * to extract the priority info and determine if the current LPI
     * priority is lower than the last computed high priority lpi interrupt.
     * If yes, replace current LPI as the new high priority lpi interrupt.
     */
    uint64_t lpipt_baddr, lpict_baddr;
    uint64_t idbits;

    idbits = MIN(FIELD_EX64(cs->gicr_propbaser, GICR_PROPBASER, IDBITS),
                 GICD_TYPER_IDBITS);

    if (!(cs->gicr_ctlr & GICR_CTLR_ENABLE_LPIS)) {
        return;
    }

    lpipt_baddr = cs->gicr_pendbaser & R_GICR_PENDBASER_PHYADDR_MASK;
    lpict_baddr = cs->gicr_propbaser & R_GICR_PROPBASER_PHYADDR_MASK;

    update_for_all_lpis(cs, lpipt_baddr, lpict_baddr, idbits,
                        cs->gic->gicd_ctlr & GICD_CTLR_DS, &cs->hpplpi);
}

void gicv3_redist_update_lpi(GICv3CPUState *cs)
{
    gicv3_redist_update_lpi_only(cs);
    gicv3_redist_update(cs);
}

void gicv3_redist_lpi_pending(GICv3CPUState *cs, int irq, int level)
{
    /*
     * This function updates the pending bit in lpi pending table for
     * the irq being activated or deactivated.
     */
    uint64_t lpipt_baddr;

    lpipt_baddr = cs->gicr_pendbaser & R_GICR_PENDBASER_PHYADDR_MASK;
    if (!set_pending_table_bit(cs, lpipt_baddr, irq, level)) {
        /* no change in the value of pending bit, return */
        return;
    }

    /*
     * check if this LPI is better than the current hpplpi, if yes
     * just set hpplpi.prio and .irq without doing a full rescan
     */
    if (level) {
        gicv3_redist_check_lpi_priority(cs, irq);
        gicv3_redist_update(cs);
    } else {
        if (irq == cs->hpplpi.irq) {
            gicv3_redist_update_lpi(cs);
        }
    }
}

void gicv3_redist_process_lpi(GICv3CPUState *cs, int irq, int level)
{
    uint64_t idbits;

    idbits = MIN(FIELD_EX64(cs->gicr_propbaser, GICR_PROPBASER, IDBITS),
                 GICD_TYPER_IDBITS);

    if (!(cs->gicr_ctlr & GICR_CTLR_ENABLE_LPIS) ||
        (irq > (1ULL << (idbits + 1)) - 1) || irq < GICV3_LPI_INTID_START) {
        return;
    }

    /* set/clear the pending bit for this irq */
    gicv3_redist_lpi_pending(cs, irq, level);
}

void gicv3_redist_inv_lpi(GICv3CPUState *cs, int irq)
{
    /*
     * The only cached information for LPIs we have is the HPPLPI.
     * We could be cleverer about identifying when we don't need
     * to do a full rescan of the pending table, but until we find
     * this is a performance issue, just always recalculate.
     */
    gicv3_redist_update_lpi(cs);
}

void gicv3_redist_mov_lpi(GICv3CPUState *src, GICv3CPUState *dest, int irq)
{
    /*
     * Move the specified LPI's pending state from the source redistributor
     * to the destination.
     *
     * If LPIs are disabled on dest this is CONSTRAINED UNPREDICTABLE:
     * we choose to NOP. If LPIs are disabled on source there's nothing
     * to be transferred anyway.
     */
    uint64_t idbits;
    uint32_t pendt_size;
    uint64_t src_baddr;

    if (!(src->gicr_ctlr & GICR_CTLR_ENABLE_LPIS) ||
        !(dest->gicr_ctlr & GICR_CTLR_ENABLE_LPIS)) {
        return;
    }

    idbits = MIN(FIELD_EX64(src->gicr_propbaser, GICR_PROPBASER, IDBITS),
                 GICD_TYPER_IDBITS);
    idbits = MIN(FIELD_EX64(dest->gicr_propbaser, GICR_PROPBASER, IDBITS),
                 idbits);

    pendt_size = 1ULL << (idbits + 1);
    if ((irq / 8) >= pendt_size) {
        return;
    }

    src_baddr = src->gicr_pendbaser & R_GICR_PENDBASER_PHYADDR_MASK;

    if (!set_pending_table_bit(src, src_baddr, irq, 0)) {
        /* Not pending on source, nothing to do */
        return;
    }
    if (irq == src->hpplpi.irq) {
        /*
         * We just made this LPI not-pending so only need to update
         * if it was previously the highest priority pending LPI
         */
        gicv3_redist_update_lpi(src);
    }
    /* Mark it pending on the destination */
    gicv3_redist_lpi_pending(dest, irq, 1);
}

void gicv3_redist_movall_lpis(GICv3CPUState *src, GICv3CPUState *dest)
{
    /*
     * We must move all pending LPIs from the source redistributor
     * to the destination. That is, for every pending LPI X on
     * src, we must set it not-pending on src and pending on dest.
     * LPIs that are already pending on dest are not cleared.
     *
     * If LPIs are disabled on dest this is CONSTRAINED UNPREDICTABLE:
     * we choose to NOP. If LPIs are disabled on source there's nothing
     * to be transferred anyway.
     */
    AddressSpace *as = &src->gic->dma_as;
    uint64_t idbits;
    uint32_t pendt_size;
    uint64_t src_baddr, dest_baddr;
    int i;

    if (!(src->gicr_ctlr & GICR_CTLR_ENABLE_LPIS) ||
        !(dest->gicr_ctlr & GICR_CTLR_ENABLE_LPIS)) {
        return;
    }

    idbits = MIN(FIELD_EX64(src->gicr_propbaser, GICR_PROPBASER, IDBITS),
                 GICD_TYPER_IDBITS);
    idbits = MIN(FIELD_EX64(dest->gicr_propbaser, GICR_PROPBASER, IDBITS),
                 idbits);

    pendt_size = 1ULL << (idbits + 1);
    src_baddr = src->gicr_pendbaser & R_GICR_PENDBASER_PHYADDR_MASK;
    dest_baddr = dest->gicr_pendbaser & R_GICR_PENDBASER_PHYADDR_MASK;

    for (i = GICV3_LPI_INTID_START / 8; i < pendt_size / 8; i++) {
        uint8_t src_pend, dest_pend;

        address_space_read(as, src_baddr + i, MEMTXATTRS_UNSPECIFIED,
                           &src_pend, sizeof(src_pend));
        if (!src_pend) {
            continue;
        }
        address_space_read(as, dest_baddr + i, MEMTXATTRS_UNSPECIFIED,
                           &dest_pend, sizeof(dest_pend));
        dest_pend |= src_pend;
        src_pend = 0;
        address_space_write(as, src_baddr + i, MEMTXATTRS_UNSPECIFIED,
                            &src_pend, sizeof(src_pend));
        address_space_write(as, dest_baddr + i, MEMTXATTRS_UNSPECIFIED,
                            &dest_pend, sizeof(dest_pend));
    }

    gicv3_redist_update_lpi(src);
    gicv3_redist_update_lpi(dest);
}

void gicv3_redist_vlpi_pending(GICv3CPUState *cs, int irq, int level)
{
    /*
     * Change the pending state of the specified vLPI.
     * Unlike gicv3_redist_process_vlpi(), we know here that the
     * vCPU is definitely resident on this redistributor, and that
     * the irq is in range.
     */
    uint64_t vptbase, ctbase;

    if (vpendbaser_uses_vpeid(cs)) {
        vptbase = cs->gicr_vpend_vptaddr;
        if (!vptbase) {
            return;
        }
    } else {
        vptbase = FIELD_EX64(cs->gicr_vpendbaser,
                             GICR_VPENDBASER, PHYADDR) << 16;
    }

    if (set_pending_table_bit(cs, vptbase, irq, level)) {
        gicr_vpendbaser_mark_dirty(cs);
        if (level) {
            /* Check whether this vLPI is now the best */
            ctbase = vpendbaser_uses_vpeid(cs) &&
                cs->gicr_vpend_vconfaddr ?
                cs->gicr_vpend_vconfaddr :
                cs->gicr_vpropbaser & R_GICR_VPROPBASER_PHYADDR_MASK;
            update_for_one_lpi(cs, irq, ctbase, true, &cs->hppvlpi);
            gicv3_cpuif_virt_irq_fiq_update(cs);
        } else {
            /* Only need to recalculate if this was previously the best vLPI */
            if (irq == cs->hppvlpi.irq) {
                gicv3_redist_update_vlpi(cs);
            }
        }
    }
}

void gicv3_redist_process_vlpi(GICv3CPUState *cs, int irq, uint64_t vptaddr,
                               uint64_t vconfaddr, uint32_t vpeid,
                               int doorbell, int level)
{
    bool bit_changed;
    bool resident = vcpu_resident(cs, vptaddr, vpeid);
    uint64_t ctbase;

    if (resident) {
        uint32_t idbits = FIELD_EX64(cs->gicr_vpropbaser, GICR_VPROPBASER, IDBITS);

        cs->gicr_vpend_vptaddr = vptaddr;
        cs->gicr_vpend_vconfaddr = vconfaddr;
        if (irq >= (1ULL << (idbits + 1))) {
            return;
        }
    }

    bit_changed = set_pending_table_bit(cs, vptaddr, irq, level);
    if (resident && bit_changed) {
        gicr_vpendbaser_mark_dirty(cs);
        if (level) {
            /* Check whether this vLPI is now the best */
            ctbase = cs->gicr_vpend_vconfaddr ?
                cs->gicr_vpend_vconfaddr :
                cs->gicr_vpropbaser & R_GICR_VPROPBASER_PHYADDR_MASK;
            update_for_one_lpi(cs, irq, ctbase, true, &cs->hppvlpi);
            gicv3_cpuif_virt_irq_fiq_update(cs);
        } else {
            /* Only need to recalculate if this was previously the best vLPI */
            if (irq == cs->hppvlpi.irq) {
                gicv3_redist_update_vlpi(cs);
            }
        }
    }

    if (!resident && level && doorbell != INTID_SPURIOUS &&
        (cs->gicr_ctlr & GICR_CTLR_ENABLE_LPIS)) {
        /* vCPU is not currently resident: ring the doorbell */
        gicv3_redist_process_lpi(cs, doorbell, 1);
    }
}

void gicv3_redist_mov_vlpi(GICv3CPUState *src, uint64_t src_vptaddr,
                           uint32_t src_vpeid, GICv3CPUState *dest,
                           uint64_t dest_vptaddr, uint64_t dest_vconfaddr,
                           uint32_t dest_vpeid, int irq, int doorbell)
{
    bool src_resident;

    /*
     * Move the specified vLPI's pending state from the source redistributor
     * to the destination.
     */
    if (!set_pending_table_bit(src, src_vptaddr, irq, 0)) {
        /* Not pending on source, nothing to do */
        return;
    }
    src_resident = vcpu_resident(src, src_vptaddr, src_vpeid);
    if (src_resident) {
        gicr_vpendbaser_mark_dirty(src);
    }
    if (src_resident && irq == src->hppvlpi.irq) {
        /*
         * Update src's cached highest-priority pending vLPI if we just made
         * it not-pending
         */
        gicv3_redist_update_vlpi(src);
    }
    /*
     * Mark the vLPI pending on the destination (ringing the doorbell
     * if the vCPU isn't resident)
     */
    gicv3_redist_process_vlpi(dest, irq, dest_vptaddr, dest_vconfaddr,
                              dest_vpeid, doorbell, irq);
}

void gicv3_redist_vinvall(GICv3CPUState *cs, uint64_t vptaddr,
                          uint32_t vpeid)
{
    if (!vcpu_resident(cs, vptaddr, vpeid)) {
        /* We don't have anything cached if the vCPU isn't resident */
        return;
    }

    /* Otherwise, our only cached information is the HPPVLPI info */
    gicv3_redist_update_vlpi(cs);
}

void gicv3_redist_inv_vlpi(GICv3CPUState *cs, int irq, uint64_t vptaddr,
                           uint32_t vpeid)
{
    /*
     * The only cached information for LPIs we have is the HPPLPI.
     * We could be cleverer about identifying when we don't need
     * to do a full rescan of the pending table, but until we find
     * this is a performance issue, just always recalculate.
     */
    gicv3_redist_vinvall(cs, vptaddr, vpeid);
}

void gicv3_redist_set_irq(GICv3CPUState *cs, int irq, int level)
{
    /* Update redistributor state for a change in an external PPI input line */
    if (level == extract32(cs->level, irq, 1)) {
        return;
    }

    trace_gicv3_redist_set_irq(gicv3_redist_affid(cs), irq, level);

    cs->level = deposit32(cs->level, irq, 1, level);

    if (level) {
        /* 0->1 edges latch the pending bit for edge-triggered interrupts */
        if (extract32(cs->edge_trigger, irq, 1)) {
            cs->gicr_ipendr0 = deposit32(cs->gicr_ipendr0, irq, 1, 1);
        }
    }

    gicv3_redist_update(cs);
}

void gicv3_redist_send_sgi(GICv3CPUState *cs, int grp, int irq, bool ns)
{
    /* Update redistributor state for a generated SGI */
    int irqgrp = gicv3_irq_group(cs->gic, cs, irq);

    /* If we are asked for a Secure Group 1 SGI and it's actually
     * configured as Secure Group 0 this is OK (subject to the usual
     * NSACR checks).
     */
    if (grp == GICV3_G1 && irqgrp == GICV3_G0) {
        grp = GICV3_G0;
    }

    if (grp != irqgrp) {
        return;
    }

    if (ns && !(cs->gic->gicd_ctlr & GICD_CTLR_DS)) {
        /* If security is enabled we must test the NSACR bits */
        int nsaccess = gicr_ns_access(cs, irq);

        if ((irqgrp == GICV3_G0 && nsaccess < 1) ||
            (irqgrp == GICV3_G1 && nsaccess < 2)) {
            return;
        }
    }

    /* OK, we can accept the SGI */
    trace_gicv3_redist_send_sgi(gicv3_redist_affid(cs), irq);
    cs->gicr_ipendr0 = deposit32(cs->gicr_ipendr0, irq, 1, 1);
    gicv3_redist_update(cs);
}
