/*
 * QBox virtual PCIe endpoint controller and test endpoint
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/pci/msi.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pcie.h"
#include "qapi/error.h"
#include "qemu/bitops.h"
#include "qemu/module.h"
#include "system/memory.h"

#define TYPE_QBOX_PCIE_EPC "qbox-pcie-epc"
OBJECT_DECLARE_SIMPLE_TYPE(QBoxPcieEpcState, QBOX_PCIE_EPC)

#define TYPE_QBOX_PCIE_TEST_EP "qbox-pcie-test-ep"
OBJECT_DECLARE_SIMPLE_TYPE(QBoxPcieTestEpState, QBOX_PCIE_TEST_EP)

#define QBOX_PCIE_EPC_ID              0x51455043
#define QBOX_PCIE_EPC_VERSION         0x00010000
#define QBOX_PCIE_EPC_REG_SIZE        0x1000
#define QBOX_PCIE_EPC_OUTBOUND_SIZE   0x400000
#define QBOX_PCIE_EPC_BAR0_SIZE       0x10000
#define QBOX_PCIE_EPC_OB_WINDOWS      2
#define QBOX_PCIE_EPC_MSI_MAX         32

#define EPC_REG_ID                    0x000
#define EPC_REG_VERSION               0x004
#define EPC_REG_STATUS                0x008
#define EPC_REG_COMMAND               0x00c
#define EPC_REG_VENDOR_DEVICE         0x020
#define EPC_REG_REV_CLASS             0x024
#define EPC_REG_SUBSYS_VENDOR_DEVICE  0x028
#define EPC_REG_BAR0_PHYS_LO          0x100
#define EPC_REG_BAR0_PHYS_HI          0x104
#define EPC_REG_BAR0_SIZE             0x108
#define EPC_REG_BAR0_CTRL             0x10c
#define EPC_REG_MSI_REQUEST_COUNT     0x200
#define EPC_REG_MSI_ENABLED_COUNT     0x204
#define EPC_REG_MSI_RAISE             0x208
#define EPC_REG_OB_BASE               0x300
#define EPC_REG_OB_STRIDE             0x020
#define EPC_REG_LAST_ERROR            0x400

#define EPC_STATUS_LINK_PRESENT       BIT(0)
#define EPC_STATUS_LINK_STARTED       BIT(1)
#define EPC_STATUS_MSI_ENABLED        BIT(2)
#define EPC_STATUS_ERROR              BIT(31)

#define EPC_COMMAND_START             BIT(0)
#define EPC_COMMAND_STOP              BIT(1)
#define EPC_COMMAND_CLEAR_ERROR       BIT(31)

#define EPC_BAR_ENABLE                BIT(0)
#define EPC_OB_ENABLE                 BIT(0)

enum QBoxPcieEpcError {
    EPC_ERROR_NONE = 0,
    EPC_ERROR_NOT_LINKED = 1,
    EPC_ERROR_BAD_HEADER = 2,
    EPC_ERROR_BAD_BAR = 3,
    EPC_ERROR_BAD_MSI = 4,
    EPC_ERROR_BAD_OUTBOUND = 5,
    EPC_ERROR_OUTBOUND_ACCESS = 6,
    EPC_ERROR_BAD_REGISTER = 7,
};

typedef struct QBoxPcieOutboundWindow {
    uint32_t local_offset;
    uint64_t pci_addr;
    uint32_t size;
    uint32_t ctrl;
} QBoxPcieOutboundWindow;

struct QBoxPcieEpcState {
    SysBusDevice parent_obj;

    MemoryRegion regs;
    MemoryRegion outbound;
    MemoryRegion *local_memory;
    AddressSpace local_as;
    bool local_as_initialized;

    QBoxPcieTestEpState *endpoint;
    bool link_started;
    uint32_t vendor_device;
    uint32_t rev_class;
    uint32_t subsys_vendor_device;
    uint64_t bar0_phys;
    uint32_t bar0_size;
    uint32_t bar0_ctrl;
    uint32_t msi_request_count;
    QBoxPcieOutboundWindow ob[QBOX_PCIE_EPC_OB_WINDOWS];
    uint32_t last_error;
};

struct QBoxPcieTestEpState {
    PCIDevice parent_obj;
    MemoryRegion bar0;
    QBoxPcieEpcState *epc;
};

static void qbox_pcie_epc_set_error(QBoxPcieEpcState *s,
                                    enum QBoxPcieEpcError error)
{
    s->last_error = error;
}

static uint32_t qbox_pcie_epc_msi_enabled_count(QBoxPcieEpcState *s)
{
    PCIDevice *pdev;

    if (!s->endpoint) {
        return 0;
    }

    pdev = PCI_DEVICE(s->endpoint);
    return msi_enabled(pdev) ? msi_nr_vectors_allocated(pdev) : 0;
}

static uint32_t qbox_pcie_epc_status(QBoxPcieEpcState *s)
{
    uint32_t status = 0;

    if (s->endpoint) {
        status |= EPC_STATUS_LINK_PRESENT;
    }
    if (s->link_started) {
        status |= EPC_STATUS_LINK_STARTED;
    }
    if (qbox_pcie_epc_msi_enabled_count(s)) {
        status |= EPC_STATUS_MSI_ENABLED;
    }
    if (s->last_error != EPC_ERROR_NONE) {
        status |= EPC_STATUS_ERROR;
    }

    return status;
}

static bool qbox_pcie_epc_valid_msi_count(uint32_t count)
{
    return count && count <= QBOX_PCIE_EPC_MSI_MAX && is_power_of_2(count);
}

static void qbox_pcie_epc_update_header(QBoxPcieEpcState *s)
{
    PCIDevice *pdev;

    if (!s->endpoint) {
        return;
    }

    pdev = PCI_DEVICE(s->endpoint);
    pci_set_word(pdev->config + PCI_VENDOR_ID, s->vendor_device);
    pci_set_word(pdev->config + PCI_DEVICE_ID, s->vendor_device >> 16);
    pdev->config[PCI_REVISION_ID] = s->rev_class;
    pdev->config[PCI_CLASS_PROG] = s->rev_class >> 8;
    pdev->config[PCI_CLASS_DEVICE] = s->rev_class >> 16;
    pdev->config[PCI_CLASS_DEVICE + 1] = s->rev_class >> 24;
    pci_set_word(pdev->config + PCI_SUBSYSTEM_VENDOR_ID,
                 s->subsys_vendor_device);
    pci_set_word(pdev->config + PCI_SUBSYSTEM_ID,
                 s->subsys_vendor_device >> 16);
}

static void qbox_pcie_epc_update_msi_cap(QBoxPcieEpcState *s)
{
    PCIDevice *pdev;
    uint16_t flags;

    if (!s->endpoint || !qbox_pcie_epc_valid_msi_count(
                            s->msi_request_count)) {
        return;
    }

    pdev = PCI_DEVICE(s->endpoint);
    flags = pci_get_word(pdev->config + pdev->msi_cap + PCI_MSI_FLAGS);
    flags &= ~PCI_MSI_FLAGS_QMASK;
    flags |= ctz32(s->msi_request_count) << ctz32(PCI_MSI_FLAGS_QMASK);
    pci_set_word(pdev->config + pdev->msi_cap + PCI_MSI_FLAGS, flags);
}

static bool qbox_pcie_epc_header_valid(QBoxPcieEpcState *s)
{
    uint16_t vendor = s->vendor_device;
    uint16_t device = s->vendor_device >> 16;

    return vendor != 0 && vendor != 0xffff &&
           device != 0 && device != 0xffff;
}

static bool qbox_pcie_epc_bar_valid(QBoxPcieEpcState *s)
{
    return (s->bar0_ctrl & EPC_BAR_ENABLE) &&
           s->bar0_size == QBOX_PCIE_EPC_BAR0_SIZE &&
           s->bar0_phys <= UINT64_MAX - (QBOX_PCIE_EPC_BAR0_SIZE - 1) &&
           s->local_as_initialized;
}

static void qbox_pcie_epc_start(QBoxPcieEpcState *s)
{
    if (!s->endpoint) {
        qbox_pcie_epc_set_error(s, EPC_ERROR_NOT_LINKED);
        return;
    }
    if (!qbox_pcie_epc_header_valid(s)) {
        qbox_pcie_epc_set_error(s, EPC_ERROR_BAD_HEADER);
        return;
    }
    if (!qbox_pcie_epc_bar_valid(s)) {
        qbox_pcie_epc_set_error(s, EPC_ERROR_BAD_BAR);
        return;
    }
    if (!qbox_pcie_epc_valid_msi_count(s->msi_request_count)) {
        qbox_pcie_epc_set_error(s, EPC_ERROR_BAD_MSI);
        return;
    }

    qbox_pcie_epc_update_header(s);
    qbox_pcie_epc_update_msi_cap(s);
    s->link_started = true;
}

static uint64_t qbox_pcie_epc_reg_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    QBoxPcieEpcState *s = opaque;
    unsigned int slot;
    unsigned int offset;
    QBoxPcieOutboundWindow *ob;

    switch (addr) {
    case EPC_REG_ID:
        return QBOX_PCIE_EPC_ID;
    case EPC_REG_VERSION:
        return QBOX_PCIE_EPC_VERSION;
    case EPC_REG_STATUS:
        return qbox_pcie_epc_status(s);
    case EPC_REG_VENDOR_DEVICE:
        return s->vendor_device;
    case EPC_REG_REV_CLASS:
        return s->rev_class;
    case EPC_REG_SUBSYS_VENDOR_DEVICE:
        return s->subsys_vendor_device;
    case EPC_REG_BAR0_PHYS_LO:
        return s->bar0_phys;
    case EPC_REG_BAR0_PHYS_HI:
        return s->bar0_phys >> 32;
    case EPC_REG_BAR0_SIZE:
        return s->bar0_size;
    case EPC_REG_BAR0_CTRL:
        return s->bar0_ctrl;
    case EPC_REG_MSI_REQUEST_COUNT:
        return s->msi_request_count;
    case EPC_REG_MSI_ENABLED_COUNT:
        return qbox_pcie_epc_msi_enabled_count(s);
    case EPC_REG_LAST_ERROR:
        return s->last_error;
    default:
        break;
    }

    if (addr >= EPC_REG_OB_BASE &&
        addr < EPC_REG_OB_BASE + QBOX_PCIE_EPC_OB_WINDOWS *
                                  EPC_REG_OB_STRIDE) {
        slot = (addr - EPC_REG_OB_BASE) / EPC_REG_OB_STRIDE;
        offset = (addr - EPC_REG_OB_BASE) % EPC_REG_OB_STRIDE;
        ob = &s->ob[slot];
        switch (offset) {
        case 0x00:
            return ob->local_offset;
        case 0x04:
            return ob->pci_addr;
        case 0x08:
            return ob->pci_addr >> 32;
        case 0x0c:
            return ob->size;
        case 0x10:
            return ob->ctrl;
        }
    }

    qbox_pcie_epc_set_error(s, EPC_ERROR_BAD_REGISTER);
    return 0;
}

static bool qbox_pcie_epc_ob_valid(QBoxPcieOutboundWindow *ob)
{
    return ob->size && ob->local_offset < QBOX_PCIE_EPC_OUTBOUND_SIZE &&
           ob->size <= QBOX_PCIE_EPC_OUTBOUND_SIZE - ob->local_offset &&
           ob->pci_addr <= UINT64_MAX - (ob->size - 1);
}

static void qbox_pcie_epc_reg_write(void *opaque, hwaddr addr,
                                    uint64_t value, unsigned size)
{
    QBoxPcieEpcState *s = opaque;
    uint32_t val = value;
    unsigned int slot;
    unsigned int offset;
    QBoxPcieOutboundWindow *ob;

    switch (addr) {
    case EPC_REG_COMMAND:
        if (val & EPC_COMMAND_CLEAR_ERROR) {
            s->last_error = EPC_ERROR_NONE;
        }
        if ((val & (EPC_COMMAND_START | EPC_COMMAND_STOP)) ==
            (EPC_COMMAND_START | EPC_COMMAND_STOP)) {
            qbox_pcie_epc_set_error(s, EPC_ERROR_BAD_REGISTER);
        } else if (val & EPC_COMMAND_STOP) {
            s->link_started = false;
        } else if (val & EPC_COMMAND_START) {
            qbox_pcie_epc_start(s);
        }
        return;
    case EPC_REG_VENDOR_DEVICE:
        if (!s->link_started) {
            s->vendor_device = val;
            qbox_pcie_epc_update_header(s);
        }
        return;
    case EPC_REG_REV_CLASS:
        if (!s->link_started) {
            s->rev_class = val;
            qbox_pcie_epc_update_header(s);
        }
        return;
    case EPC_REG_SUBSYS_VENDOR_DEVICE:
        if (!s->link_started) {
            s->subsys_vendor_device = val;
            qbox_pcie_epc_update_header(s);
        }
        return;
    case EPC_REG_BAR0_PHYS_LO:
        if (!s->link_started) {
            s->bar0_phys = deposit64(s->bar0_phys, 0, 32, val);
        }
        return;
    case EPC_REG_BAR0_PHYS_HI:
        if (!s->link_started) {
            s->bar0_phys = deposit64(s->bar0_phys, 32, 32, val);
        }
        return;
    case EPC_REG_BAR0_SIZE:
        if (!s->link_started) {
            s->bar0_size = val;
        }
        return;
    case EPC_REG_BAR0_CTRL:
        if (!s->link_started) {
            if ((val & EPC_BAR_ENABLE) &&
                s->bar0_size != QBOX_PCIE_EPC_BAR0_SIZE) {
                qbox_pcie_epc_set_error(s, EPC_ERROR_BAD_BAR);
                s->bar0_ctrl = 0;
            } else {
                s->bar0_ctrl = val & EPC_BAR_ENABLE;
            }
        }
        return;
    case EPC_REG_MSI_REQUEST_COUNT:
        if (!s->link_started) {
            if (!qbox_pcie_epc_valid_msi_count(val)) {
                qbox_pcie_epc_set_error(s, EPC_ERROR_BAD_MSI);
            } else {
                s->msi_request_count = val;
                qbox_pcie_epc_update_msi_cap(s);
            }
        }
        return;
    case EPC_REG_MSI_RAISE:
        if (!s->endpoint || !s->link_started || !msi_enabled(
                PCI_DEVICE(s->endpoint)) || val == 0 ||
            val > qbox_pcie_epc_msi_enabled_count(s)) {
            qbox_pcie_epc_set_error(s, EPC_ERROR_BAD_MSI);
        } else {
            msi_notify(PCI_DEVICE(s->endpoint), val - 1);
        }
        return;
    default:
        break;
    }

    if (addr >= EPC_REG_OB_BASE &&
        addr < EPC_REG_OB_BASE + QBOX_PCIE_EPC_OB_WINDOWS *
                                  EPC_REG_OB_STRIDE) {
        slot = (addr - EPC_REG_OB_BASE) / EPC_REG_OB_STRIDE;
        offset = (addr - EPC_REG_OB_BASE) % EPC_REG_OB_STRIDE;
        ob = &s->ob[slot];
        switch (offset) {
        case 0x00:
            ob->local_offset = val;
            return;
        case 0x04:
            ob->pci_addr = deposit64(ob->pci_addr, 0, 32, val);
            return;
        case 0x08:
            ob->pci_addr = deposit64(ob->pci_addr, 32, 32, val);
            return;
        case 0x0c:
            ob->size = val;
            return;
        case 0x10:
            if ((val & EPC_OB_ENABLE) && !qbox_pcie_epc_ob_valid(ob)) {
                qbox_pcie_epc_set_error(s, EPC_ERROR_BAD_OUTBOUND);
                ob->ctrl = 0;
            } else {
                ob->ctrl = val & EPC_OB_ENABLE;
            }
            return;
        }
    }

    qbox_pcie_epc_set_error(s, EPC_ERROR_BAD_REGISTER);
}

static const MemoryRegionOps qbox_pcie_epc_reg_ops = {
    .read = qbox_pcie_epc_reg_read,
    .write = qbox_pcie_epc_reg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static QBoxPcieOutboundWindow *
qbox_pcie_epc_find_ob(QBoxPcieEpcState *s, hwaddr addr, unsigned size)
{
    unsigned int i;

    for (i = 0; i < QBOX_PCIE_EPC_OB_WINDOWS; ++i) {
        QBoxPcieOutboundWindow *ob = &s->ob[i];

        if (!(ob->ctrl & EPC_OB_ENABLE) || addr < ob->local_offset ||
            size > ob->size || addr - ob->local_offset > ob->size - size) {
            continue;
        }
        return ob;
    }

    return NULL;
}

static MemTxResult qbox_pcie_epc_outbound_read(void *opaque, hwaddr addr,
                                                uint64_t *data,
                                                unsigned size,
                                                MemTxAttrs attrs)
{
    QBoxPcieEpcState *s = opaque;
    QBoxPcieOutboundWindow *ob;
    uint8_t buf[8] = { 0 };
    MemTxResult result;

    if (!s->endpoint || !s->link_started) {
        qbox_pcie_epc_set_error(s, EPC_ERROR_NOT_LINKED);
        return MEMTX_ERROR;
    }

    ob = qbox_pcie_epc_find_ob(s, addr, size);
    if (!ob) {
        qbox_pcie_epc_set_error(s, EPC_ERROR_OUTBOUND_ACCESS);
        return MEMTX_ERROR;
    }

    result = pci_dma_read(PCI_DEVICE(s->endpoint),
                          ob->pci_addr + addr - ob->local_offset,
                          buf, size);
    if (result != MEMTX_OK) {
        qbox_pcie_epc_set_error(s, EPC_ERROR_OUTBOUND_ACCESS);
        return result;
    }
    *data = ldn_le_p(buf, size);
    return MEMTX_OK;
}

static MemTxResult qbox_pcie_epc_outbound_write(void *opaque, hwaddr addr,
                                                 uint64_t data,
                                                 unsigned size,
                                                 MemTxAttrs attrs)
{
    QBoxPcieEpcState *s = opaque;
    QBoxPcieOutboundWindow *ob;
    uint8_t buf[8];
    MemTxResult result;

    if (!s->endpoint || !s->link_started) {
        qbox_pcie_epc_set_error(s, EPC_ERROR_NOT_LINKED);
        return MEMTX_ERROR;
    }

    ob = qbox_pcie_epc_find_ob(s, addr, size);
    if (!ob) {
        qbox_pcie_epc_set_error(s, EPC_ERROR_OUTBOUND_ACCESS);
        return MEMTX_ERROR;
    }

    stn_le_p(buf, size, data);
    result = pci_dma_write(PCI_DEVICE(s->endpoint),
                           ob->pci_addr + addr - ob->local_offset,
                           buf, size);
    if (result != MEMTX_OK) {
        qbox_pcie_epc_set_error(s, EPC_ERROR_OUTBOUND_ACCESS);
    }
    return result;
}

static const MemoryRegionOps qbox_pcie_epc_outbound_ops = {
    .read_with_attrs = qbox_pcie_epc_outbound_read,
    .write_with_attrs = qbox_pcie_epc_outbound_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
};

static MemTxResult qbox_pcie_ep_bar_read(void *opaque, hwaddr addr,
                                         uint64_t *data, unsigned size,
                                         MemTxAttrs attrs)
{
    QBoxPcieTestEpState *ep = opaque;
    QBoxPcieEpcState *s = ep->epc;
    uint8_t buf[8] = { 0 };
    MemTxResult result;

    if (!s || !s->link_started || !(s->bar0_ctrl & EPC_BAR_ENABLE) ||
        addr > QBOX_PCIE_EPC_BAR0_SIZE - size) {
        return MEMTX_ERROR;
    }

    result = address_space_read(&s->local_as, s->bar0_phys + addr,
                                attrs, buf, size);
    *data = ldn_le_p(buf, size);
    return result;
}

static MemTxResult qbox_pcie_ep_bar_write(void *opaque, hwaddr addr,
                                          uint64_t data, unsigned size,
                                          MemTxAttrs attrs)
{
    QBoxPcieTestEpState *ep = opaque;
    QBoxPcieEpcState *s = ep->epc;
    uint8_t buf[8];

    if (!s || !s->link_started || !(s->bar0_ctrl & EPC_BAR_ENABLE) ||
        addr > QBOX_PCIE_EPC_BAR0_SIZE - size) {
        return MEMTX_ERROR;
    }

    stn_le_p(buf, size, data);
    return address_space_write(&s->local_as, s->bar0_phys + addr,
                               attrs, buf, size);
}

static const MemoryRegionOps qbox_pcie_ep_bar_ops = {
    .read_with_attrs = qbox_pcie_ep_bar_read,
    .write_with_attrs = qbox_pcie_ep_bar_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
};

static uint32_t qbox_pcie_ep_config_read(PCIDevice *pdev,
                                         uint32_t address, int len)
{
    QBoxPcieTestEpState *ep = QBOX_PCIE_TEST_EP(pdev);

    if (!ep->epc || !ep->epc->link_started) {
        return UINT32_MAX;
    }
    return pci_default_read_config(pdev, address, len);
}

static void qbox_pcie_ep_config_write(PCIDevice *pdev, uint32_t address,
                                      uint32_t value, int len)
{
    QBoxPcieTestEpState *ep = QBOX_PCIE_TEST_EP(pdev);

    if (ep->epc && ep->epc->link_started) {
        pci_default_write_config(pdev, address, value, len);
    }
}

static void qbox_pcie_ep_reset(DeviceState *dev)
{
    QBoxPcieTestEpState *ep = QBOX_PCIE_TEST_EP(dev);

    msi_reset(PCI_DEVICE(ep));
    if (ep->epc) {
        ep->epc->link_started = false;
    }
}

static void qbox_pcie_ep_realize(PCIDevice *pdev, Error **errp)
{
    QBoxPcieTestEpState *ep = QBOX_PCIE_TEST_EP(pdev);
    int ret;

    if (!ep->epc) {
        error_setg(errp, "qbox-pcie-test-ep requires an EPC link");
        return;
    }
    if (ep->epc->endpoint) {
        error_setg(errp, "qbox-pcie-epc supports one endpoint function");
        return;
    }

    memory_region_init_io(&ep->bar0, OBJECT(ep), &qbox_pcie_ep_bar_ops,
                          ep, "qbox-pcie-ep-bar0",
                          QBOX_PCIE_EPC_BAR0_SIZE);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY |
                              PCI_BASE_ADDRESS_MEM_TYPE_64,
                     &ep->bar0);

    ret = pcie_endpoint_cap_init(pdev, 0);
    if (ret < 0) {
        error_setg_errno(errp, -ret, "failed to initialize PCIe capability");
        return;
    }
    pcie_cap_fill_link_ep_usp(pdev, QEMU_PCI_EXP_LNK_X4,
                              QEMU_PCI_EXP_LNK_32GT, false);

    ret = msi_init(pdev, 0, QBOX_PCIE_EPC_MSI_MAX, true, false, errp);
    if (ret) {
        pcie_cap_exit(pdev);
        return;
    }

    ep->epc->endpoint = ep;
    qbox_pcie_epc_update_header(ep->epc);
    qbox_pcie_epc_update_msi_cap(ep->epc);
}

static void qbox_pcie_ep_exit(PCIDevice *pdev)
{
    QBoxPcieTestEpState *ep = QBOX_PCIE_TEST_EP(pdev);

    if (ep->epc && ep->epc->endpoint == ep) {
        ep->epc->endpoint = NULL;
        ep->epc->link_started = false;
    }
    msi_uninit(pdev);
    pcie_cap_exit(pdev);
}

static void qbox_pcie_epc_reset(DeviceState *dev)
{
    QBoxPcieEpcState *s = QBOX_PCIE_EPC(dev);

    s->link_started = false;
    s->vendor_device = 0xffffffff;
    s->rev_class = 0;
    s->subsys_vendor_device = 0;
    s->bar0_phys = 0;
    s->bar0_size = QBOX_PCIE_EPC_BAR0_SIZE;
    s->bar0_ctrl = 0;
    s->msi_request_count = 1;
    memset(s->ob, 0, sizeof(s->ob));
    s->last_error = EPC_ERROR_NONE;
    qbox_pcie_epc_update_header(s);
    qbox_pcie_epc_update_msi_cap(s);
}

static void qbox_pcie_epc_realize(DeviceState *dev, Error **errp)
{
    QBoxPcieEpcState *s = QBOX_PCIE_EPC(dev);

    if (!s->local_memory) {
        error_setg(errp, "qbox-pcie-epc requires local-memory");
        return;
    }

    address_space_init(&s->local_as, s->local_memory,
                       "qbox-pcie-epc-local");
    s->local_as_initialized = true;
    qbox_pcie_epc_reset(dev);
}

static void qbox_pcie_epc_unrealize(DeviceState *dev)
{
    QBoxPcieEpcState *s = QBOX_PCIE_EPC(dev);

    if (s->endpoint) {
        s->endpoint->epc = NULL;
        s->endpoint = NULL;
    }
    if (s->local_as_initialized) {
        address_space_destroy(&s->local_as);
        s->local_as_initialized = false;
    }
}

static const Property qbox_pcie_epc_properties[] = {
    DEFINE_PROP_LINK("local-memory", QBoxPcieEpcState, local_memory,
                     TYPE_MEMORY_REGION, MemoryRegion *),
};

static const Property qbox_pcie_ep_properties[] = {
    DEFINE_PROP_LINK("epc", QBoxPcieTestEpState, epc,
                     TYPE_QBOX_PCIE_EPC, QBoxPcieEpcState *),
};

static void qbox_pcie_epc_init(Object *obj)
{
    QBoxPcieEpcState *s = QBOX_PCIE_EPC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->regs, obj, &qbox_pcie_epc_reg_ops, s,
                          "qbox-pcie-epc-regs", QBOX_PCIE_EPC_REG_SIZE);
    memory_region_init_io(&s->outbound, obj, &qbox_pcie_epc_outbound_ops,
                          s, "qbox-pcie-epc-outbound",
                          QBOX_PCIE_EPC_OUTBOUND_SIZE);
    sysbus_init_mmio(sbd, &s->regs);
    sysbus_init_mmio(sbd, &s->outbound);
}

static void qbox_pcie_epc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "QBox virtual PCIe endpoint controller";
    dc->realize = qbox_pcie_epc_realize;
    dc->unrealize = qbox_pcie_epc_unrealize;
    device_class_set_legacy_reset(dc, qbox_pcie_epc_reset);
    device_class_set_props(dc, qbox_pcie_epc_properties);
}

static void qbox_pcie_ep_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);

    dc->desc = "QBox virtual PCIe test endpoint";
    device_class_set_legacy_reset(dc, qbox_pcie_ep_reset);
    device_class_set_props(dc, qbox_pcie_ep_properties);
    pc->realize = qbox_pcie_ep_realize;
    pc->exit = qbox_pcie_ep_exit;
    pc->config_read = qbox_pcie_ep_config_read;
    pc->config_write = qbox_pcie_ep_config_write;
    pc->vendor_id = PCI_VENDOR_ID_REDHAT_QUMRANET;
    pc->device_id = 0x1110;
    pc->revision = 0;
    pc->class_id = PCI_CLASS_OTHERS;
}

static const TypeInfo qbox_pcie_types[] = {
    {
        .name = TYPE_QBOX_PCIE_EPC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(QBoxPcieEpcState),
        .instance_init = qbox_pcie_epc_init,
        .class_init = qbox_pcie_epc_class_init,
    },
    {
        .name = TYPE_QBOX_PCIE_TEST_EP,
        .parent = TYPE_PCI_DEVICE,
        .instance_size = sizeof(QBoxPcieTestEpState),
        .class_init = qbox_pcie_ep_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { INTERFACE_PCIE_DEVICE },
            { },
        },
    },
};

DEFINE_TYPES(qbox_pcie_types)
