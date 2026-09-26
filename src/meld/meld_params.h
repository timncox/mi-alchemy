/*
 * meld_params.h -- what Meld adds to Warps' panel mapping, shared by
 * meld_alchemy.cpp and test/test_meld.cpp so the test proves the firmware's
 * own arithmetic.
 *
 *   MapStock   the stock-Warps "Warps" mode's parameters, from the Parasites
 *              mapping (src/warps/warps_params.h, used unchanged): Meld's
 *              map_params() tail (~/tim-os/meld/firmware/meld_patch.cpp)
 *   Extras     Meld's Patch extras on the raw inputs, per sample: the two
 *              envelope followers (1 ms attack, 50 ms release) and the
 *              coincidence clock (rising zero crossings of carrier and
 *              modulator within kWindow samples fire a kGateMs gate)
 *   Cv2Volts   CV 2: the carrier envelope or the internal note, 1 V/oct
 */
#pragma once

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "warps/dsp/parameters.h"
#include "warps_stock/dsp/parameters.h"
#include "warps_params.h"

namespace mi_meld {

/* The MODE selector: META first, Parasites' eight in their enum order (the
 * order src/warps uses), then stock Warps as the tenth, index 9. */
static constexpr int kNumModes  = 10;
static constexpr int kModeStock = 9;
static const warps::FeatureMode kModeOrder[kNumModes - 1] = {
    warps::FEATURE_MODE_META, warps::FEATURE_MODE_DOPPLER, warps::FEATURE_MODE_FOLD,
    warps::FEATURE_MODE_CHEBYSCHEV, warps::FEATURE_MODE_FREQUENCY_SHIFTER,
    warps::FEATURE_MODE_BITCRUSHER, warps::FEATURE_MODE_COMPARATOR,
    warps::FEATURE_MODE_VOCODER, warps::FEATURE_MODE_DELAY};

/* The stock modulator reads the same fields minus Parasites' raw_* set,
 * plus the easter-egg ones, which its meta mode never touches.
 * modulation_algorithm passes through unclamped: its last hair puts stock's
 * vocoder into Warps' spectral freeze (vocoder.cc: release > 0.995 freezes
 * the envelope followers). That is a feature; a freeze from silence holds
 * silence. */
static inline void MapStock(const warps::Parameters& p, warps_stock::Parameters* s)
{
    s->channel_drive[0]     = p.channel_drive[0];
    s->channel_drive[1]     = p.channel_drive[1];
    s->modulation_algorithm = p.modulation_algorithm;
    s->modulation_parameter = p.modulation_parameter;
    s->frequency_shift_pot  = 0.5f;
    s->frequency_shift_cv   = 0.0f;
    s->phase_shift          = 0.0f;
    s->note                 = p.note;
    s->carrier_shape        = p.carrier_shape;
}

/* CV 2's source (SETUP P3). AUTO is Meld's rule: the note while the
 * internal carrier is on, the carrier's envelope while it is external. */
enum Cv2Source { CV2_AUTO = 0, CV2_ENVELOPE = 1, CV2_NOTE = 2 };

/* Envelope 0..1 -> 0..+5 V. Note: Meld spread notes 36..96 over its 0-5 V
 * DAC, which is exactly 1 V/oct from note 36 (C2); kept, clamped 0..5 V. */
static inline float Cv2Volts(int source, int carrier_shape, float carrier_env, float note)
{
    const bool note_mode = source == CV2_NOTE || (source == CV2_AUTO && carrier_shape != 0);
    float v = note_mode ? (note - 36.0f) / 12.0f : 5.0f * carrier_env;
    return v < 0.f ? 0.f : (v > 5.f ? 5.f : v);
}

struct Extras
{
    static constexpr int      kWindow = 2;          /* samples */
    static constexpr int      kNoCross = -100000;
    static constexpr uint32_t kGateMs = 5;

    float env[2]        = {0.f, 0.f};   /* carrier (J1), modulator (J2), 0..1 */
    float att = 0.f, rel = 0.f;
    bool  above[2]      = {false, false};
    int   last_cross[2] = {kNoCross, kNoCross};
    uint32_t fires      = 0;            /* coincidences since Init */

    void Init(float sample_rate)
    {
        att = 1.0f - expf(-1.0f / (0.001f * sample_rate));
        rel = 1.0f - expf(-1.0f / (0.050f * sample_rate));
        env[0] = env[1] = 0.f;
        above[0] = above[1] = false;
        last_cross[0] = last_cross[1] = kNoCross;
        fires = 0;
    }

    /* One block of raw inputs (-1..1). Returns true when the coincidence
     * clock fired somewhere in the block. Meld's callback, unchanged. */
    bool Process(const float* x, const float* y, size_t size)
    {
        bool fired = false;
        for (size_t i = 0; i < size; i++)
        {
            const float v[2] = {x[i], y[i]};
            for (int c = 0; c < 2; c++)
            {
                const float a = fabsf(v[c]);
                env[c] += (a > env[c] ? att : rel) * (a - env[c]);
            }
            bool fresh = false;
            for (int c = 0; c < 2; c++)
            {
                const bool up = v[c] > 0.0f;
                if (up && !above[c]) { last_cross[c] = (int)i; fresh = true; }
                above[c] = up;
            }
            /* A crossing from the previous block carries a negative index
             * (the ageing below), so a pair straddling the edge counts. */
            if (fresh && last_cross[0] != kNoCross && last_cross[1] != kNoCross
                && abs(last_cross[0] - last_cross[1]) <= kWindow)
            {
                fired = true;
                fires++;
                last_cross[0] = last_cross[1] = kNoCross;
            }
        }
        /* Age pending crossings into the next block's index space; anything
         * older than the window can never match and goes back to none. */
        for (int c = 0; c < 2; c++)
        {
            if (last_cross[c] == kNoCross) continue;
            last_cross[c] -= (int)size;
            if (last_cross[c] < -kWindow - 1) last_cross[c] = kNoCross;
        }
        return fired;
    }
};

} // namespace mi_meld
