#include <assert.h>

#include "ice_types.h"

int main(void)
{
    assert(ice_reflect_pkt_len_fits(0));
    assert(ice_reflect_pkt_len_fits(ICE_PKT_BUF_DATA_SIZE - 1));
    assert(ice_reflect_pkt_len_fits(ICE_PKT_BUF_DATA_SIZE));
    assert(!ice_reflect_pkt_len_fits(ICE_PKT_BUF_DATA_SIZE + 1));
    return 0;
}
