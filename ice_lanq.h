#ifndef ICE_LANQ_H
#define ICE_LANQ_H

#include <stdbool.h>
#include <stdint.h>

#include "ice_types.h"

int run_rx_listen(struct ice_vfio_dev *d, int timeout_ms);
int run_rx_reflect(struct ice_vfio_dev *d, int timeout_ms, uint16_t reflect_batch);
int run_tx_send(struct ice_vfio_dev *d, const uint8_t *dst_mac, int count,
                int interval_ms, const char *payload);
int run_tx_bench(struct ice_vfio_dev *d, const uint8_t *dst_mac, int seconds,
                 int payload_len, bool pin_cpus);

#endif
