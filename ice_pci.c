#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/vfio.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ice_pci.h"
#include "ice_regs.h"
#include "ice_utils.h"

static int get_iommu_group_id(const char *bdf)
{
    char path[256];
    char target[256];
    ssize_t n;
    char *slash;

    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/iommu_group", bdf);
    n = readlink(path, target, sizeof(target) - 1);
    if (n < 0)
        die_errno("readlink iommu_group");
    target[n] = '\0';

    slash = strrchr(target, '/');
    if (!slash || !*(slash + 1))
        die_msg("failed to parse iommu group");

    return atoi(slash + 1);
}

void vfio_init(struct ice_vfio_dev *d, const char *bdf)
{
    struct vfio_group_status gstatus = { .argsz = sizeof(gstatus) };
    struct vfio_region_info bar0_rinfo = { .argsz = sizeof(bar0_rinfo), .index = VFIO_PCI_BAR0_REGION_INDEX };
    struct vfio_region_info cfg_rinfo = { .argsz = sizeof(cfg_rinfo), .index = VFIO_PCI_CONFIG_REGION_INDEX };
    char group_path[128];
    uint16_t pci_cmd;

    d->container_fd = open("/dev/vfio/vfio", O_RDWR);
    if (d->container_fd < 0)
        die_errno("open /dev/vfio/vfio");

    if (ioctl(d->container_fd, VFIO_GET_API_VERSION) != VFIO_API_VERSION)
        die_msg("VFIO API version mismatch");

    if (!ioctl(d->container_fd, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU))
        die_msg("VFIO_TYPE1_IOMMU is not supported");

    d->group_id = get_iommu_group_id(bdf);
    snprintf(group_path, sizeof(group_path), "/dev/vfio/%d", d->group_id);
    d->group_fd = open(group_path, O_RDWR);
    if (d->group_fd < 0)
        die_errno("open vfio group");

    if (ioctl(d->group_fd, VFIO_GROUP_GET_STATUS, &gstatus) < 0)
        die_errno("VFIO_GROUP_GET_STATUS");
    if (!(gstatus.flags & VFIO_GROUP_FLAGS_VIABLE))
        die_msg("vfio group is not viable");

    if (ioctl(d->group_fd, VFIO_GROUP_SET_CONTAINER, &d->container_fd) < 0)
        die_errno("VFIO_GROUP_SET_CONTAINER");

    if (ioctl(d->container_fd, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU) < 0)
        die_errno("VFIO_SET_IOMMU");

    d->device_fd = ioctl(d->group_fd, VFIO_GROUP_GET_DEVICE_FD, bdf);
    if (d->device_fd < 0)
        die_errno("VFIO_GROUP_GET_DEVICE_FD");

    if (ioctl(d->device_fd, VFIO_DEVICE_GET_REGION_INFO, &cfg_rinfo) < 0)
        die_errno("VFIO_DEVICE_GET_REGION_INFO CFG");

    if (pread(d->device_fd, &pci_cmd, sizeof(pci_cmd), cfg_rinfo.offset + PCI_COMMAND_OFF) != sizeof(pci_cmd))
        die_errno("pread PCI_COMMAND");
    pci_cmd |= (PCI_COMMAND_MEM | PCI_COMMAND_MASTER);
    if (pwrite(d->device_fd, &pci_cmd, sizeof(pci_cmd), cfg_rinfo.offset + PCI_COMMAND_OFF) != sizeof(pci_cmd))
        die_errno("pwrite PCI_COMMAND");

    if (ioctl(d->device_fd, VFIO_DEVICE_GET_REGION_INFO, &bar0_rinfo) < 0)
        die_errno("VFIO_DEVICE_GET_REGION_INFO BAR0");

    d->bar0_size = bar0_rinfo.size;
    d->bar0 = mmap(NULL, d->bar0_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                   d->device_fd, bar0_rinfo.offset);
    if (d->bar0 == MAP_FAILED)
        die_errno("mmap BAR0");
}
