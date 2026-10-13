/*
 * Native test for Beads' engine as the firmware drives it: the same buffers
 * (sizes from beads_alchemy.cpp), 48 kHz, 12-frame blocks, GainControl in
 * front, and the 48 <-> 32 kHz resampler around the Clouds quality.
 *
 *   1. every QUALITY x {stereo, mono} renders 3 s of a 220 Hz tone through
 *      dense random grains: finite, and the wet signal is not silent
 *   2. delay mode (SIZE fully clockwise) sounds once its 1.5 s crossfade
 *      has run
 *   3. freeze holds the recording: frozen with silent input it keeps
 *      sounding; unfrozen it dies away
 *   4. the wavetable synth renders at all four resolutions
 *   5. the resampler: unity in the passband, 20 kHz rejected
 *   6. the pot laws (beads_laws.h): pitch detents, attenuverter, gain
 *   7. timing: seconds of host CPU per second of audio, for the record
 */
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "beads/dsp/processor.h"
#include "beads/dsp/synth.h"
#include "beads/gain_control.h"
#include "stmlib/utils/gate_flags.h"

#include "../src/beads/beads_laws.h"
#include "../src/beads/resample_3_2.h"

using namespace beads;

static const size_t kBlock   = 12;
static const size_t kBlock32 = 8;
static const float  kSr      = 48000.0f;

static uint8_t  delay_buffer_1[512 * 1024];
static uint8_t  delay_buffer_2[256 * 1024];
static uint16_t reverb_buffer[32768];

static Processor            processor;
static Synth                synth;
static GainControl          gain;
static mi_beads::Resample32k src32;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

struct Stats { double rms; bool finite; };

struct Knobs
{
    float time = 0.3f, size = 0.5f, pitch = 0.f, shape = 0.5f, density = 0.75f;
    float dry_wet = 1.0f, feedback = 0.f, reverb = 0.f;
    float time_amt = 0.f, size_amt = 0.f, pitch_amt = 0.f, shape_amt = 0.f;
    bool  freeze = false;
};

static void fresh(Processor::Media media, bool mono)
{
    memset(delay_buffer_1, 0, sizeof delay_buffer_1);
    memset(delay_buffer_2, 0, sizeof delay_buffer_2);
    memset(reverb_buffer, 0, sizeof reverb_buffer);
    uint8_t* bufs[3] = {delay_buffer_1, delay_buffer_1 + 256 * 1024, delay_buffer_2};
    processor.Init(bufs, reverb_buffer);
    processor.Configure(media, mono);
    gain.Init(kSr, GainControl::SOFT_CLIPPING_MODE_OFF, true);
    src32.Init();
}

/* Render `seconds` the way AudioCallback does: tone (or silence) in. */
static Stats run(const Knobs& k, float seconds, bool tone, double& phase,
                 bool synth_mode = false, Synth::Resolution res = Synth::RESOLUTION_16_BIT,
                 bool mono = false)
{
    Codec::Frame in[kBlock];
    FloatFrame   f_in[kBlock], f_out[kBlock], e_in[kBlock32], e_out[kBlock32];
    bool         freeze[kBlock];
    const int    blocks = (int)(seconds * kSr / kBlock);
    double       acc    = 0.0;
    long         n      = 0;
    bool         finite = true;

    Parameters p;
    p.time  = {k.time, k.time_amt, 0.f, false};
    p.size  = {k.size, k.size_amt, 0.f, false};
    p.shape = {k.shape, k.shape_amt, 0.f, false};
    p.pitch = {k.pitch, k.pitch_amt, 0.f, false};
    p.monophonic_mode = false;
    ModulatedParameter d = {k.density, 0.f};
    Processor::MixParameters m = {k.feedback, k.dry_wet, k.reverb, false};

    for (int b = 0; b < blocks; b++)
    {
        for (size_t i = 0; i < kBlock; i++)
        {
            const float s = tone ? 0.5f * (float)std::sin(phase) : 0.f;
            phase += 2.0 * M_PI * 220.0 / kSr;
            in[i].l = (int32_t)(s * 2147483647.0f);
            in[i].r = (int32_t)(-s * 2147483647.0f);
            freeze[i] = k.freeze;
        }
        gain.Process(true, 0, synth_mode, synth_mode || mono, in, f_in, kBlock);
        if (synth_mode)
            synth.Render(SEED_BUTTON_STATE_LOCKED, nullptr, freeze, res, d, p, m, f_out, kBlock);
        else if (processor.sample_rate() == 32000.0f)
        {
            const size_t e = src32.Down(reinterpret_cast<mi_beads::StereoF*>(f_in),
                                        reinterpret_cast<mi_beads::StereoF*>(e_in), kBlock);
            CHECK(e == kBlock32, "resampler gave %zu frames, not 8", e);
            processor.Process(SEED_BUTTON_STATE_LOCKED, nullptr, freeze, d, p, m, e_in, e_out, e);
            const size_t o = src32.Up(reinterpret_cast<mi_beads::StereoF*>(e_out),
                                      reinterpret_cast<mi_beads::StereoF*>(f_out), e);
            CHECK(o == kBlock, "resampler gave %zu frames back, not 12", o);
        }
        else
            processor.Process(SEED_BUTTON_STATE_LOCKED, nullptr, freeze, d, p, m, f_in, f_out, kBlock);

        for (size_t i = 0; i < kBlock; i++)
        {
            const float l = f_out[i].l, r = f_out[i].r;
            if (!std::isfinite(l) || !std::isfinite(r)) finite = false;
            acc += l * l + r * r;
            n += 2;
        }
    }
    return {std::sqrt(acc / (double)(n ? n : 1)), finite};
}

/* RMS of a sine through Down + Up, after the filters settle. */
static double src_gain(double hz)
{
    src32.Init();
    mi_beads::StereoF in[kBlock], mid[kBlock32], out[kBlock];
    double ph = 0.0, acc_in = 0.0, acc_out = 0.0;
    for (int b = 0; b < 4000; b++)
    {
        for (size_t i = 0; i < kBlock; i++)
        {
            in[i].l = in[i].r = (float)std::sin(ph);
            ph += 2.0 * M_PI * hz / kSr;
            if (b >= 100) acc_in += in[i].l * in[i].l;
        }
        const size_t e = src32.Down(in, mid, kBlock);
        src32.Up(mid, out, e);
        if (b >= 100)
            for (size_t i = 0; i < kBlock; i++) acc_out += out[i].l * out[i].l;
    }
    return std::sqrt(acc_out / acc_in);
}

int main()
{
    static const char* kMedia[4] = {"tape", "clean lo-fi", "clouds", "hi-fi"};

    printf("1. qualities x stereo/mono, 3 s of 220 Hz, dense random grains, wet only\n");
    double total_audio = 0.0, total_cpu = 0.0;
    for (int media = 3; media >= 0; media--)
        for (int mono = 0; mono < 2; mono++)
        {
            fresh((Processor::Media)media, mono);
            Knobs k;
            /* TIME is a fraction of the whole buffer (4 s hi-fi, up to 16 s
             * mono tape): stay within the 3 s recorded. */
            k.time      = 0.05f;
            k.time_amt  = 0.3f;
            k.pitch_amt = -0.3f;
            double     ph = 0.0;
            const auto t0 = std::chrono::steady_clock::now();
            const Stats s = run(k, 3.0f, true, ph, false, Synth::RESOLUTION_16_BIT, mono);
            const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            total_audio += 3.0;
            total_cpu += dt;
            printf("   %-12s %-6s rms %.4f  %s\n", kMedia[media], mono ? "mono" : "stereo",
                   s.rms, s.finite ? "" : "NON-FINITE");
            CHECK(s.finite, "%s produced non-finite samples", kMedia[media]);
            CHECK(s.rms > 0.005, "%s %s is silent (rms %.5f)", kMedia[media],
                  mono ? "mono" : "stereo", s.rms);
        }

    printf("2. delay mode (SIZE fully clockwise), hi-fi and clouds\n");
    for (int media : {3, 2})
    {
        fresh((Processor::Media)media, false);
        Knobs k;
        k.size     = 1.0f;
        k.feedback = 0.3f;
        double ph = 0.0;
        run(k, 2.0f, true, ph);   /* past the 1.5 s granular -> delay fade */
        const Stats s = run(k, 1.0f, true, ph);
        printf("   %-7s rms %.4f\n", kMedia[media], s.rms);
        CHECK(s.finite && s.rms > 0.005, "%s delay mode is silent (rms %.5f)", kMedia[media], s.rms);
    }

    printf("3. freeze holds the recording (hi-fi stereo)\n");
    {
        fresh(Processor::MEDIA_DIGITAL, false);
        Knobs  k;
        double ph = 0.0;
        run(k, 2.0f, true, ph);
        k.freeze = true;
        const Stats frozen = run(k, 2.0f, false, ph);
        fresh(Processor::MEDIA_DIGITAL, false);
        k.freeze = false;
        run(k, 2.0f, true, ph);
        run(k, 5.0f, false, ph);   /* the 4 s buffer fills with silence */
        const Stats open = run(k, 0.5f, false, ph);
        printf("   frozen, silent input: rms %.4f   unfrozen: rms %.5f\n", frozen.rms, open.rms);
        CHECK(frozen.rms > 0.01, "frozen buffer went quiet (rms %.5f)", frozen.rms);
        CHECK(open.rms < frozen.rms * 0.1, "unfrozen output did not die away");
    }

    printf("4. wavetable synth, four resolutions\n");
    synth.Init(reverb_buffer);
    for (int r = 0; r < 4; r++)
    {
        Knobs  k;
        k.dry_wet  = 0.5f;
        k.feedback = 0.25f;   /* the bank */
        double ph = 0.0;
        gain.Init(kSr, GainControl::SOFT_CLIPPING_MODE_OFF, true);
        const Stats s = run(k, 2.0f, false, ph, true, (Synth::Resolution)r);
        printf("   resolution %d rms %.4f\n", r, s.rms);
        CHECK(s.finite && s.rms > 0.005, "synth resolution %d is silent (rms %.5f)", r, s.rms);
    }

    printf("5. 48 <-> 32 kHz resampler\n");
    {
        const double g1k = src_gain(1000.0), g10k = src_gain(10000.0), g20k = src_gain(20000.0);
        printf("   1 kHz %+.2f dB   10 kHz %+.2f dB   20 kHz %+.1f dB\n",
               20 * std::log10(g1k), 20 * std::log10(g10k), 20 * std::log10(g20k));
        CHECK(std::fabs(20 * std::log10(g1k)) < 0.1, "1 kHz is not unity");
        CHECK(std::fabs(20 * std::log10(g10k)) < 1.0, "10 kHz is not flat");
        CHECK(20 * std::log10(g20k) < -40.0, "20 kHz gets through");
    }

    printf("6. pot laws\n");
    {
        using namespace mi_beads;
        printf("   pitch: %.2f %.2f %.2f st; attenuverter: %.3f %.3f %.3f; gain %d %d\n",
               DetuneLaw(0.f), DetuneLaw(0.5f), DetuneLaw(1.f), AttenuverterLaw(0.f),
               AttenuverterLaw(0.5f), AttenuverterLaw(1.f), GainSetting(0.f), GainSetting(1.f));
        CHECK(DetuneLaw(0.5f) == 0.f, "PITCH noon is not unison");
        CHECK(std::fabs(DetuneLaw(1.f) - 24.f) < 1e-3 && std::fabs(DetuneLaw(0.f) + 24.f) < 1e-3,
              "PITCH range is not +/-24 st");
        /* Each sector plateaus on its interval for its first third: */
        CHECK(std::fabs(DetuneLaw(0.5f + 4.1f / 12.0f) - 7.f) < 1e-3, "no fifth detent");
        CHECK(AttenuverterLaw(0.5f) == 0.f && AttenuverterLaw(0.51f) == 0.f, "no dead band");
        CHECK(std::fabs(AttenuverterLaw(1.f) - 1.f) < 1e-3 && std::fabs(AttenuverterLaw(0.f) + 1.f) < 1e-3,
              "attenuverter range is not +/-1");
        CHECK(GainSetting(0.f) == 0 && GainSetting(0.03f) == 0 && GainSetting(0.05f) >= 1 &&
              GainSetting(1.f) == 255, "gain law");
    }

    printf("7. host time: %.3f s CPU per s of audio (granular, averaged)\n",
           total_cpu / total_audio);

    if (failures)
    {
        printf("beads: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("beads: all checks passed\n");
    return 0;
}
