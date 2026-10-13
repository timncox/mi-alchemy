/*
 * Beads' pot laws, from beads/cv_reader.h (PotChannel::Process), which can't
 * be included here: it pulls in the module's ADC drivers. Same arithmetic,
 * applied to the SDK's 0..1 knob value instead of a 16-bit ADC reading.
 * Copyright 2021 Emilie Gillet, MIT (as the vendored tree).
 */
#pragma once

#include <math.h>
#include <stdint.h>

#include "stmlib/dsp/dsp.h"

namespace mi_beads {

/* LAW_ATTENUVERTER: -1..+1 with a dead band 3 % either side of noon. */
inline float AttenuverterLaw(float v)
{
    v -= 0.5f;
    const float a = fabsf(v) - 0.03f;
    return (2.0f / 0.47f) * v * (a > 0.0f ? a : 0.0f);
}

/* LAW_DETUNE: PITCH in semitones, -24..+24. Six equal sectors either side
 * of noon, each a plateau on a musical interval for its first third and a
 * smoothstep to the next one after it. */
inline float DetuneLaw(float v)
{
    float       detune = 2.0f * v - 1.0f;
    const float sign   = detune < 0.0f ? -1.0f : 1.0f;
    detune             = detune * sign * 6.0f;
    MAKE_INTEGRAL_FRACTIONAL(detune);
    if (detune_integral > 6)
    {
        detune_integral   = 6;
        detune_fractional = 0.0f;
    }

    static const float intervals[8] = {0.0f, 1.0f, 3.0f, 4.0f, 7.0f, 12.0f, 24.0f, 24.0f};

    detune_fractional = (detune_fractional - 0.33333333f) * 1.5f;
    if (detune_fractional < 0.0f) detune_fractional = 0.0f;

    const float a = intervals[detune_integral];
    const float b = intervals[detune_integral + 1];
    return stmlib::Crossfade(a, b, stmlib::SmoothStep(detune_fractional)) * sign;
}

/* Beads' State::gain: 0 is automatic, 1..255 manual (GainControl maps it to
 * x1..x40 on a fourth-power curve). The bottom 4 % of the knob is AUTO. */
inline uint8_t GainSetting(float v)
{
    if (v < 0.04f) return 0u;
    const float g = 1.0f + (v - 0.04f) / 0.96f * 254.0f;
    return (uint8_t)fminf(g + 0.5f, 255.0f);
}

}  // namespace mi_beads
