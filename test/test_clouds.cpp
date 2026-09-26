/*
 * Native test for Clouds' engine as the firmware drives it: the same
 * buffers (sizes from clouds_alchemy.cpp), 32 kHz, 32-frame blocks,
 * Prepare() between blocks the way the firmware's main loop calls it.
 *
 *   1. every MODE x QUALITY renders 3 s of a 220 Hz tone: finite, and the
 *      wet signal is not silent
 *   2. freeze holds the buffer: after 1.5 s of tone, freeze and feed
 *      silence -- granular output keeps sounding; unfrozen it dies
 *   3. PITCH: the knob's quantized curve at noon is 0 semitones, and the
 *      extremes reach -24 / +24 (the firmware adds V/Oct on top)
 *   4. timing: seconds of host CPU per second of audio, for the record
 */
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "clouds/dsp/granular_processor.h"
#include "clouds/resources.h"
#include "stmlib/dsp/dsp.h"

using namespace clouds;

static const size_t kBlock = kMaxBlockSize;   /* 32 */
static const float  kSr    = 32000.0f;

static uint8_t block_mem[118784];
static uint8_t block_ccm[65536 - 128];

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

struct Stats { double rms; bool finite; };

/* Render `seconds`, tone (or silence) in, Prepare() between blocks. */
static int g_prepares = 8;   /* Prepare() calls per block (main-loop spins) */

static Stats run(GranularProcessor& p, float seconds, bool tone, double& phase)
{
    ShortFrame in[kBlock], out[kBlock];
    const int blocks = (int)(seconds * kSr / kBlock);
    double    acc    = 0.0;
    long      n      = 0;
    bool      finite = true;
    for (int b = 0; b < blocks; b++)
    {
        for (size_t i = 0; i < kBlock; i++)
        {
            const float s = tone ? 0.5f * sinf((float)phase) : 0.0f;
            phase += 2.0 * M_PI * 220.0 / kSr;
            in[i].l = in[i].r = (int16_t)(s * 32767.0f);
        }
        p.Process(in, out, kBlock);
        for (int k = 0; k < g_prepares; k++) p.Prepare();
        for (size_t i = 0; i < kBlock; i++)
        {
            const double l = out[i].l / 32768.0, r = out[i].r / 32768.0;
            if (!std::isfinite(l) || !std::isfinite(r)) finite = false;
            acc += l * l + r * r;
            n += 2;
        }
    }
    return {std::sqrt(acc / (double)(n ? n : 1)), finite};
}

static void set_params(GranularProcessor& p, bool freeze)
{
    Parameters* q    = p.mutable_parameters();
    q->position      = 0.3f;
    q->size          = 0.5f;
    q->pitch         = 0.0f;
    q->density       = 0.85f;   /* dense, randomly seeded grains */
    q->texture       = 0.5f;
    q->dry_wet       = 0.99999f; /* wet only, capped as the firmware caps it */
    q->stereo_spread = 0.3f;
    q->feedback      = 0.0f;
    q->reverb        = 0.0f;
    q->freeze        = freeze;
    q->trigger       = false;
    q->gate          = false;
}

static GranularProcessor* fresh(int mode, int quality)
{
    static GranularProcessor p;
    memset(block_mem, 0, sizeof block_mem);
    memset(block_ccm, 0, sizeof block_ccm);
    p.Init(block_mem, sizeof block_mem, block_ccm, sizeof block_ccm);
    p.set_playback_mode((PlaybackMode)mode);
    p.set_quality(quality);
    p.Prepare();   /* performs the reset the two setters flagged */
    set_params(p, false);
    return &p;
}

int main()
{
    static const char* kModes[4] = {"granular", "stretch", "looping", "spectral"};
    static const char* kQual[4]  = {"16/st", "16/mono", "8/st", "8/mono"};

    printf("1. modes x qualities, 3 s of 220 Hz, wet only\n");
    double total_audio = 0.0, total_cpu = 0.0;
    for (int m = 0; m < 4; m++)
        for (int q = 0; q < 4; q++)
        {
            GranularProcessor* p = fresh(m, q);
            double ph = 0.0;
            const auto t0 = std::chrono::steady_clock::now();
            const Stats s = run(*p, 3.0f, true, ph);
            const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            total_audio += 3.0;
            total_cpu += dt;
            printf("   %-9s %-8s rms %.4f  %s\n", kModes[m], kQual[q], s.rms, s.finite ? "" : "NON-FINITE");
            CHECK(s.finite, "%s %s produced non-finite samples", kModes[m], kQual[q]);
            CHECK(s.rms > 0.005, "%s %s is silent (rms %.5f)", kModes[m], kQual[q], s.rms);
        }

    printf("2. freeze holds the buffer (granular, 16-bit stereo)\n");
    {
        GranularProcessor* p = fresh(0, 0);
        double ph = 0.0;
        run(*p, 1.5f, true, ph);
        set_params(*p, true);
        const Stats frozen = run(*p, 1.5f, false, ph);
        p = fresh(0, 0);
        run(*p, 1.5f, true, ph);
        set_params(*p, false);
        run(*p, 1.2f, false, ph);   /* let the buffer fill with silence */
        const Stats open = run(*p, 0.3f, false, ph);
        printf("   frozen, silent input: rms %.4f   unfrozen: rms %.5f\n", frozen.rms, open.rms);
        CHECK(frozen.rms > 0.01, "frozen buffer went quiet (rms %.5f)", frozen.rms);
        CHECK(open.rms < frozen.rms * 0.1, "unfrozen output did not die away");
    }

    printf("3. pitch curve\n");
    {
        const float mid = stmlib::Interpolate(lut_quantized_pitch, 0.5f, 1024.0f);
        const float lo  = stmlib::Interpolate(lut_quantized_pitch, 0.0f, 1024.0f);
        const float hi  = stmlib::Interpolate(lut_quantized_pitch, 1.0f, 1024.0f);
        printf("   noon %.2f st, min %.2f, max %.2f\n", mid, lo, hi);
        CHECK(fabsf(mid) < 0.01f, "noon is not 0 semitones");
        CHECK(lo <= -23.9f && hi >= 23.9f, "range is not +/-2 octaves");
    }

    printf("4. host time: %.3f s CPU per s of audio (all modes averaged)\n",
           total_cpu / total_audio);

    if (failures)
    {
        printf("clouds: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("clouds: all checks passed\n");
    return 0;
}
