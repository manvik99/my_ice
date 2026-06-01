#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ice_adminq.h"
#include "ice_dma.h"
#include "ice_lanq.h"
#include "ice_utils.h"
#include "ice_vfio.h"

int main(int argc, char **argv)
{
    struct ice_vfio_dev d;
    const char *bdf;
    bool run_rx_listen_mode = false;
    bool run_rx_reflect_mode = false;
    bool run_tx_send_mode = false;
    bool run_tx_bench_mode = false;
    bool use_hugepages = false;
    const char *huge_dir = "/mnt/huge";
    uint16_t txq_count = 1;
    uint16_t tx_desc_count = ICE_TX_DESC_COUNT;
    uint16_t reflect_batch = DEFAULT_REFLECT_BATCH;
    bool pin_cpus = false;
    bool dump_topo = false;
    const char *metrics_log = NULL;
    uint32_t qparent_override = 0;
    bool qparent_override_set = false;
    int rx_listen_timeout_s = 30;
    int rx_reflect_timeout_s = 30;
    uint8_t tx_send_dst_mac[ETHER_ADDR_LEN] = {0};
    int tx_send_count = 1;
    int tx_send_interval_ms = 100;
    const char *tx_send_payload = "my_ice-userspace-tx";
    uint8_t tx_bench_dst_mac[ETHER_ADDR_LEN] = {0};
    int tx_bench_seconds = 0;
    int tx_bench_payload_len = 0;
    int rc = EXIT_FAILURE;

    ice_vfio_dev_init(&d);

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <BDF> [--rx-listen [seconds]|--rx-reflect [seconds]|--tx-send <dst-mac> [count] [interval-ms] [payload]|--tx-bench <seconds> <dst-mac> <payload-len>] [--tx-queues <n>] [--tx-desc-count <n>] [--reflect-batch <n>] [--metrics-log <path>] [--pin-cpus] [--dump-topo] [--qparent-teid <hex>] [--hugepages [--hugepage-dir <dir>]]\n", argv[0]);
        fprintf(stderr, "Example: %s 0000:17:00.0 --rx-listen\n", argv[0]);
        fprintf(stderr, "Example: %s 0000:17:00.0 --rx-listen 60\n", argv[0]);
        fprintf(stderr, "Example: %s 0000:17:00.0 --rx-reflect 60\n", argv[0]);
        fprintf(stderr, "Example: %s 0000:17:00.0 --rx-reflect 60 --reflect-batch 32\n", argv[0]);
        fprintf(stderr, "Example: %s 0000:17:00.0 --rx-reflect 60 --metrics-log /tmp/my_ice_metrics.log\n", argv[0]);
        fprintf(stderr, "Example: %s 0000:17:00.0 --tx-send aa:bb:cc:dd:ee:ff\n", argv[0]);
        fprintf(stderr, "Example: %s 0000:17:00.0 --tx-send aa:bb:cc:dd:ee:ff 20 100 hello\n", argv[0]);
        fprintf(stderr, "Example: %s 0000:17:00.0 --tx-bench 10 aa:bb:cc:dd:ee:ff 46\n", argv[0]);
        fprintf(stderr, "Example: %s 0000:17:00.0 --tx-bench 15 aa:bb:cc:dd:ee:ff 1472 --hugepages --hugepage-dir /mnt/huge\n", argv[0]);
        return EXIT_FAILURE;
    }

    bdf = argv[1];
    if (argc >= 3) {
        int i = 2;
        bool mode_set = false;

        while (i < argc) {
            if (strcmp(argv[i], "--rx-listen") == 0) {
                if (mode_set) {
                    fprintf(stderr, "only one mode allowed (--rx-listen/--rx-reflect/--tx-send/--tx-bench)\n");
                    return EXIT_FAILURE;
                }
                mode_set = true;
                run_rx_listen_mode = true;
                i++;
                if (i < argc && strncmp(argv[i], "--", 2) != 0) {
                    if (parse_int_range(argv[i], 1, 3600, &rx_listen_timeout_s) < 0) {
                        fprintf(stderr, "invalid --rx-listen timeout '%s' (expected 1..3600 seconds)\n",
                                argv[i]);
                        return EXIT_FAILURE;
                    }
                    i++;
                }
            } else if (strcmp(argv[i], "--rx-reflect") == 0) {
                if (mode_set) {
                    fprintf(stderr, "only one mode allowed (--rx-listen/--rx-reflect/--tx-send/--tx-bench)\n");
                    return EXIT_FAILURE;
                }
                /* rx-reflect runs the receive -> rewrite -> transmit loop for the requested duration. */
                mode_set = true;
                run_rx_reflect_mode = true;
                i++;
                if (i < argc && strncmp(argv[i], "--", 2) != 0) {
                    if (parse_int_range(argv[i], 1, 3600, &rx_reflect_timeout_s) < 0) {
                        fprintf(stderr, "invalid --rx-reflect timeout '%s' (expected 1..3600 seconds)\n",
                                argv[i]);
                        return EXIT_FAILURE;
                    }
                    i++;
                }
            } else if (strcmp(argv[i], "--tx-send") == 0) {
                if (mode_set) {
                    fprintf(stderr, "only one mode allowed (--rx-listen/--rx-reflect/--tx-send/--tx-bench)\n");
                    return EXIT_FAILURE;
                }
                mode_set = true;
                run_tx_send_mode = true;
                i++;
                if (i >= argc) {
                    fprintf(stderr, "--tx-send requires <dst-mac>\n");
                    return EXIT_FAILURE;
                }
                if (parse_mac_addr(argv[i], tx_send_dst_mac) < 0) {
                    fprintf(stderr, "invalid dst MAC '%s' (expected aa:bb:cc:dd:ee:ff)\n",
                            argv[i]);
                    return EXIT_FAILURE;
                }
                i++;
                if (i < argc && strncmp(argv[i], "--", 2) != 0) {
                    if (parse_int_range(argv[i], 1, 1000000, &tx_send_count) < 0) {
                        fprintf(stderr, "invalid --tx-send count '%s' (expected 1..1000000)\n",
                                argv[i]);
                        return EXIT_FAILURE;
                    }
                    i++;
                }
                if (i < argc && strncmp(argv[i], "--", 2) != 0) {
                    if (parse_int_range(argv[i], 0, 60000, &tx_send_interval_ms) < 0) {
                        fprintf(stderr, "invalid --tx-send interval '%s' ms (expected 0..60000)\n",
                                argv[i]);
                        return EXIT_FAILURE;
                    }
                    i++;
                }
                if (i < argc && strncmp(argv[i], "--", 2) != 0) {
                    tx_send_payload = argv[i];
                    i++;
                }
            } else if (strcmp(argv[i], "--tx-bench") == 0) {
                if (mode_set) {
                    fprintf(stderr, "only one mode allowed (--rx-listen/--rx-reflect/--tx-send/--tx-bench)\n");
                    return EXIT_FAILURE;
                }
                mode_set = true;
                run_tx_bench_mode = true;
                if (i + 3 >= argc) {
                    fprintf(stderr, "--tx-bench requires <seconds> <dst-mac> <payload-len>\n");
                    return EXIT_FAILURE;
                }
                if (parse_int_range(argv[i + 1], 1, 3600, &tx_bench_seconds) < 0) {
                    fprintf(stderr, "invalid --tx-bench seconds '%s' (expected 1..3600)\n",
                            argv[i + 1]);
                    return EXIT_FAILURE;
                }
                if (parse_mac_addr(argv[i + 2], tx_bench_dst_mac) < 0) {
                    fprintf(stderr, "invalid dst MAC '%s' (expected aa:bb:cc:dd:ee:ff)\n",
                            argv[i + 2]);
                    return EXIT_FAILURE;
                }
                if (parse_int_range(argv[i + 3], 0, ICE_TX_PKT_BUF_SIZE - 14,
                                    &tx_bench_payload_len) < 0) {
                    fprintf(stderr, "invalid --tx-bench payload-len '%s' (expected 0..%d)\n",
                            argv[i + 3], ICE_TX_PKT_BUF_SIZE - 14);
                    return EXIT_FAILURE;
                }
                i += 4;
            } else if (strcmp(argv[i], "--hugepages") == 0) {
                use_hugepages = true;
                i++;
            } else if (strcmp(argv[i], "--hugepage-dir") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "--hugepage-dir requires <dir>\n");
                    return EXIT_FAILURE;
                }
                huge_dir = argv[i + 1];
                i += 2;
            } else if (strcmp(argv[i], "--tx-queues") == 0) {
                int v;
                if (i + 1 >= argc) {
                    fprintf(stderr, "--tx-queues requires <n>\n");
                    return EXIT_FAILURE;
                }
                if (parse_int_range(argv[i + 1], 1, 1024, &v) < 0) {
                    fprintf(stderr, "invalid --tx-queues '%s' (expected 1..1024)\n",
                            argv[i + 1]);
                    return EXIT_FAILURE;
                }
                txq_count = (uint16_t)v;
                i += 2;
            } else if (strcmp(argv[i], "--tx-desc-count") == 0) {
                int v;
                if (i + 1 >= argc) {
                    fprintf(stderr, "--tx-desc-count requires <n>\n");
                    return EXIT_FAILURE;
                }
                if (parse_int_range(argv[i + 1], 32, 8192, &v) < 0) {
                    fprintf(stderr, "invalid --tx-desc-count '%s' (expected 32..8192)\n",
                            argv[i + 1]);
                    return EXIT_FAILURE;
                }
                tx_desc_count = (uint16_t)v;
                i += 2;
            } else if (strcmp(argv[i], "--reflect-batch") == 0) {
                int v;
                if (i + 1 >= argc) {
                    fprintf(stderr, "--reflect-batch requires <n>\n");
                    return EXIT_FAILURE;
                }
                /* reflect_batch caps one hot-loop burst: how many packets we poll, swap, rearm, and enqueue together. */
                if (parse_int_range(argv[i + 1], 1, MAX_REFLECT_BATCH, &v) < 0) {
                    fprintf(stderr, "invalid --reflect-batch '%s' (expected 1..%u)\n",
                            argv[i + 1], MAX_REFLECT_BATCH);
                    return EXIT_FAILURE;
                }
                reflect_batch = (uint16_t)v;
                i += 2;
            } else if (strcmp(argv[i], "--metrics-log") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "--metrics-log requires <path>\n");
                    return EXIT_FAILURE;
                }
                metrics_log = argv[i + 1];
                i += 2;
            } else if (strcmp(argv[i], "--pin-cpus") == 0) {
                pin_cpus = true;
                i++;
            } else if (strcmp(argv[i], "--dump-topo") == 0) {
                dump_topo = true;
                i++;
            } else if (strcmp(argv[i], "--qparent-teid") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "--qparent-teid requires <hex>\n");
                    return EXIT_FAILURE;
                }
                if (parse_u32_hex(argv[i + 1], &qparent_override) < 0) {
                    fprintf(stderr, "invalid --qparent-teid '%s'\n", argv[i + 1]);
                    return EXIT_FAILURE;
                }
                qparent_override_set = true;
                i += 2;
            } else {
                fprintf(stderr, "unknown option: %s\n", argv[i]);
                return EXIT_FAILURE;
            }
        }

        if (!mode_set) {
            fprintf(stderr, "one mode is required (--rx-listen/--rx-reflect/--tx-send/--tx-bench)\n");
            return EXIT_FAILURE;
        }
    }

    d.txq_count = txq_count;
    d.txq_alloc_count = txq_count;
    d.tx_desc_count = tx_desc_count;
    if (metrics_log && copy_path_option(d.metrics_log_path, metrics_log, "--metrics-log") < 0)
        return EXIT_FAILURE;
    d.txqs = calloc(d.txq_count, sizeof(*d.txqs));
    if (!d.txqs)
        die_errno("calloc txqs");
    alloc_queue_sw_state(&d);
    aq_set_topology_options(dump_topo, qparent_override_set, qparent_override);

    fprintf(stderr, "[my_ice] opening VFIO device %s\n", bdf);
    if (ice_vfio_open(&d, bdf) < 0)
        goto out;
    fprintf(stderr, "[my_ice] vfio init done, bar0_size=0x%zx\n", d.bar0_size);

    if (use_hugepages) {
        fprintf(stderr, "[my_ice] using hugepages from %s\n", huge_dir);
        prepare_hugepage_file(&d, ice_dma_required_bytes(&d), huge_dir);
    }

    if (ice_vfio_init(&d) < 0)
        goto out;

    if (run_rx_listen_mode) {
        fprintf(stderr, "[my_ice] running rx listen path\n");
        if (run_rx_listen(&d, rx_listen_timeout_s * 1000) < 0)
            goto out;
    } else if (run_rx_reflect_mode) {
        fprintf(stderr, "[my_ice] running rx reflect path\n");
        /* The reflect mode owns one Rx queue and one Tx queue, rewrites Ethernet headers in place, and recycles buffers through reflect_pool. */
        if (run_rx_reflect(&d, rx_reflect_timeout_s * 1000, reflect_batch) < 0)
            goto out;
    } else if (run_tx_send_mode) {
        fprintf(stderr, "[my_ice] running tx send path\n");
        if (run_tx_send(&d, tx_send_dst_mac, tx_send_count,
                        tx_send_interval_ms, tx_send_payload) < 0)
            goto out;
    } else if (run_tx_bench_mode) {
        fprintf(stderr, "[my_ice] running tx bench path\n");
        if (run_tx_bench(&d, tx_bench_dst_mac, tx_bench_seconds,
                         tx_bench_payload_len, pin_cpus) < 0)
            goto out;
    }

    rc = EXIT_SUCCESS;

out:
    ice_vfio_close(&d);
    return rc;
}
