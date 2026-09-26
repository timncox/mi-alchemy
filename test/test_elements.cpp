/*
 * Native test for the Elements port.
 *
 *   1. The samples file: the tool's output read back through es::Loader
 *      (the firmware's own reader) in awkward chunk sizes, byte-identical
 *      to the vendored arrays; and every failure mode refused.
 *   2. The engine at 32 kHz / 16-frame blocks, as the firmware runs it:
 *      strike, bow and blow a note on each resonator model (and the
 *      Ominous voice) for ~2 s -- finite, not silent, and on the string
 *      model the pitch lands on the note asked for.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "elements/resources.cc"   /* the original arrays, with sizes */
#include "elements/dsp/part.h"
#include "elements_samples.h"

static_assert(sizeof(elements::smp_sample_data) / 2 == es::kSampleCount, "count");
static_assert(sizeof(elements::smp_noise_sample) / 2 == es::kNoiseCount, "count");

static int g_fail = 0;
#define CHECK(c, ...)                                             \
    do {                                                          \
        if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
                    printf(__VA_ARGS__); printf("\n"); g_fail++; } \
    } while (0)

static std::vector<uint8_t> read_file(const char* path)
{
    std::vector<uint8_t> v;
    FILE*                f = fopen(path, "rb");
    if (!f) return v;
    uint8_t buf[65536];
    size_t  n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) v.insert(v.end(), buf, buf + n);
    fclose(f);
    return v;
}

static es::Status load(const std::vector<uint8_t>& file, size_t chunk,
                       std::vector<int16_t>& d, std::vector<int16_t>& n)
{
    d.assign(es::kSampleCount, 0x5555);
    n.assign(es::kNoiseCount, 0x5555);
    es::Loader ld;
    ld.Begin(d.data(), n.data());
    for (size_t at = 0; at < file.size(); at += chunk)
    {
        const size_t len = file.size() - at < chunk ? file.size() - at : chunk;
        if (!ld.Feed(file.data() + at, len)) break;
    }
    return ld.Finish();
}

static void test_file(const char* path)
{
    const std::vector<uint8_t> file = read_file(path);
    CHECK(file.size() == es::kFileBytes, "file is %zu bytes, want %zu", file.size(), es::kFileBytes);

    std::vector<int16_t> d, n;
    /* 4096 is the firmware's read; 1, 7 and 23 put the header and the
     * array boundary across chunks. */
    const size_t chunks[] = {4096, 1, 7, 23, 1u << 20};
    for (size_t c : chunks)
    {
        const es::Status st = load(file, c, d, n);
        CHECK(st == es::Status::Ok, "chunk %zu: %s", c, es::StatusName(st));
        CHECK(memcmp(d.data(), elements::smp_sample_data, sizeof elements::smp_sample_data) == 0,
              "chunk %zu: sample data differs", c);
        CHECK(memcmp(n.data(), elements::smp_noise_sample, sizeof elements::smp_noise_sample) == 0,
              "chunk %zu: noise differs", c);
    }

    std::vector<uint8_t> bad = file;
    bad[es::kHeaderBytes + 100000] ^= 0x01;
    CHECK(load(bad, 4096, d, n) == es::Status::BadCrc, "flipped bit not caught");
    bad = file;
    bad[0] = 'X';
    CHECK(load(bad, 4096, d, n) == es::Status::BadHeader, "bad magic not caught");
    bad = file;
    bad[8] ^= 1;   /* sample count */
    CHECK(load(bad, 4096, d, n) == es::Status::BadHeader, "bad count not caught");
    bad.assign(file.begin(), file.end() - 1);
    CHECK(load(bad, 4096, d, n) == es::Status::Short, "truncation not caught");
    bad.assign(file.begin(), file.begin() + 10);
    CHECK(load(bad, 4096, d, n) == es::Status::Short, "short header not caught");
    bad = file;
    bad.push_back(0);
    CHECK(load(bad, 4096, d, n) == es::Status::BadHeader, "trailing bytes not caught");
    bad.clear();
    CHECK(load(bad, 4096, d, n) == es::Status::Missing, "empty file not Missing");
    printf("samples file: %zu bytes, round trip ok in 5 chunkings, 6 failure modes refused\n",
           file.size());
}

/* ---- engine --------------------------------------------------------------- */

static const size_t kBlock = elements::kMaxBlockSize;
static const float  kSr    = 32000.0f;

struct Run
{
    float rms = 0.f, peak = 0.f;
    bool  finite = true;
    std::vector<float> tail;   /* main out after the attack, for pitch */
};

enum Excite { kStrike, kBow, kBlow };

static uint16_t g_reverb[32768];

static Run play(int model, bool ominous, Excite ex, float note, float seconds,
                float geometry = 0.4f)
{
    static elements::Part part;
    memset(static_cast<void*>(&part), 0, sizeof part);
    memset(g_reverb, 0, sizeof g_reverb);
    part.Init(g_reverb);
    uint32_t seed[3] = {0x12345678u, 0x9abcdef0u, 0x0badf00du};
    part.Seed(seed, 3);
    part.set_easter_egg(ominous);
    part.set_resonator_model(static_cast<elements::ResonatorModel>(model));

    elements::Patch* p = part.mutable_patch();
    p->exciter_envelope_shape = 0.3f;
    p->exciter_bow_level      = ex == kBow ? 0.8f : 0.f;
    p->exciter_bow_timbre     = 0.5f;
    p->exciter_blow_level     = ex == kBlow ? 0.8f : 0.f;
    p->exciter_blow_meta      = 0.5f;
    p->exciter_blow_timbre    = 0.5f;
    p->exciter_strike_level   = ex == kStrike ? 0.8f : 0.f;
    p->exciter_strike_meta    = 0.5f;   /* the samples zone of MALLET */
    p->exciter_strike_timbre  = 0.5f;
    p->resonator_geometry     = geometry;
    p->resonator_brightness   = 0.5f;
    p->resonator_damping      = 0.6f;
    p->resonator_position     = 0.3f;
    p->space                  = 0.0f;   /* dry: no reverb in the pitch window */

    float silence[kBlock] = {0}, main[kBlock], aux[kBlock];
    Run   r;
    const size_t blocks = (size_t)(seconds * kSr / kBlock);
    double       acc    = 0.0;
    for (size_t b = 0; b < blocks; b++)
    {
        elements::PerformanceState s;
        s.note       = note;
        s.modulation = 0.f;
        s.strength   = 0.5f;
        /* Strike: a 50 ms gate. Bow / blow: held throughout. */
        s.gate = ex == kStrike ? (b * kBlock < 1600) : true;
        part.Process(s, silence, silence, main, aux, kBlock);
        for (size_t i = 0; i < kBlock; i++)
        {
            if (!std::isfinite(main[i]) || !std::isfinite(aux[i])) r.finite = false;
            const float a = fabsf(main[i]);
            if (a > r.peak) r.peak = a;
            acc += (double)main[i] * main[i];
            if (b * kBlock >= 3200 && r.tail.size() < 8192) r.tail.push_back(main[i]);
        }
    }
    r.rms = (float)sqrt(acc / (double)(blocks * kBlock));
    return r;
}

/* Autocorrelation f0 over 60..1000 Hz, parabolic peak. */
static float pitch_of(const std::vector<float>& x)
{
    const int lo = (int)(kSr / 1000.f), hi = (int)(kSr / 60.f);
    const int n  = (int)x.size() - hi - 2;
    if (n < 1024) return 0.f;
    std::vector<double> c(hi + 2, 0.0);
    for (int lag = lo - 1; lag <= hi + 1; lag++)
        for (int i = 0; i < n; i++) c[lag] += (double)x[i] * x[i + lag];
    int best = lo;
    for (int lag = lo; lag <= hi; lag++)
        if (c[lag] > c[best]) best = lag;
    const double a = c[best - 1], b = c[best], d = c[best + 1];
    const double den = a - 2 * b + d;
    const double off = den != 0.0 ? 0.5 * (a - d) / den : 0.0;
    return (float)(kSr / (best + off));
}

static void test_engine()
{
    struct Case { const char* name; int model; bool om; Excite ex; };
    const Case cases[] = {
        {"modal strike",   0, false, kStrike}, {"modal bow",   0, false, kBow},
        {"modal blow",     0, false, kBlow},   {"string strike", 1, false, kStrike},
        {"strings strike", 2, false, kStrike}, {"ominous blow", 0, true, kBlow},
    };
    for (const Case& c : cases)
    {
        const Run r = play(c.model, c.om, c.ex, 57.0f, 2.0f);
        CHECK(r.finite, "%s: non-finite output", c.name);
        CHECK(r.rms > 1e-3f, "%s: silent (rms %g)", c.name, r.rms);
        CHECK(r.peak <= 1.0f + 1e-6f, "%s: peak %g past the soft limit", c.name, r.peak);
        printf("%-15s rms %.4f  peak %.3f\n", c.name, r.rms, r.peak);
    }

    /* Pitch: the single-string model is harmonic when GEOMETRY (its
     * dispersion, string.h) sits in the 0.24-0.26 dead zone -- elsewhere
     * the stiffness stretches it sharp, as upstream (0.4 reads ~+130
     * cents). Note 57 = A3 = 220 Hz, and an octave up for 2:1 tracking. */
    const float notes[] = {57.0f, 69.0f};
    for (float nt : notes)
    {
        const Run   r    = play(1, false, kStrike, nt, 0.6f, 0.25f);
        const float want = 440.0f * powf(2.0f, (nt - 69.0f) / 12.0f);
        const float got  = pitch_of(r.tail);
        const float cents = 1200.0f * log2f(got / want);
        CHECK(fabsf(cents) < 15.0f, "string note %.0f: %.2f Hz, want %.2f (%.1f cents)",
              nt, got, want, cents);
        printf("string note %.0f: %.2f Hz (want %.2f, %+.1f cents)\n", nt, got, want, cents);
    }
}

int main(int argc, char** argv)
{
    if (argc != 2) { fprintf(stderr, "usage: %s elements.smp\n", argv[0]); return 2; }
    test_file(argv[1]);
    test_engine();
    if (g_fail) { printf("test-elements: %d FAILED\n", g_fail); return 1; }
    printf("test-elements: ok\n");
    return 0;
}
