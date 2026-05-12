#ifndef ICE_REGS_H
#define ICE_REGS_H

#include <stdint.h>

#include "ice_types.h"

#ifndef VFIO_PCI_BAR0_REGION_INDEX
#define VFIO_PCI_BAR0_REGION_INDEX 0
#endif

#ifndef VFIO_PCI_CONFIG_REGION_INDEX
#define VFIO_PCI_CONFIG_REGION_INDEX 7
#endif

#define PCI_COMMAND_OFF 0x04
#define PCI_COMMAND_MEM 0x2
#define PCI_COMMAND_MASTER 0x4

static inline uint32_t reg_read32(struct ice_vfio_dev *d, uint32_t off)
{
    volatile uint32_t *p = (volatile uint32_t *)(d->bar0 + off);
    return *p;
}

static inline void reg_write32(struct ice_vfio_dev *d, uint32_t off, uint32_t val)
{
    volatile uint32_t *p = (volatile uint32_t *)(d->bar0 + off);
    *p = val;
}

#endif
