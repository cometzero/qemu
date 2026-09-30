/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef HW_DMA_ARM_DMA350_H
#define HW_DMA_ARM_DMA350_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_ARM_DMA350 "arm-dma350"
OBJECT_DECLARE_SIMPLE_TYPE(ARMDMA350State, ARM_DMA350)

/* dma-req/dma-ack GPIOs carry ACTIVE in bit 2 and the type in bits 1:0. */
#endif
