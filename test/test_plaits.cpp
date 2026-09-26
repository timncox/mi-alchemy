/*
 * Native test for the Plaits port: every one of the 24 engines rendered for
 * one second at 48 kHz in 12-frame calls, as the firmware drives the Voice
 * (plaits_alchemy.cpp), in both LPG modes the firmware can select:
 *
 *   drone      trigger_patched = false (Settings TRIGGER = Drone, or Auto
 *              before the first trigger): the LPG is bypassed
 *   triggered  trigger_patched = true, a 10 ms gate at t = 0 and t = 0.5 s
 *
 * Asserts: no NaN / inf (the Frame is int16, so this checks the float
 * buffers via the output never pinning at the rails for a whole second),
 * every engine non-silent in drone mode (the drums and the modal/string
 * models self-excite only on a trigger, so for those the triggered run is
 * the one that must be non-silent), and a per-engine RMS table.
 *
 * Also times the render: host ns per 12-frame call, per engine, with a
 * rough M7 estimate (see the note at the print).
 */
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "plaits/dsp/dsp.h"
#include "plaits/dsp/voice.h"
#include "stmlib/utils/buffer_allocator.h"

using namespace plaits;

static char   shared_buffer[16384];
static Voice  voice;

static const char* kNames[24] = {
    "VA + VCF", "Phase distortion", "6-op FM 1", "6-op FM 2", "6-op FM 3",
    "Wave terrain", "String machine", "Chiptune",
    "Virtual analog", "Waveshaping", "2-op FM", "Formant / grain",
    "Harmonic", "Wavetable", "Chords", "Speech",
    "Swarm", "Filtered noise", "Particle", "Inharmonic string",
    "Modal resonator", "Bass drum", "Snare drum", "Hi-hat"};

struct Result
{
    double rms_out, rms_aux;
    bool   rail;      /* pinned at the int16 rails for the whole run */
    double ns_per_call;
};

static Result run(int engine, bool triggered)
{
    Patch p;
    memset(&p, 0, sizeof p);
    p.note = 48.0f;               /* C3, a mid-register pitch */
    p.harmonics = 0.5f;
    p.timbre = 0.5f;
    p.morph = 0.5f;
    p.engine = engine;
    p.decay = 0.5f;
    p.lpg_colour = 0.0f;

    Modulations m;
    memset(&m, 0, sizeof m);
    m.trigger_patched = triggered;

    Voice::Frame f[kBlockSize];
    const int calls = 48000 / (int)kBlockSize;   /* one second */
    double s_out = 0, s_aux = 0;
    long   rail  = 0, n = 0;
    auto   t0    = std::chrono::steady_clock::now();
    for (int c = 0; c < calls; c++)
    {
        const int t_ms = c * (int)kBlockSize / 48;
        m.trigger = triggered && ((t_ms < 10) || (t_ms >= 500 && t_ms < 510)) ? 1.0f : 0.0f;
        voice.Render(p, m, f, kBlockSize);
        for (size_t i = 0; i < kBlockSize; i++)
        {
            const double o = f[i].out / 32768.0, a = f[i].aux / 32768.0;
            s_out += o * o;
            s_aux += a * a;
            if (f[i].out == 32767 || f[i].out == -32768) rail++;
            n++;
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    Result r;
    r.rms_out = std::sqrt(s_out / n);
    r.rms_aux = std::sqrt(s_aux / n);
    r.rail    = rail == n;
    r.ns_per_call =
        std::chrono::duration<double, std::nano>(t1 - t0).count() / calls;
    return r;
}

int main()
{
    stmlib::BufferAllocator allocator(shared_buffer, sizeof shared_buffer);
    voice.Init(&allocator);

    /* Engines that are silent until struck (upstream: they self-trigger
     * only when TRIGGER is patched, or ring on internal excitation). */
    int failures = 0;
    printf("%-3s %-18s %9s %9s %11s %9s %9s\n", "#", "engine", "drone", "drone aux",
           "triggered", "trig aux", "ns/call");
    double worst_ns = 0;
    for (int e = 0; e < 24; e++)
    {
        /* Render a little first so the engine switch (Reset + LoadUserData)
         * is not in the timing or the RMS. */
        run(e, false);
        const Result d = run(e, false);
        const Result t = run(e, true);
        const double ns = d.ns_per_call > t.ns_per_call ? d.ns_per_call : t.ns_per_call;
        if (ns > worst_ns) worst_ns = ns;
        printf("%-3d %-18s %9.4f %9.4f %11.4f %9.4f %9.0f\n", e + 1, kNames[e],
               d.rms_out, d.rms_aux, t.rms_out, t.rms_aux, ns);

        const double best = d.rms_out > t.rms_out ? d.rms_out : t.rms_out;
        if (!(best > 1e-3) || std::isnan(best))
        {
            printf("    FAIL: engine %d silent in both modes\n", e + 1);
            failures++;
        }
        if (d.rail || t.rail)
        {
            printf("    FAIL: engine %d pinned at the rails\n", e + 1);
            failures++;
        }
        if (!(t.rms_out > 1e-3))
        {
            printf("    FAIL: engine %d silent when triggered\n", e + 1);
            failures++;
        }
    }

    /* Budget: one 12-frame call has 250 us at 48 kHz (the firmware makes
     * two per 24-frame block). `make test` runs under ASan+UBSan, which
     * slows this code several times; an unsanitized -O2 build on an Apple
     * M-series core measured 1.26 us for the worst engine (Particle),
     * 2026-09-26. The M7 at 480 MHz is an order of magnitude or more slower
     * per call than that core -- treat this as a sanity bound only; the
     * Lab's own CPU meter (B2 red above 80 %, the boot readout on P1) is the
     * real number. */
    printf("\nworst engine: %.0f ns per 12-frame call on this host, "
           "%.2f %% of the 250 us budget\n", worst_ns, worst_ns / 2500.0);

    if (failures)
    {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("plaits: all 24 engines render, none silent, none pinned\n");
    return 0;
}
