// Native test for the Marbles port: runs src/marbles/marbles_engine.h (the
// exact per-block code the firmware runs) over the vendored generators at
// 32 kHz in 16-frame blocks, and checks what a patch would hear.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>

#include "../src/marbles/marbles_engine.h"

using namespace marbles_port;

static const float kSr = 32000.0f;
static int g_fail = 0;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        if (!(cond)) { printf("  FAIL: " __VA_ARGS__); printf("\n"); g_fail++; } \
    } while (0)

struct Run
{
    std::vector<uint8_t> t;     // bit k = T(k+1), per sample
    std::vector<float>   x[4];  // X1 X2 X3 Y, per sample
    bool nan = false;
};

// `clock_hz` > 0 feeds 5 ms, 5 V pulses at that rate into J1.
static Run run(Controls& c, float seconds, float clock_hz = 0.f, float cv_volts = 0.f)
{
    Engine e;
    e.Init(kSr);
    Run r;
    const size_t n = (size_t)(seconds * kSr);
    float t_in[kMaxBlock], x_in[kMaxBlock];
    Block b;
    size_t s = 0;
    while (s < n)
    {
        for (size_t i = 0; i < kMaxBlock; i++)
        {
            float v = 0.f;
            if (clock_hz > 0.f)
            {
                const double ph = fmod((double)(s + i) * clock_hz / kSr, 1.0);
                v = ph * (1.0 / clock_hz) < 0.005 ? 1.0f : 0.0f;   // +5 V
            }
            t_in[i] = v;
            x_in[i] = 0.f;
        }
        const uint32_t ms = (uint32_t)((double)s * 1000.0 / kSr);
        e.Process(c, t_in, x_in, cv_volts, ms, kMaxBlock, &b);
        const float* v = e.voltages();
        for (size_t i = 0; i < kMaxBlock; i++)
        {
            r.t.push_back((uint8_t)(b.t[0][i] | (b.t[1][i] << 1) | (b.t[2][i] << 2)));
            for (int j = 0; j < 4; j++)
            {
                const float x = v[4 * i + j];
                if (!(x == x) || isinf(x)) r.nan = true;
                r.x[j].push_back(x);
            }
        }
        s += kMaxBlock;
    }
    return r;
}

static std::vector<size_t> rising(const Run& r, int bit, size_t from = 0)
{
    std::vector<size_t> e;
    for (size_t i = from + 1; i < r.t.size(); i++)
        if ((r.t[i] >> bit & 1) && !(r.t[i - 1] >> bit & 1)) e.push_back(i);
    return e;
}

static float rate_hz(const Run& r, int bit, size_t skip)
{
    std::vector<size_t> e = rising(r, bit, skip);
    if (e.size() < 3) return 0.f;
    return (float)(e.size() - 1) * kSr / (float)(e.back() - e.front());
}

static Controls steady()
{
    Controls c;
    c.rate = 0.5f;       // 0 semitones -> 2 Hz in the x1 range
    c.t_jitter = 0.f;
    c.t_bias = 0.5f;
    c.t_model = 0;       // coin toss
    c.x_clock = 3;       // X clocked by T2, all three channels
    return c;
}

int main()
{
    printf("marbles: internal clock\n");
    {
        Controls c = steady();
        Run r = run(c, 10.f);
        float f = rate_hz(r, 1, 32000);
        printf("  RATE noon: T2 %.3f Hz (expect 2.000)\n", f);
        CHECK(fabsf(f - 2.0f) < 0.02f, "T2 at noon %.3f Hz", f);
        c.rate = 0.6f;   // +12 semitones
        r = run(c, 10.f);
        f = rate_hz(r, 1, 32000);
        printf("  RATE +12 st: T2 %.3f Hz (expect 4.000)\n", f);
        CHECK(fabsf(f - 4.0f) < 0.04f, "T2 at +12 st %.3f Hz", f);
        c.rate = 0.5f;
        r = run(c, 10.f, 0.f, 1.0f);   // J8 -> RATE, +1 V = +1 octave
        f = rate_hz(r, 1, 32000);
        printf("  J8 +1 V: T2 %.3f Hz (expect 4.000)\n", f);
        CHECK(fabsf(f - 4.0f) < 0.04f, "J8 1 V/oct: %.3f Hz", f);
        // Coin toss: T1 and T3 together follow T2's ticks, each ~half.
        const size_t t1 = rising(r, 0, 32000).size(), t2 = rising(r, 1, 32000).size(),
                     t3 = rising(r, 2, 32000).size();
        printf("  coin toss ticks T1 %zu, T2 %zu, T3 %zu\n", t1, t2, t3);
        CHECK(t1 > 0 && t3 > 0 && t1 + t3 >= t2 * 8 / 10, "coin toss T1/T3 do not follow T2");
        CHECK(!r.nan, "NaN in X");
    }

    printf("marbles: external clock on J1\n");
    {
        Controls c = steady();
        c.rate = 0.5f;   // divider 1/1 at noon
        Run r = run(c, 10.f, 3.0f);
        float f = rate_hz(r, 1, 3 * 32000);
        printf("  3 Hz in, RATE noon: T2 %.3f Hz (expect 3.000)\n", f);
        CHECK(fabsf(f - 3.0f) < 0.05f, "external 1/1: %.3f Hz", f);
        c.rate = 1.0f;   // top of the divider table: x4
        r = run(c, 10.f, 3.0f);
        f = rate_hz(r, 1, 3 * 32000);
        printf("  3 Hz in, RATE max: T2 %.3f Hz (expect 12.000)\n", f);
        CHECK(fabsf(f - 12.0f) < 0.3f, "external x4: %.3f Hz", f);
        c.j1_ignore = true;
        r = run(c, 10.f, 3.0f);
        f = rate_hz(r, 1, 32000);
        printf("  J1 ignored: T2 %.3f Hz (expect 64.000, internal at RATE max)\n", f);
        CHECK(fabsf(f - 64.0f) < 1.0f, "J1 ignore: %.3f Hz", f);
    }

    printf("marbles: X ranges\n");
    {
        const char*  name[3] = {"+2 V", "+5 V", "+/-5 V"};
        const float  lo[3] = {0.f, 0.f, -5.f}, hi[3] = {2.f, 5.f, 5.f};
        for (int rg = 0; rg < 3; rg++)
        {
            Controls c = steady();
            c.rate = 0.7f;           // 8 Hz: many samples
            c.x_range = rg;
            c.x_spread = 1.0f;       // widest
            c.x_steps = 0.5f;
            Run r = run(c, 10.f);
            float mn = 1e9f, mx = -1e9f;
            for (int j = 0; j < 3; j++)
                for (float v : r.x[j]) { mn = fminf(mn, v); mx = fmaxf(mx, v); }
            printf("  %-7s X in [%.3f, %.3f]\n", name[rg], mn, mx);
            CHECK(mn >= lo[rg] - 0.01f && mx <= hi[rg] + 0.01f, "%s out of range", name[rg]);
            CHECK(mx - mn > 0.5f * (hi[rg] - lo[rg]), "%s spread too narrow", name[rg]);
            CHECK(!r.nan, "NaN");
        }
    }

    printf("marbles: deja vu locked repeats after LENGTH steps\n");
    {
        const float lens[3] = {0.1f, 0.5f, 1.0f};
        for (float lk : lens)
        {
            Controls c = steady();
            c.rate = 0.7f;
            c.x_steps = 1.0f;        // quantized: exact equality is meaningful
            c.x_spread = 0.8f;
            c.length = lk;
            c.t_deja_vu = DEJA_VU_LOCKED;
            c.x_deja_vu = DEJA_VU_LOCKED;
            Engine probe; probe.Init(kSr);
            Run r = run(c, 20.f);
            // Length as the engine quantizes it.
            {
                Block b; float z[kMaxBlock] = {0};
                probe.Process(c, z, z, 0.f, 0, kMaxBlock, &b);
            }
            const int L = probe.loop_length();
            // X1 sampled mid-way through each T2 period, after 2 s warm-up.
            std::vector<size_t> e = rising(r, 1, 64000);
            std::vector<float>  xs;
            std::vector<uint8_t> ts;
            for (size_t i = 0; i + 1 < e.size(); i++)
            {
                const size_t mid = (e[i] + e[i + 1]) / 2;
                xs.push_back(r.x[0][mid]);
                ts.push_back(r.t[mid] & 5u);
            }
            int bad_x = 0, bad_t = 0, n = 0;
            for (size_t i = 0; i + L < xs.size(); i++, n++)
            {
                if (fabsf(xs[i] - xs[i + L]) > 1e-4f) bad_x++;
                if (ts[i] != ts[i + L]) bad_t++;
            }
            printf("  LENGTH %d: %d steps compared, X mismatches %d, T mismatches %d\n",
                   L, n, bad_x, bad_t);
            CHECK(n > 20 && bad_x == 0, "X loop of %d does not repeat", L);
            CHECK(bad_t == 0, "T loop of %d does not repeat", L);
            // And it is not trivially constant (spread is wide).
            bool varied = false;
            for (int i = 1; i < L && i < (int)xs.size(); i++)
                if (fabsf(xs[i] - xs[0]) > 1e-3f) varied = true;
            CHECK(L == 1 || varied, "loop of %d is constant", L);
        }
    }

    printf("marbles: quantized X sits on the scale\n");
    {
        for (int sc = 0; sc < 6; sc++)
        {
            Controls c = steady();
            c.rate = 0.7f;
            c.x_steps = 0.8f;       // quantized, most of the scale's notes
            c.x_spread = 0.9f;
            c.x_scale = sc;
            c.x_range = 2;
            Run r = run(c, 8.f);
            const marbles::Scale& s = marbles::kPresetScales[sc];
            int off = 0, n = 0;
            std::vector<size_t> e = rising(r, 1, 32000);
            for (size_t i = 0; i + 1 < e.size(); i++)
            {
                const float v = r.x[0][(e[i] + e[i + 1]) / 2];
                float f = v - floorf(v / s.base_interval) * s.base_interval;
                float best = 1e9f;
                for (int d = 0; d < s.num_degrees; d++)
                {
                    best = fminf(best, fabsf(f - s.degree[d].voltage));
                    best = fminf(best, fabsf(f - s.degree[d].voltage - s.base_interval));
                }
                if (best > 2e-3f) off++;
                n++;
            }
            printf("  scale %d: %d notes, %d off the scale\n", sc, n, off);
            CHECK(n > 20 && off == 0, "scale %d: %d of %d off", sc, off, n);
        }
    }

    printf("marbles: external X processing (J8 sampled)\n");
    {
        Controls c = steady();
        c.rate = 0.7f;
        c.ext_x = true;
        c.x_steps = 0.5f;
        c.x_spread = 0.5f;
        Run r = run(c, 4.f, 0.f, 2.0f);   // J8 at +2 V
        const float v = r.x[0].back();
        printf("  J8 +2 V -> X1 %.3f V\n", v);
        CHECK(fabsf(v - 2.0f) < 0.5f, "external X1 %.3f V for 2 V in", v);
    }

    if (g_fail) { printf("marbles: %d FAILED\n", g_fail); return 1; }
    printf("marbles: all passed\n");
    return 0;
}
