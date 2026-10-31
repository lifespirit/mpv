/*
 * Host-side experimental USB shutter phase ramp.
 *
 * This is ONLY deadline math against DRM/KMS UST (CLOCK_MONOTONIC).
 * It cannot measure or change actual IR/lens timing. Do not let the
 * effective phase cross the midpoint of the physical L/R period:
 * crossing a period would require an explicit MSC/eye parity remap.
 */
#ifndef MPV_STEREO_USB_PHASE_RAMP_H
#define MPV_STEREO_USB_PHASE_RAMP_H

#include <stdbool.h>
#include <stdint.h>

#define STEREO_USB_PHASE_LIMIT_NS INT64_C(4000000)
#define STEREO_USB_NSEC_PER_SEC INT64_C(1000000000)

/* The configured rate is in nanoseconds of phase change per second;
 * 7000 ns/s = 7 us/s. Split at the second boundary so intermediate
 * multiplication cannot overflow during ordinary multi-hour playback.
 * Caller restricts rate to +/-20000 ns/s and elapsed_ns >= 0. */
static inline int64_t stereo_usb_phase_ramp_ns(int64_t elapsed_ns,
                                               int64_t rate_ns_per_s)
{
    if (elapsed_ns <= 0 || !rate_ns_per_s)
        return 0;
    return (elapsed_ns / STEREO_USB_NSEC_PER_SEC) * rate_ns_per_s +
           ((elapsed_ns % STEREO_USB_NSEC_PER_SEC) * rate_ns_per_s) /
           STEREO_USB_NSEC_PER_SEC;
}

static inline int64_t stereo_usb_effective_phase_ns(int manual_phase_us,
                                                    int64_t elapsed_ns,
                                                    int64_t rate_ns_per_s,
                                                    bool *clamped)
{
    int64_t requested = (int64_t)manual_phase_us * 1000 +
                        stereo_usb_phase_ramp_ns(elapsed_ns, rate_ns_per_s);
    bool saturated = false;
    if (requested < -STEREO_USB_PHASE_LIMIT_NS) {
        requested = -STEREO_USB_PHASE_LIMIT_NS;
        saturated = true;
    } else if (requested > STEREO_USB_PHASE_LIMIT_NS) {
        requested = STEREO_USB_PHASE_LIMIT_NS;
        saturated = true;
    }
    if (clamped)
        *clamped = saturated;
    return requested;
}

#endif /* MPV_STEREO_USB_PHASE_RAMP_H */
