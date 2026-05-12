#ifndef ICE_CONTROLQ_H
#define ICE_CONTROLQ_H

#include <stdint.h>

#include "ice_types.h"

void set_ctx_bits(uint8_t *src_ctx, uint8_t *dest_ctx,
                  const struct ice_ctx_ele *ce_info);

#endif
