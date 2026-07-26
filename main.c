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

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s <BDF> --rx-reflect [seconds] [--tx-desc-count <n>] [--reflect-batch <n>] [--pin-cpus] [--dump-topo] [--qparent-teid <hex>] [--hugepages [--hugepage-dir <dir>]] [--analysis-warmup-batches <n> --analysis-samples <n>]\n",
            prog);
    fprintf(stderr, "Example: %s 0000:17:00.0 --rx-reflect 60\n", prog);
}

int main(int argc, char **argv)
{
    struct ice_vfio_dev d;
    const char *bdf;
    const char *huge_dir = "/mnt/huge";
    bool run_rx_reflect_mode = false;
    bool use_hugepages = false;
    bool pin_cpus = false;
    bool dump_topo = false;
    bool qparent_override_set = false;
    uint16_t tx_desc_count = ICE_TX_DESC_COUNT;
    uint16_t reflect_batch = DEFAULT_REFLECT_BATCH;
    uint32_t qparent_override = 0;
    int rx_reflect_timeout_s = 30;
    struct ice_analysis_config analysis = {0};
    bool analysis_option_seen = false;
    bool analysis_await_completion = false;
    int rc = EXIT_FAILURE;
    int i;

    ice_vfio_dev_init(&d);

    if (argc < 3) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    bdf = argv[1];
    i = 2;
    while (i < argc) {
        if (strcmp(argv[i], "--rx-reflect") == 0) {
            if (run_rx_reflect_mode) {
                fprintf(stderr, "--rx-reflect may only be specified once\n");
                return EXIT_FAILURE;
            }
            run_rx_reflect_mode = true;
            i++;
            if (i < argc && strncmp(argv[i], "--", 2) != 0) {
                if (parse_int_range(argv[i], 1, 3600, &rx_reflect_timeout_s) < 0) {
                    fprintf(stderr,
                            "invalid --rx-reflect timeout '%s' (expected 1..3600 seconds)\n",
                            argv[i]);
                    return EXIT_FAILURE;
                }
                i++;
            }
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
            if (parse_int_range(argv[i + 1], 1, 1024, &v) < 0) {
                fprintf(stderr, "invalid --reflect-batch '%s' (expected 1..1024)\n",
                        argv[i + 1]);
                return EXIT_FAILURE;
            }
            reflect_batch = (uint16_t)v;
            i += 2;
        } else if (strcmp(argv[i], "--analysis-warmup-batches") == 0 ||
                   strcmp(argv[i], "--analysis-samples") == 0) {
            int v;
            bool is_samples = strcmp(argv[i], "--analysis-samples") == 0;

            if (i + 1 >= argc || parse_int_range(argv[i + 1], 0, 1000000, &v) < 0) {
                fprintf(stderr, "%s requires an integer in 0..1000000\n", argv[i]);
                return EXIT_FAILURE;
            }
            if (is_samples)
                analysis.samples = (uint32_t)v;
            else
                analysis.warmup_batches = (uint32_t)v;
            analysis_option_seen = true;
            i += 2;
        } else if (strcmp(argv[i], "--analysis-await-completion") == 0) {
            analysis_await_completion = true;
            analysis_option_seen = true;
            i++;
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

    if (!run_rx_reflect_mode) {
        fprintf(stderr, "the only supported mode is --rx-reflect [seconds]\n");
        return EXIT_FAILURE;
    }
#if defined(MY_ICE_ANALYSIS)
    analysis.enabled = analysis_option_seen;
    if (analysis.enabled && analysis.samples == 0) {
        fprintf(stderr, "--analysis-samples must be greater than zero\n");
        return EXIT_FAILURE;
    }
    if (analysis_await_completion) {
        fprintf(stderr,
                "--analysis-await-completion is not available: only the submission region is supported\n");
        return EXIT_FAILURE;
    }
#else
    if (analysis_option_seen) {
        (void)analysis_await_completion;
        fprintf(stderr, "analysis options require an ANALYSIS=1 build\n");
        return EXIT_FAILURE;
    }
#endif

    d.pin_cpus = pin_cpus;
    d.tx_desc_count = tx_desc_count;
    d.reflect_batch = reflect_batch > MAX_REFLECT_BATCH ? MAX_REFLECT_BATCH : reflect_batch;
    alloc_queue_sw_state(&d);
    aq_set_topology_options(dump_topo, qparent_override_set, qparent_override);

    MY_ICE_INFO("[my_ice] opening VFIO device %s\n", bdf);
    if (ice_vfio_open(&d, bdf) < 0)
        goto out;
    MY_ICE_INFO("[my_ice] vfio init done, bar0_size=0x%zx\n", d.bar0_size);

    if (use_hugepages) {
        MY_ICE_INFO("[my_ice] using hugepages from %s\n", huge_dir);
        prepare_hugepage_file(&d, ice_dma_required_bytes(&d), huge_dir);
    }

    if (ice_vfio_init(&d) < 0) {
        fprintf(stderr, "[my_ice] initialization failed\n");
        goto out;
    }

    if (run_rx_reflect(&d, rx_reflect_timeout_s * 1000, reflect_batch, &analysis) < 0) {
        fprintf(stderr, "[my_ice] error: rx-reflect failed\n");
        goto out;
    }

    rc = EXIT_SUCCESS;

out:
    ice_vfio_close(&d);
    return rc;
}
