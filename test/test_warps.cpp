/*
 * Native test for Warps Parasites as the mi-alchemy firmware drives it:
 * 96 kHz, 60-frame blocks, the Modulator placement-new'd into zeroed
 * memory, parameters set through src/warps/warps_params.h (the firmware's
 * own mapping). Ported from meld/test/test_modulator.cc (Parasites parts).
 *
 *   1. digital ring mod: 1000 Hz x 200 Hz -> 800 + 1200 Hz, inputs suppressed
 *   2. analog ring mod and crossfade produce output in range
 *   3. every Parasites mode x carrier state (4) runs 1 s finite, not stuck
 *      at full scale; DELAY in all four topologies exercises the read-index
 *      wrap restored from meld (vendor/VENDOR.md)
 *   4. Parasites' vocoder mode is alive with an internal saw carrier
 *   5. the mapping: V/Oct on J5 moves the internal carrier 12 st per volt,
 *      the algorithm knob at exactly 1.0 stays inside lut_pot_curve, and
 *      the internal carrier really follows the note (pitch measured)
 */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "warps/dsp/modulator.h"
#include "warps/dsp/parameters.h"
#include "warps_params.h"

using namespace warps;

static const float  kSampleRate = 96000.0f;
static const size_t kBlock      = 60;   /* Warps' own block, as the firmware */

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

static double power_at(const float* x, size_t n, double hz)
{
    const double w = 2.0 * M_PI * hz / kSampleRate, c = 2.0 * cos(w);
    double s1 = 0, s2 = 0;
    for (size_t i = 0; i < n; ++i) { const double s0 = x[i] + c * s1 - s2; s2 = s1; s1 = s0; }
    return (s1 * s1 + s2 * s2 - c * s1 * s2) / (double)n;
}

/* Zeroed memory first: Modulator::Init leaves some previous_parameters_
 * fields untouched (meld's lesson; the firmware memsets its D2 block). */
static Modulator* make(const mi_warps::Panel& panel)
{
    void*      mem = calloc(1, sizeof(Modulator));
    Modulator* m   = new (mem) Modulator();
    m->Init(kSampleRate);
    mi_warps::Map(panel, m->mutable_parameters());
    return m;
}
static void destroy(Modulator* m) { m->~Modulator(); free(m); }

/* `seconds` of two sines (carrier on L = J1, modulator on R = J2); keeps
 * the main output. With an internal carrier Warps uses input 1 as through-
 * zero FM into it, so internal-carrier tests keep amp_l at 0. */
static size_t run(Modulator* m, float f1, float f2, float seconds, float* out,
                  float amp_r = 0.5f, float amp_l = -1.f)
{
    if (amp_l < 0.f) amp_l = amp_r;
    ShortFrame in[kBlock], o[kBlock];
    const size_t total = (size_t)(seconds * kSampleRate);
    size_t n = 0;
    double p1 = 0, p2 = 0;
    while (n + kBlock <= total)
    {
        for (size_t i = 0; i < kBlock; ++i)
        {
            in[i].l = (short)(amp_l * 32767.0f * sin(p1));
            in[i].r = (short)(amp_r * 32767.0f * sin(p2));
            p1 += 2.0 * M_PI * f1 / kSampleRate;
            p2 += 2.0 * M_PI * f2 / kSampleRate;
        }
        m->Process(in, o, kBlock);
        for (size_t i = 0; i < kBlock; ++i) out[n + i] = o[i].l / 32768.0f;
        n += kBlock;
    }
    return n;
}

static float buf[96000 * 2];

int main()
{
    printf("sizeof(warps::Modulator) = %zu bytes\n", sizeof(Modulator));

    printf("1. digital ring mod, meta mode\n");
    {
        mi_warps::Panel pn;
        Modulator* m = make(pn);
        m->set_feature_mode(FEATURE_MODE_META);
        Parameters* p = m->mutable_parameters();
        p->modulation_algorithm = 3.0f / 8.0f;   /* algorithm index 3 */
        p->modulation_parameter = 0.0f;
        const size_t n = run(m, 1000.f, 200.f, 1.5f, buf);
        const float* x = buf + 48000; const size_t len = n - 48000;
        const double side = power_at(x, len, 800) + power_at(x, len, 1200);
        const double ins  = power_at(x, len, 200) + power_at(x, len, 1000);
        printf("   sidebands %.3g  inputs %.3g\n", side, ins);
        CHECK(side > 1e-4, "sidebands missing (%.3g)", side);
        CHECK(side > 20.0 * ins, "inputs not suppressed (%.3g vs %.3g)", side, ins);
        destroy(m);
    }

    printf("2. analog ring mod and crossfade\n");
    {
        const float idx[2] = {2.0f / 8.0f, 0.0f};
        const char* nm[2]  = {"analog ringmod", "crossfade"};
        for (int k = 0; k < 2; ++k)
        {
            mi_warps::Panel pn;
            pn.level[0] = pn.level[1] = 0.7071f;   /* drive = level^2 = 0.5, as meld's test */
            Modulator* m = make(pn);
            m->set_feature_mode(FEATURE_MODE_META);
            m->mutable_parameters()->modulation_algorithm = idx[k];
            m->mutable_parameters()->modulation_parameter = 0.5f;
            const size_t n = run(m, 1000.f, 200.f, 1.0f, buf);
            double rms = 0;
            for (size_t i = 48000; i < n; ++i) rms += buf[i] * buf[i];
            rms = sqrt(rms / (n - 48000));
            printf("   %-15s rms %.3f\n", nm[k], rms);
            CHECK(rms > 0.01 && rms < 1.0, "%s rms out of range (%.3f)", nm[k], rms);
            destroy(m);
        }
    }

    printf("3. every mode x carrier state, 1 s at 96 kHz\n");
    {
        const FeatureMode modes[] = {FEATURE_MODE_DOPPLER, FEATURE_MODE_FOLD, FEATURE_MODE_CHEBYSCHEV,
                                     FEATURE_MODE_FREQUENCY_SHIFTER, FEATURE_MODE_BITCRUSHER,
                                     FEATURE_MODE_COMPARATOR, FEATURE_MODE_VOCODER, FEATURE_MODE_DELAY,
                                     FEATURE_MODE_META};
        const char* names[] = {"doppler", "fold", "chebyschev", "freq shifter", "bitcrusher",
                               "comparator", "vocoder", "delay", "meta"};
        for (int shape = 0; shape <= 3; ++shape)
            for (size_t k = 0; k < sizeof modes / sizeof modes[0]; ++k)
            {
                mi_warps::Panel pn;
                pn.algorithm = 0.37f;
                pn.timbre    = 0.61f;
                pn.carrier   = shape;
                Modulator* m = make(pn);
                m->set_feature_mode(modes[k]);
                const size_t n = run(m, 440.f, 3.f, 1.0f, buf);
                size_t stuck = 0, bad = 0;
                double peak = 0;
                for (size_t i = 0; i < n; ++i)
                {
                    if (!std::isfinite(buf[i])) bad++;
                    if (fabsf(buf[i]) >= 0.999f) ++stuck;
                    if (fabs(buf[i]) > peak) peak = fabs(buf[i]);
                }
                printf("   %-13s carrier %d: peak %.3f, full-scale %zu/%zu\n", names[k], shape, peak, stuck, n);
                CHECK(bad == 0, "%s carrier %d non-finite", names[k], shape);
                CHECK(stuck < n / 2, "%s carrier %d stuck at full scale", names[k], shape);
                /* A vocoder follows the modulator's band energy, and a 3 Hz
                 * modulator has none: silence is correct there (section 4
                 * proves the vocoder with a real modulator). */
                if (modes[k] != FEATURE_MODE_VOCODER)
                    CHECK(peak > 1e-4, "%s carrier %d silent", names[k], shape);
                destroy(m);
            }
    }

    printf("4. Parasites vocoder, internal saw carrier\n");
    {
        mi_warps::Panel pn;
        pn.level[0] = pn.level[1] = 1.0f;
        pn.algorithm = 0.5f;
        pn.carrier   = 3;
        Modulator* m = make(pn);
        m->set_feature_mode(FEATURE_MODE_VOCODER);
        const size_t n = run(m, 3.f, 440.f, 1.0f, buf, 0.9f, 0.0f);
        double rms = 0;
        for (size_t i = 48000; i < n; ++i) rms += buf[i] * buf[i];
        rms = sqrt(rms / (n - 48000));
        printf("   rms %.3f\n", rms);
        CHECK(rms > 0.05, "vocoder too quiet (rms %.4f)", rms);
        destroy(m);
    }

    printf("5. the firmware's mapping\n");
    {
        warps::Parameters a, b;
        mi_warps::Panel pn;
        pn.level[0] = 0.5f;
        mi_warps::Map(pn, &a);
        pn.voct = 1.0f;
        mi_warps::Map(pn, &b);
        printf("   note at LEVEL 1 noon: %.2f; +1 V on J5: %.2f\n", a.note, b.note);
        CHECK(fabsf(a.note - 66.0f) < 1e-4f, "noon note should be 60*0.5+36 = 66 (got %.3f)", a.note);
        CHECK(fabsf(b.note - a.note - 12.0f) < 1e-4f, "1 V should add 12 semitones");

        pn = mi_warps::Panel();
        pn.algorithm = 1.0f;
        mi_warps::Map(pn, &a);
        printf("   ALGORITHM at 1.0 -> modulation_algorithm %.4f (raw %.4f)\n", a.modulation_algorithm, a.raw_algorithm);
        CHECK(a.modulation_algorithm <= 1.0f && a.modulation_algorithm > 0.9f, "algorithm end out of range");

        /* Pitch actually produced: meta crossfade fully to the internal sine
         * carrier (algorithm 0, timbre 0 = carrier only), no modulator. */
        for (float v : {0.0f, 1.0f})
        {
            mi_warps::Panel q;
            q.algorithm = 0.0f;
            q.timbre    = 0.0f;
            q.carrier   = 1;       /* internal sine */
            q.level[0]  = 0.5f;    /* note 66 = 369.99 Hz */
            q.level[1]  = 0.0f;
            q.voct      = v;
            Modulator* m = make(q);
            m->set_feature_mode(FEATURE_MODE_META);
            const size_t n = run(m, 0.f, 0.f, 1.0f, buf, 0.0f, 0.0f);
            /* zero-crossing frequency over the last half second */
            int zc = 0;
            for (size_t i = n / 2 + 1; i < n; ++i) if (buf[i - 1] <= 0.f && buf[i] > 0.f) zc++;
            const double hz   = zc / ((n - n / 2) / (double)kSampleRate);
            const double want = 440.0 * pow(2.0, (66.0 + 12.0 * v - 69.0) / 12.0);
            printf("   internal carrier, J5 %.0f V: %.1f Hz (want %.1f)\n", v, hz, want);
            CHECK(fabs(hz / want - 1.0) < 0.03, "internal carrier off pitch (%.1f vs %.1f Hz)", hz, want);
            destroy(m);
        }
    }

    if (failures) { printf("warps: %d FAILURE(S)\n", failures); return 1; }
    printf("warps: all checks passed\n");
    return 0;
}
