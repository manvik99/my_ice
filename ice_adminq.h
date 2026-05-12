#ifndef ICE_ADMINQ_H
#define ICE_ADMINQ_H

#include <stdbool.h>
#include <stdint.h>

#include "ice_types.h"

void aq_set_topology_options(bool dump_topo, bool qparent_override_set,
                             uint32_t qparent_override);
void fill_dflt_direct_desc(struct ice_aq_desc *desc, uint16_t opcode);
void adminq_hw_init(struct ice_vfio_dev *d);
int aq_send_cmd(struct ice_vfio_dev *d, struct ice_aq_desc *desc, void *buf,
                uint16_t buf_size);
int aq_get_fw_ver(struct ice_vfio_dev *d);
int aq_manage_mac_read(struct ice_vfio_dev *d);
int aq_get_default_vsi_and_lport(struct ice_vfio_dev *d);
int aq_get_qparent_teid(struct ice_vfio_dev *d);
int aq_add_rx_mac_rule(struct ice_vfio_dev *d, uint16_t *rule_idx);
void aq_remove_sw_rule_best_effort(struct ice_vfio_dev *d, uint16_t rule_type,
                                   uint16_t rule_idx);

#endif
