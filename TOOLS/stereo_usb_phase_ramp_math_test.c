/*
 * Hardware-free regression for the experimental stereo USB phase ramp.
 *
 * Compile from repository root:
 *   cc -std=c11 -Wall -Wextra -Werror -o /tmp/stereo-phase-math \
 *      TOOLS/stereo_usb_phase_ramp_math_test.c
 *   /tmp/stereo-phase-math
 *
 * These tests cover host deadline calculations, not optical/firmware timing.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../video/out/opengl/stereo_usb_phase_ramp.h"

static int64_t seconds(int64_t n)
{
    return n * STEREO_USB_NSEC_PER_SEC;
}

int main(void)
{
    bool clamped = true;
    // With drift disabled, preserve exact old manual phase at all times.
    assert(stereo_usb_effective_phase_ns(-1500, 0, 0, &clamped) == -1500000);
    assert(!clamped);
    assert(stereo_usb_effective_phase_ns(-1500, seconds(300), 0,
                                         &clamped) == -1500000);
    assert(!clamped);

    // +7 us/s is +420us after one minute, +1680us after four.
    assert(stereo_usb_phase_ramp_ns(seconds(60), 7000) == 420000);
    assert(stereo_usb_phase_ramp_ns(seconds(240), 7000) == 1680000);
    assert(stereo_usb_effective_phase_ns(-1500, seconds(240), 7000,
                                         &clamped) == 180000);
    assert(!clamped);

    // -7 us/s moves earlier, stays within bounds for 300s.
    assert(stereo_usb_effective_phase_ns(-1500, seconds(240), -7000,
                                         &clamped) == -3180000);
    assert(!clamped);
    assert(stereo_usb_effective_phase_ns(-1500, seconds(300), -7000,
                                         &clamped) == -3600000);
    assert(!clamped);

    // Exactly 0.5s at +7us/s = +3500ns, and 8ms = +56ns.
    assert(stereo_usb_phase_ramp_ns(500000000, 7000) == 3500);
    assert(stereo_usb_phase_ramp_ns(8000000, 7000) == 56);

    // Never wrap a whole vblank or change eye parity at the safety limit.
    assert(stereo_usb_effective_phase_ns(-1500, seconds(360), -7000,
                                         &clamped) == -4000000);
    assert(clamped);
    assert(stereo_usb_effective_phase_ns(-1500, seconds(360), 7000,
                                         &clamped) == 1020000);
    assert(!clamped);

    // Manual tuning alters baseline but does not restart ramp integration.
    assert(stereo_usb_effective_phase_ns(-1475, seconds(60), 7000,
                                         &clamped) == -1055000);
    assert(!clamped);
    assert(stereo_usb_effective_phase_ns(-1525, seconds(60), 7000,
                                         &clamped) == -1105000);
    assert(!clamped);

    // Rate sign and zero elapsed are well-defined.
    assert(stereo_usb_phase_ramp_ns(-1, -7000) == 0);
    assert(stereo_usb_phase_ramp_ns(seconds(1), -7000) == -7000);

    puts("PASS: stereo USB host phase ramp arithmetic; optics NOT measured");
    return 0;
}
