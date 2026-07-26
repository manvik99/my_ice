#ifndef ICE_LANQ_H
#define ICE_LANQ_H

#include <stdint.h>

#include "ice_analysis.h"
#include "ice_types.h"

int run_rx_reflect(struct ice_vfio_dev *d, int timeout_ms, uint16_t reflect_batch,
                   const struct ice_analysis_config *analysis);

#endif
