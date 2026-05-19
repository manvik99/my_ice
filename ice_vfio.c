#define _GNU_SOURCE
#include <linux/vfio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ice_adminq.h"
#include "ice_dma.h"
#include "ice_pci.h"
#include "ice_utils.h"
#include "ice_vfio.h"

void ice_vfio_dev_init(struct ice_vfio_dev *dev)
{
    memset(dev, 0, sizeof(*dev));
    dev->container_fd = -1;
    dev->group_fd = -1;
    dev->device_fd = -1;
    dev->huge_fd = -1;
}

int ice_vfio_open(struct ice_vfio_dev *dev, const char *pci_bdf)
{
    vfio_init(dev, pci_bdf);
    return 0;
}

int ice_vfio_init(struct ice_vfio_dev *dev)
{
    size_t dma_map_bytes = ice_dma_required_bytes(dev);

    if (dev->huge_fd >= 0 && dev->huge_alloc_size != 0)
        dma_map_bytes = dev->huge_alloc_size;

    dma_map(dev, dma_map_bytes);
    layout_dma(dev);
    pkt_pool_init(dev);
    adminq_hw_init(dev);

    fprintf(stderr, "[my_ice] adminq initialized, sending GET_VER\n");
    if (aq_get_fw_ver(dev) < 0)
        return -1;

    fprintf(stderr, "[my_ice] GET_VER ok, sending MANAGE_MAC_READ\n");
    if (aq_manage_mac_read(dev) < 0)
        return -1;

    return 0;
}

void ice_vfio_close(struct ice_vfio_dev *dev)
{
    uint16_t i;

    if (dev->bar0 && dev->bar0 != MAP_FAILED)
        munmap(dev->bar0, dev->bar0_size);

    if (dev->dma.vaddr && dev->dma.vaddr != MAP_FAILED) {
        struct vfio_iommu_type1_dma_unmap unmap = {0};
        unmap.argsz = sizeof(unmap);
        unmap.iova = dev->dma.iova;
        unmap.size = dev->dma.size;
        if (dev->container_fd >= 0)
            (void)ioctl(dev->container_fd, VFIO_IOMMU_UNMAP_DMA, &unmap);
        munmap(dev->dma.vaddr, dev->dma.size);
    }

    if (dev->device_fd >= 0)
        close(dev->device_fd);
    if (dev->group_fd >= 0)
        close(dev->group_fd);
    if (dev->container_fd >= 0)
        close(dev->container_fd);
    if (dev->huge_fd >= 0)
        close(dev->huge_fd);
    if (dev->huge_path[0] != '\0')
        unlink(dev->huge_path);
    free(dev->io.rx_pkt_bufs);
    free(dev->reflect_pool.free_ring);
    for (i = 0; i < dev->txq_alloc_count; i++) {
        free(dev->txqs[i].tx_pkt_buf_refs);
        free(dev->txqs[i].tx_rsq);
    }
    free(dev->txqs);
}
