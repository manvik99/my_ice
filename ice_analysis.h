#ifndef ICE_ANALYSIS_H
#define ICE_ANALYSIS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* Outcome values are part of the stable analysis record/marker ABI. */
enum ice_analysis_outcome {
    ICE_ANALYSIS_ACCEPTED = 0,
    ICE_ANALYSIS_REJECT_PARTIAL_RX = 1,
    ICE_ANALYSIS_REJECT_PACKET = 2,
    ICE_ANALYSIS_REJECT_ERROR = 3,
    ICE_ANALYSIS_REJECT_PENDING_DOORBELL = 4,
};

struct ice_analysis_config {
    bool enabled;
    uint32_t warmup_batches;
    uint32_t samples;
};

static inline void ice_analysis_marker_begin(uint64_t sample_id, uint32_t packet_count)
{
#if defined(MY_ICE_ANALYSIS)
    extern void analysis_sample_begin(uint64_t, uint32_t);
    analysis_sample_begin(sample_id, packet_count);
#else
    (void)sample_id;
    (void)packet_count;
#endif
}

static inline void ice_analysis_marker_end(uint64_t sample_id, uint32_t packet_count,
                                           enum ice_analysis_outcome outcome)
{
#if defined(MY_ICE_ANALYSIS)
    extern void analysis_sample_end(uint64_t, uint32_t, uint32_t);
    analysis_sample_end(sample_id, packet_count, (uint32_t)outcome);
#else
    (void)sample_id;
    (void)packet_count;
    (void)outcome;
#endif
}

static inline void ice_analysis_marker_rejected(uint64_t sample_id,
                                                enum ice_analysis_outcome outcome)
{
#if defined(MY_ICE_ANALYSIS)
    extern void analysis_sample_rejected(uint64_t, uint32_t);
    analysis_sample_rejected(sample_id, (uint32_t)outcome);
#else
    (void)sample_id;
    (void)outcome;
#endif
}

static inline const char *ice_analysis_outcome_name(enum ice_analysis_outcome outcome)
{
    switch (outcome) {
    case ICE_ANALYSIS_ACCEPTED:
        return "accepted";
    case ICE_ANALYSIS_REJECT_PARTIAL_RX:
        return "partial_rx";
    case ICE_ANALYSIS_REJECT_PACKET:
        return "packet_rejected";
    case ICE_ANALYSIS_REJECT_ERROR:
        return "error";
    case ICE_ANALYSIS_REJECT_PENDING_DOORBELL:
        return "pending_doorbell";
    }
    return "unknown";
}

static inline void ice_analysis_record(FILE *stream, uint64_t sample_id,
                                       uint32_t warmup_accepted, uint16_t batch,
                                       enum ice_analysis_outcome outcome)
{
    fprintf(stream,
            "[my_ice-analysis] sample_id=%llu status=%s reason=%s packet_count=%u "
            "effective_batch=%u warmup_accepted=%u region=%s tool=markers\n",
            (unsigned long long)sample_id,
            outcome == ICE_ANALYSIS_ACCEPTED ? "accepted" : "rejected",
            ice_analysis_outcome_name(outcome), batch, batch, warmup_accepted,
            "submission");
}

#endif
