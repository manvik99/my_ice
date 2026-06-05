#ifndef ICE_UTILS_H
#define ICE_UTILS_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#include "ice_types.h"

void die_errno(const char *msg);
void die_msg(const char *msg);
int parse_int_range(const char *s, int min_v, int max_v, int *out);
int parse_mac_addr(const char *s, uint8_t *out);
int parse_u32_hex(const char *s, uint32_t *out);
uint64_t monotonic_ns(void);
double bytes_ns_to_gbps(uint64_t bytes, uint64_t duration_ns);
double pkts_ns_to_mpps(uint64_t pkts, uint64_t duration_ns);
int copy_path_option(char dst[PATH_MAX], const char *src, const char *opt_name);
int build_cpu_list(int *out, int max_out);
void pin_thread_to_cpu(int cpu);
void dump_hex(const uint8_t *buf, size_t len, size_t max_len);
size_t get_hugepage_size(void);
size_t prepare_hugepage_file(struct ice_vfio_dev *d, size_t size, const char *dir);

#endif
