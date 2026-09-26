/*
 * warps_params.h -- the panel-to-Parameters mapping for Warps Parasites,
 * shared by warps_alchemy.cpp and test/test_warps.cpp so the test proves
 * the firmware's own arithmetic.
 *
 * It is Parasites' CvScaler::Read() (warps/cv_scaler.cc @ 32fa66f) in its
 * "nothing patched into the level CV jacks" branch, the one Warps takes when
 * its normalization probe finds the jacks empty. The Lab cannot probe, and
 * its CV reaches the knobs through the SDK's CV matrix (summed into the pot
 * value), so that branch is always the one taken:
 *
 *   channel_drive[i] = level_i ^ 2         raw_level[i] = level_i
 *   modulation_algorithm = lut_pot_curve(algo), with the 1.08x - 0.01
 *                          anti-bleed below 0.125
 *   modulation_parameter = timbre
 *   note = 60 * level_1 + 12 + 24  (+ 12 per volt on J5, + TUNE)
 *
 * On a real Warps the LEVEL 1 CV jack doubles as the internal oscillator's
 * V/Oct input; the Lab has a jack to spare, so V/Oct is J5 on its own.
 */
#pragma once

#include "stmlib/stmlib.h"
#include "stmlib/dsp/dsp.h"
#include "warps/dsp/parameters.h"
#include "warps/resources.h"

namespace mi_warps {

struct Panel
{
    float algorithm = 0.0f;  /* 0..1, the big knob */
    float timbre    = 0.5f;  /* 0..1, the small knob */
    float level[2]  = {0.5f, 0.5f};
    int   carrier   = 0;     /* 0..3: Warps' button state */
    float voct      = 0.0f;  /* volts on J5 */
    float tune      = 0.0f;  /* semitones, SETUP TUNE */
};

static inline float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

static inline void Map(const Panel& in, warps::Parameters* p)
{
    const float l0 = clamp01(in.level[0]), l1 = clamp01(in.level[1]);
    p->channel_drive[0] = l0 * l0;
    p->channel_drive[1] = l1 * l1;
    p->raw_level[0]     = l0;
    p->raw_level[1]     = l1;

    /* Interpolate() reads table[i] and table[i + 1] of lut_pot_curve's 513
     * entries, so an index of exactly 1.0 reads one past it. Warps' ADC
     * never returns 1.0; a Lab pot does (meld's note). */
    float big = clamp01(in.algorithm);
    if (big > 0.9999f) big = 0.9999f;
    const float unwrapped = stmlib::Interpolate(warps::lut_pot_curve, big, 512.0f);
    p->raw_algorithm_pot  = unwrapped;
    p->raw_algorithm_cv   = 0.0f;
    p->raw_algorithm      = clamp01(in.algorithm);

    float alg = unwrapped;
    if (alg <= 0.125f)
    {
        /* Warps: prevent wavefolder bleed from a slight pot offset. */
        alg = alg * 1.08f - 0.01f;
        CONSTRAIN(alg, 0.0f, 1.0f);
    }
    p->modulation_algorithm = alg;
    p->modulation_parameter = clamp01(in.timbre);

    p->note          = 60.0f * l0 + 12.0f + 24.0f + 12.0f * in.voct + in.tune;
    p->carrier_shape = in.carrier & 3;
}

} // namespace mi_warps
