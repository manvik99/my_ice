#include <stdint.h>
#include <string.h>

#include "ice_controlq.h"

void set_ctx_bits(uint8_t *src_ctx, uint8_t *dest_ctx,
                  const struct ice_ctx_ele *ce_info)
{
    int f;

    for (f = 0; ce_info[f].width; f++) {
        uint64_t v = 0;
        uint16_t bit;

        if (ce_info[f].width > ce_info[f].size_of * 8)
            continue;

        memcpy(&v, src_ctx + ce_info[f].offset, ce_info[f].size_of);

        for (bit = 0; bit < ce_info[f].width; bit++) {
            uint32_t dst_bit = ce_info[f].lsb + bit;
            uint8_t *dst_byte = &dest_ctx[dst_bit / 8];
            uint8_t dst_mask = (uint8_t)(1U << (dst_bit % 8));
            if (v & (1ULL << bit))
                *dst_byte |= dst_mask;
            else
                *dst_byte &= (uint8_t)~dst_mask;
        }
    }
}
