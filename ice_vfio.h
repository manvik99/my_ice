#ifndef ICE_VFIO_H
#define ICE_VFIO_H

#include "ice_types.h"

void ice_vfio_dev_init(struct ice_vfio_dev *dev);
int ice_vfio_open(struct ice_vfio_dev *dev, const char *pci_bdf);
int ice_vfio_init(struct ice_vfio_dev *dev);
void ice_vfio_close(struct ice_vfio_dev *dev);

#endif
