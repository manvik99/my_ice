#ifndef ICE_PCI_H
#define ICE_PCI_H

#include "ice_types.h"

void vfio_init(struct ice_vfio_dev *d, const char *bdf);

#endif
