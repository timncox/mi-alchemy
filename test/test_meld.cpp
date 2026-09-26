/*
 * Native test for Meld as the mi-alchemy firmware drives it: 96 kHz,
 * 60-frame blocks, both modulators placement-new'd into zeroed memory, the
 * panel mapped by src/warps/warps_params.h and src/meld/meld_params.h (the
 * firmware's own arithmetic). Stock-Warps parts ported from
 * ~/tim-os/meld/test/test_modulator.cc; the Parasites modes are covered by
 * test/test_warps.cpp and not repeated here.
 *
 *   1. stock digital ring mod: 1000 x 200 Hz -> 800 + 1200 Hz, inputs gone
 *   2. stock big knob swept over nine positions (internal saw): no NaN, the
 *      vocoder region sounds, the freeze from silence stays silent
 *   3. the freeze holds a charged spectrum; release 0.2 decays instead
 *   4. Parasites' vocoder alive on the same terms (the other vocoder)
 *   5. the knob at its stop reaches the freeze through the firmware's own
 *      mapping (meld's README left that unmeasured on the Patch)
 *   6. MapStock copies exactly what Meld's map_params() copied
 *   7. coincidence clock: in-phase inputs fire once per shared period,
 *      anti-phase never, and a drifting pair straddling block edges counts
 *   8. envelopes follow level; CV 1 = 5 V x envelope; CV 2 AUTO / ENVELOPE /
 *      NOTE and its 1 V/oct note scaling from C2
 */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "warps/dsp/modulator.h"
#include "warps_stock/dsp/modulator.h"
#include "meld_params.h"

using namespace warps;

static const float  kSampleRate = 96000.0f;
static const size_t kBlock      = 60;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

static double power_at(const float* x, size_t n, double hz)
{
    const double w = 2.0 * M_PI * hz / kSampleRate, c = 2.0 * cos(w);
    double s1 = 0, s2 = 0;
    for (size_t i = 0; i < n; ++i) { const double s0 = x[i] + c * s1 - s2; s2 = s1; s1 = s0; }
    return (s1 * s1 + s2 * s2 - c * s1 * s2) / (double)n;
}

static void defaults(warps_stock::Parameters* p)
{
    p->channel_drive[0] = p->channel_drive[1] = 0.5f;
    p->modulation_algorithm = 0.0f;
    p->modulation_parameter = 0.0f;
    p->frequency_shift_pot  = 0.5f;
    p->frequency_shift_cv   = 0.0f;
    p->phase_shift          = 0.0f;
    p->note                 = 48.0f;
    p->carrier_shape        = 0;
}

/* Zeroed memory first: Init() leaves some previous_parameters_ fields to
 * the zeroed statics Warps had (meld's lesson; the firmware memsets D2). */
template <class Mod> static Mod* make()
{
    void* mem = calloc(1, sizeof(Mod));
    Mod*  m   = new (mem) Mod();
    m->Init(kSampleRate);
    return m;
}
template <class Mod> static void destroy(Mod* m) { m->~Mod(); free(m); }

static void process(Modulator* m, ShortFrame* in, ShortFrame* out, size_t n) { m->Process(in, out, n); }
static void process(warps_stock::Modulator* m, ShortFrame* in, ShortFrame* out, size_t n)
{
    m->Process(reinterpret_cast<warps_stock::ShortFrame*>(in),
               reinterpret_cast<warps_stock::ShortFrame*>(out), n);
}

/* `seconds` of two sines (L = J1 carrier, R = J2 modulator); keeps main. */
template <class Mod>
static size_t run(Mod* m, float f1, float f2, float seconds, float* out, float amp_r = 0.5f,
                  float amp_l = -1.f)
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
        process(m, in, o, kBlock);
        for (size_t i = 0; i < kBlock; ++i) out[n + i] = o[i].l / 32768.0f;
        n += kBlock;
    }
    return n;
}

static double rms_of(const float* x, size_t a, size_t b)
{
    double s = 0;
    for (size_t i = a; i < b; ++i) s += x[i] * x[i];
    return sqrt(s / (double)(b - a));
}

/* Feed the extras `seconds` of two sines, block by block; returns fires. */
static uint32_t coincide(float f1, float ph1, float f2, float ph2, float amp1, float amp2,
                         float seconds, mi_meld::Extras* e)
{
    float x[kBlock], y[kBlock];
    const size_t total = (size_t)(seconds * kSampleRate);
    double a = ph1, b = ph2;
    e->Init(kSampleRate);
    for (size_t n = 0; n + kBlock <= total; n += kBlock)
    {
        for (size_t i = 0; i < kBlock; ++i)
        {
            x[i] = amp1 * (float)sin(a);
            y[i] = amp2 * (float)sin(b);
            a += 2.0 * M_PI * f1 / kSampleRate;
            b += 2.0 * M_PI * f2 / kSampleRate;
        }
        e->Process(x, y, kBlock);
    }
    return e->fires;
}

int main()
{
    printf("sizeof: warps::Modulator %zu B, warps_stock::Modulator %zu B\n",
           sizeof(Modulator), sizeof(warps_stock::Modulator));
    static float buf[96000 * 2];

    printf("1. stock digital ring mod\n");
    {
        warps_stock::Modulator* m = make<warps_stock::Modulator>();
        defaults(m->mutable_parameters());
        m->mutable_parameters()->modulation_algorithm = 3.0f / 8.0f;
        const size_t n = run(m, 1000.0f, 200.0f, 1.5f, buf);
        const float* x = buf + 48000;
        const size_t len = n - 48000;
        const double side = power_at(x, len, 800) + power_at(x, len, 1200);
        const double ins  = power_at(x, len, 200) + power_at(x, len, 1000);
        printf("   sidebands %.3g, inputs %.3g\n", side, ins);
        CHECK(side > 1e-4, "stock ring mod sidebands missing (%.3g)", side);
        CHECK(side > 20.0 * ins, "stock inputs not suppressed: %.3g vs %.3g", side, ins);
        destroy(m);
    }

    printf("2. stock big-knob sweep, internal saw, hot modulator\n");
    for (int pos = 0; pos <= 8; ++pos)
    {
        warps_stock::Modulator* s = make<warps_stock::Modulator>();
        warps_stock::Parameters* q = s->mutable_parameters();
        defaults(q);
        q->carrier_shape = 3;
        q->channel_drive[0] = q->channel_drive[1] = 1.0f;
        q->modulation_algorithm = pos / 8.0f;
        q->modulation_parameter = 0.5f;
        const size_t n = run(s, 3.0f, 440.0f, 1.0f, buf, 0.9f, 0.0f);
        size_t rail = 0;
        for (size_t i = 48000; i < n; ++i) if (buf[i] <= -0.999f) ++rail;
        const double rms = rms_of(buf, 48000, n);
        printf("   position %d: rms %.3f, rail samples %zu\n", pos, rms, rail);
        CHECK(rail < (n - 48000) * 9 / 10, "position %d pinned to the rail (NaN?)", pos);
        if (pos == 7) CHECK(rms > 0.05, "vocoder region too quiet at 7 (rms %.4f)", rms);
        if (pos == 8) CHECK(rms < 1e-3, "freeze from silence should be silent (rms %.4f)", rms);
        destroy(s);
    }

    printf("3. the spectral freeze holds\n");
    {
        double held[2] = {0, 0};
        for (int frozen = 0; frozen < 2; ++frozen)
        {
            warps_stock::Modulator* s = make<warps_stock::Modulator>();
            warps_stock::Parameters* q = s->mutable_parameters();
            defaults(q);
            q->carrier_shape = 3;
            q->channel_drive[0] = q->channel_drive[1] = 1.0f;
            q->modulation_parameter = 0.5f;
            q->modulation_algorithm = 7.0f / 8.0f;
            run(s, 3.0f, 440.0f, 0.5f, buf, 0.9f, 0.0f);
            q->modulation_algorithm = frozen ? 1.0f : 0.8f;
            const size_t n = run(s, 3.0f, 440.0f, 1.0f, buf, 0.0f, 0.0f);
            held[frozen] = rms_of(buf, n * 3 / 4, n);
            printf("   modulator removed at %s: rms %.4f\n", frozen ? "1.0 (freeze)" : "0.8", held[frozen]);
            destroy(s);
        }
        CHECK(held[1] > 0.05, "freeze did not hold the spectrum (rms %.4f)", held[1]);
        CHECK(held[0] < 0.01, "0.8 did not decay (rms %.4f)", held[0]);
    }

    printf("4. Parasites' vocoder mode\n");
    {
        Modulator* m = make<Modulator>();
        m->set_feature_mode(FEATURE_MODE_VOCODER);
        mi_warps::Panel panel;
        panel.level[0] = panel.level[1] = 1.0f;
        panel.algorithm = 0.5f;
        panel.timbre    = 0.5f;
        panel.carrier   = 3;
        mi_warps::Map(panel, m->mutable_parameters());
        const size_t n = run(m, 3.0f, 440.0f, 1.0f, buf, 0.9f, 0.0f);
        const double rms = rms_of(buf, 48000, n);
        printf("   rms %.3f\n", rms);
        CHECK(rms > 0.05, "parasites vocoder too quiet (rms %.4f)", rms);
        destroy(m);
    }

    printf("5. the ALGORITHM pot at its stop reaches the freeze\n");
    {
        mi_warps::Panel panel;
        panel.algorithm = 1.0f;
        warps::Parameters p;
        memset(&p, 0, sizeof p);
        mi_warps::Map(panel, &p);
        warps_stock::Parameters s;
        mi_meld::MapStock(p, &s);
        printf("   pot 1.0 -> modulation_algorithm %.5f (freeze above 0.995)\n", s.modulation_algorithm);
        CHECK(s.modulation_algorithm > 0.995f, "the pot's stop does not reach the freeze (%.5f)",
              s.modulation_algorithm);
        panel.algorithm = 0.97f;
        mi_warps::Map(panel, &p);
        mi_meld::MapStock(p, &s);
        printf("   pot 0.97 -> %.5f\n", s.modulation_algorithm);
        CHECK(s.modulation_algorithm < 0.995f, "the freeze is not confined to the knob's end (%.5f)",
              s.modulation_algorithm);
    }

    printf("6. MapStock carries Meld's fields\n");
    {
        mi_warps::Panel panel;
        panel.level[0] = 0.6f; panel.level[1] = 0.3f;
        panel.algorithm = 0.4f; panel.timbre = 0.7f; panel.carrier = 2; panel.voct = 1.0f;
        warps::Parameters p;
        memset(&p, 0, sizeof p);
        mi_warps::Map(panel, &p);
        warps_stock::Parameters s;
        memset(&s, 0x55, sizeof s);
        mi_meld::MapStock(p, &s);
        CHECK(s.channel_drive[0] == p.channel_drive[0] && s.channel_drive[1] == p.channel_drive[1],
              "drive not copied");
        CHECK(s.modulation_algorithm == p.modulation_algorithm
              && s.modulation_parameter == p.modulation_parameter, "algorithm/parameter not copied");
        CHECK(s.note == p.note && s.carrier_shape == 2, "note/carrier not copied");
        CHECK(s.frequency_shift_pot == 0.5f && s.frequency_shift_cv == 0.0f && s.phase_shift == 0.0f,
              "easter-egg fields not neutral");
        printf("   note %.2f (60*0.6+36 + 12 for +1 V = 84)\n", s.note);
        CHECK(fabsf(s.note - 84.0f) < 1e-3f, "note mapping moved (%.3f)", s.note);
    }

    printf("7. coincidence clock\n");
    {
        mi_meld::Extras e;
        const uint32_t same = coincide(190.0f, 0.0f, 190.0f, 0.0f, 0.5f, 0.5f, 2.0f, &e);
        const uint32_t anti = coincide(190.0f, 0.0f, 190.0f, (float)M_PI, 0.5f, 0.5f, 2.0f, &e);
        const uint32_t oct  = coincide(190.0f, 0.0f, 380.0f, 0.0f, 0.5f, 0.5f, 2.0f, &e);
        const uint32_t odd  = coincide(190.0f, 0.0f, 263.0f, 0.3f, 0.5f, 0.5f, 2.0f, &e);
        printf("   2 s: in phase %u (want ~380), anti-phase %u, octave %u (want ~380), 190 vs 263 Hz %u\n",
               same, anti, oct, odd);
        /* 190 Hz is not a divisor of 96 kHz / 60, so the crossings walk
         * across block edges: counting all of them proves the straddle. */
        CHECK(same >= 376 && same <= 381, "in-phase pair should fire every shared cycle (%u)", same);
        CHECK(anti == 0, "anti-phase pair should never coincide (%u)", anti);
        CHECK(oct >= 376 && oct <= 381, "octave pair should fire once per low cycle (%u)", oct);
        CHECK(odd < same / 4, "unrelated pair fires too often (%u)", odd);
    }

    printf("8. envelopes and the CV outputs\n");
    {
        mi_meld::Extras e;
        coincide(200.0f, 0.0f, 300.0f, 0.0f, 0.5f, 0.1f, 1.0f, &e);
        const float cv1 = 5.0f * e.env[1];
        printf("   carrier 0.5 -> env %.3f, modulator 0.1 -> env %.3f (CV 1 %.2f V)\n",
               e.env[0], e.env[1], cv1);
        CHECK(e.env[0] > 0.40f && e.env[0] < 0.52f, "carrier envelope off (%.3f)", e.env[0]);
        CHECK(e.env[1] > 0.08f && e.env[1] < 0.105f, "modulator envelope off (%.3f)", e.env[1]);
        CHECK(fabsf(e.env[0] / e.env[1] - 5.0f) < 0.5f, "envelopes not proportional to level");

        /* silence: the 50 ms release takes it down */
        float z[kBlock] = {0};
        for (int b = 0; b < (int)(0.5f * kSampleRate / kBlock); ++b) e.Process(z, z, kBlock);
        CHECK(e.env[0] < 0.001f, "envelope did not release (%.4f)", e.env[0]);

        using namespace mi_meld;
        CHECK(Cv2Volts(CV2_AUTO, 0, 0.4f, 60.f) == 2.0f, "AUTO, external carrier: envelope");
        CHECK(fabsf(Cv2Volts(CV2_AUTO, 1, 0.4f, 60.f) - 2.0f) < 1e-5f, "AUTO, internal: note 60 = 2 V");
        CHECK(fabsf(Cv2Volts(CV2_NOTE, 0, 0.4f, 48.f) - 1.0f) < 1e-5f, "NOTE: note 48 = 1 V");
        CHECK(Cv2Volts(CV2_ENVELOPE, 3, 0.2f, 90.f) == 1.0f, "ENVELOPE ignores the carrier");
        CHECK(Cv2Volts(CV2_NOTE, 0, 0.f, 20.f) == 0.0f && Cv2Volts(CV2_NOTE, 0, 0.f, 120.f) == 5.0f,
              "note CV not clamped to 0..5 V");
        printf("   CV 2: AUTO/ENVELOPE/NOTE and 1 V/oct from C2 as specified\n");
    }

    if (failures)
    {
        printf("meld: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("meld: all checks passed\n");
    return 0;
}
