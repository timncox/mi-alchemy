/*
 * Parasites' edge cases (vendor/VENDOR.md changes 4 and 5), on their own
 * binary: it needs ASan's global-array redzones, which test/Makefile turns
 * off for the other suites, and a fatal UBSan float->int check, because
 * Process1 rounds its float output to int and a NaN would otherwise just
 * read back as 0. Found with the emulator's SAN=address,undefined build of
 * warps_alchemy; here Process1<> is driven straight.
 *
 *   1. FOLD, full-scale in-phase inputs, algorithm and timbre at 1.0: the
 *      index ran up to 155 floats past the 4097-entry lut_bipolar_fold
 *      (Parasites adds p_2 after the scaling; stock Warps has no offset).
 *   2. CHEBYSCHEV with TIMBRE at 0: x / p_2 with p_2 = 0 gave NaN / inf.
 *   3. COMPARATOR_CHEBYSCHEV with silence in: 0.9 / envelope_ with the
 *      envelope at 0 gave inf, then 0 * inf = NaN.
 */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>

#include "warps/dsp/modulator.h"

using namespace warps;

static const float  kSampleRate = 96000.0f;
static const size_t kBlock      = 60;
static int          failures    = 0;

/* One second through Process1<A>; amp = 0 is silence. Square waves, in
 * phase on both inputs so they add: x_1 = x_2 = +-amp. */
template <XmodAlgorithm A>
static double run(const char* name, float algorithm, float timbre, float amp)
{
    void*      mem = calloc(1, sizeof(Modulator));
    Modulator* m   = new (mem) Modulator();
    m->Init(kSampleRate);
    Parameters* p = m->mutable_parameters();
    p->channel_drive[0]     = 1.0f;
    p->channel_drive[1]     = 1.0f;
    p->modulation_algorithm = algorithm;   /* p_1 in Process1; skews p_2 */
    p->modulation_parameter = timbre;
    p->carrier_shape        = 0;

    ShortFrame in[kBlock], out[kBlock];
    const short hi_v = (short)(amp * 32767.0f), lo_v = (short)(-amp * 32767.0f);
    double sq = 0.0;
    size_t n = 0;
    for (int b = 0; b < 1600; ++b)
    {
        for (size_t i = 0; i < kBlock; ++i)
        {
            const bool hi = ((b * kBlock + i) / 48) & 1;
            in[i].l = in[i].r = hi ? hi_v : lo_v;
        }
        m->Process1<A>(in, out, kBlock);
        for (size_t i = 0; i < kBlock; ++i)
        {
            const double v = out[i].l / 32768.0;
            sq += v * v;
            n++;
        }
    }
    m->~Modulator();
    free(mem);
    const double rms = std::sqrt(sq / (double)n);
    printf("   %-34s rms %.3f\n", name, rms);
    return rms;
}

int main()
{
    if (!(run<ALGORITHM_FOLD>("FOLD, full scale, all up", 1.0f, 1.0f, 1.0f) > 0.01))
    {
        printf("  FAIL: FOLD silent at full scale\n");
        failures++;
    }
    run<ALGORITHM_CHEBYSCHEV>("CHEBYSCHEV, timbre 0", 1.0f, 0.0f, 0.5f);
    run<ALGORITHM_CHEBYSCHEV>("CHEBYSCHEV, timbre 0.5", 1.0f, 0.5f, 0.5f);
    run<ALGORITHM_COMPARATOR_CHEBYSCHEV>("COMPARATOR_CHEBYSCHEV, silence", 0.5f, 0.5f, 0.0f);
    run<ALGORITHM_COMPARATOR_CHEBYSCHEV>("COMPARATOR_CHEBYSCHEV, timbre 0", 0.5f, 0.0f, 0.5f);
    /* the NaN cases have no assertion of their own: a NaN reaching
     * Process1's float->int conversion aborts the run (warps_bounds.mk) */
    if (failures) { printf("test_warps_bounds: %d failure(s)\n", failures); return 1; }
    printf("test_warps_bounds: ok\n");
    return 0;
}
