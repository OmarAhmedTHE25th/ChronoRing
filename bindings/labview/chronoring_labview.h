#ifndef CHRONORING_LABVIEW_H
#define CHRONORING_LABVIEW_H

/* National Instruments LabVIEW Call Library Function Node (CLFN) header wrapper. */
#include "chronoring/c_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Direct scalar LabVIEW-friendly helpers for CLFN wizard import. */
static inline int32_t lv_chronoring_push_scalar(chronoring_spsc_t* ring,
                                                uint64_t ts_ns,
                                                uint64_t seq,
                                                uint32_t detector_id,
                                                float energy_mev) {
    chronoring_event_frame_t ev = {0};
    ev.timestamp_ns = ts_ns;
    ev.sequence_id = seq;
    ev.detector_id = detector_id;
    ev.energy_mev = energy_mev;
    ev.quality_flags = 3u;
    ev.multiplicity = 1;
    return chronoring_spsc_try_push(ring, &ev);
}

#ifdef __cplusplus
}
#endif

#endif /* CHRONORING_LABVIEW_H */
