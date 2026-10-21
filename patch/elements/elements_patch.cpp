/*
 * Elements on the Daisy Patch -- Mutable Instruments' modal synthesis voice
 * (bow / blow / strike exciters into a modal or string resonator, with its
 * reverb), loadable from the Patch's SD-card module picker.
 *
 * The DSP is elements::Part, vendored byte-identical from pichenettes/
 * eurorack under ../../vendor/eurorack (VENDOR.md), run at its native
 * 32 kHz in its native 16-frame blocks: the Patch codecs are set to 32 kHz,
 * so nothing is resampled and the 2 kHz control rate of cv_scaler.cc holds.
 * Unlike the Alchemy Lab build, the two exciter sample tables stay in the
 * image (the Patch has no SDK eating the 480 KB SRAM), so there is no card
 * file to copy.
 *
 * Panel (each knob is summed with its CV jack before the ADC):
 *
 *   CTRL 1   PITCH -- five octaves, and the V/oct input. Never paged: a
 *            param that takes CV owns its knob.
 *   CTRL 2-4 the three params of the selected PAGE, with pickup:
 *              RESON  geometry . brightness . damping
 *              SPACE  position . space . contour
 *              LEVEL  bow . blow . strike           (the exciter mix)
 *              META   flow . mallet . fine
 *              TIMBR  bow timbre . blow timbre . strike timbre
 *   GATE IN 1   the gate (Elements' PLAY button / GATE jack)
 *   AUDIO IN 1  external exciter into the BLOW path (Elements' BLOW IN)
 *   AUDIO IN 2  external exciter into the STRIKE path (STRIKE IN)
 *   OUT 1 / 2   main / aux (the stereo pair from the reverb); 3 / 4 the same
 *   CV OUT 1    exciter level, 0..5 V;  CV OUT 2  resonator level, 0..5 V
 *   GATE OUT    the gate as the engine sees it (one block late, as upstream)
 *   Encoder     click to select a row ('*'), turn to change, click to let
 *               go; rows Page / Model / Trans / mods (click = picker)
 *
 * Listening decisions, not facts: kSemitonesPerUnit (the V/oct scale of the
 * summed channel), the knob travel window, the CV-out scaling.
 */
#include "daisy_patch.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "elements/dsp/dsp.h"
#include "elements/dsp/part.h"
#include "module_picker.h"

using namespace daisy;

DaisyPatch hw;

static constexpr size_t BLOCK = elements::kMaxBlockSize; /* 16 frames at 32 kHz */

/* PITCH channel. Elements' COARSE spans 60 semitones, and on the Patch the
 * same ADC reading carries the CV, so one unit of the channel is taken as
 * 5 V: 60 semitones = 12 per volt. UNCALIBRATED -- the summing network's
 * real gain is not in any source read for this port; check with a tuner
 * and a known 1 V step before trusting it. */
static constexpr float kSemitonesPerUnit = 60.0f;
/* note at CTRL 1 = 0, before Trans: Elements' 24.1 + 19 (cv_scaler.cc) */
static constexpr float kNoteBase = 43.1f;

/* Knob travel for the paged params: a real Patch knob stops short of both
 * ends (memory: knobs don't reach full scale; CyberDuck79's defaults are
 * 0.03..0.96), so 0.02..0.97 of travel spans the whole param. Not applied
 * to PITCH, where volts are volts. */
static constexpr float kTravelLo = 0.02f, kTravelHi = 0.97f;
/* Pickup: a knob takes over a param once it crosses it or comes within
 * this much of it (Belt's PICKUP_SLACK 3 of 127). */
static constexpr float kPickupSlack = 3.0f / 127.0f;

/* ------------------------------------------------------------------------ */
/* Params                                                                    */

enum Param
{
    P_GEOM, P_BRIGHT, P_DAMP,
    P_POS, P_SPACE, P_CONTOUR,
    P_BOW, P_BLOW, P_STRIKE,
    P_FLOW, P_MALLET, P_FINE,
    P_BOW_T, P_BLOW_T, P_STRIKE_T,
    P_COUNT
};
static constexpr int N_PAGES = P_COUNT / 3;
static const char *const PAGE_NAME[N_PAGES] = {"RESON", "SPACE", "LEVEL", "META", "TIMBR"};
static const char *const PARAM_LABEL[P_COUNT] = {
    "geom", "brit", "damp", "posn", "spac", "cont", "bow", "blow", "strk",
    "flow", "mall", "fine", "bowT", "blwT", "stkT"};

/* Power-up values: the Alchemy build's first-boot panel (its Targets
 * defaults), which is a struck modal bar with the reverb at noon. */
static float g_val[P_COUNT] = {
    0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.0f, 0.0f, 0.8f,
    0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};

static const char *const MODEL_NAME[4] = {"Modal", "String", "Strings", "Ominous"};

struct Settings
{
    int page;  /* 0..N_PAGES-1 */
    int model; /* 0..2 resonator model, 3 = Ominous (the easter egg) */
    int trans; /* semitones, -24..+24 */
};
static Settings g = {0, 0, 0};

/* ------------------------------------------------------------------------ */
/* Engine                                                                    */

/* Part (~100 KB with its voices) and the reverb's 64 KB buffer in D2 SRAM,
 * the NOLOAD .heap section BOOT_SRAM's linker script leaves empty (Belt and
 * Meld use the same trick); zeroed in main() before Init, as upstream's
 * .bss / CCM was. */
static uint16_t __attribute__((section(".heap"), aligned(32))) reverb_buffer[32768];
static uint8_t __attribute__((section(".heap"), aligned(32))) part_mem[sizeof(elements::Part)];
static elements::Part *part;

static float blow_in[BLOCK], strike_in[BLOCK], out_main[BLOCK], out_aux[BLOCK];

/* Control loop -> callback. Single floats/ints: torn reads are harmless. */
static volatile float g_pitch_unit = 0.5f; /* CTRL 1, smoothed */
static volatile int   g_model_req  = 0;
static volatile float g_exc_level, g_res_level;
static volatile bool  g_gate_engine;

static float sm[P_COUNT];
static bool  sm_primed;
static float note_cv, strike_level, blow_level;
static bool  prev_gate, gate_now;
static int   applied_model = -1;

static float          g_ticks_per_block_inv;
static volatile float g_load_peak;
static CpuLoadMeter   g_cpu;

static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

static inline float quadratic_bipolar(float x) /* cv_scaler.cc LAW_QUADRATIC_BIPOLAR */
{
    const float x2 = x * x * 3.3f;
    return x < 0.f ? -x2 : x2;
}

static void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)
{
    const uint32_t t0 = System::GetTick();
    g_cpu.OnBlockStart();
    if(size > BLOCK)
    {
        for(size_t i = 0; i < size; i++)
            for(int c = 0; c < 4; c++) out[c][i] = 0.0f;
        g_cpu.OnBlockEnd();
        return;
    }

    const int m = g_model_req;
    if(m != applied_model)
    {
        applied_model = m;
        part->set_easter_egg(m == 3);
        if(m < 3) part->set_resonator_model(static_cast<elements::ResonatorModel>(m));
    }

    /* cv_scaler.cc's smoothing: front-panel pots low-passed at 0.01 per
     * block, the params that also took CV upstream at 0.05. */
    static const float kCoef[P_COUNT] = {0.05f, 0.05f, 0.05f, 0.01f, 0.01f, 0.01f, 0.01f, 0.01f,
                                         0.01f, 0.05f, 0.05f, 0.01f, 0.05f, 0.05f, 0.05f};
    for(int i = 0; i < P_COUNT; i++)
    {
        if(!sm_primed) sm[i] = g_val[i];
        sm[i] += kCoef[i] * (g_val[i] - sm[i]);
    }
    sm_primed = true;

    elements::Patch *p          = part->mutable_patch();
    p->resonator_geometry       = clampf(sm[P_GEOM], 0.f, 0.9995f);
    p->resonator_brightness     = clampf(sm[P_BRIGHT], 0.f, 0.9995f);
    p->resonator_damping        = clampf(sm[P_DAMP], 0.f, 0.9995f);
    p->resonator_position       = clampf(sm[P_POS], 0.f, 0.9995f);
    p->exciter_bow_level        = sm[P_BOW];
    p->exciter_blow_level       = sm[P_BLOW];
    p->exciter_strike_level     = sm[P_STRIKE];
    p->exciter_envelope_shape   = sm[P_CONTOUR];
    p->exciter_blow_meta        = clampf(sm[P_FLOW], 0.f, 0.9995f);
    p->exciter_strike_meta      = clampf(sm[P_MALLET], 0.f, 0.9995f);
    p->exciter_bow_timbre       = clampf(sm[P_BOW_T], 0.f, 0.9995f);
    p->exciter_blow_timbre      = clampf(sm[P_BLOW_T], 0.f, 0.9995f);
    p->exciter_strike_timbre    = clampf(sm[P_STRIKE_T], 0.f, 0.995f);
    /* SPACE knob: 0..2 as on the Alchemy build (the last eighth freezes). */
    p->space = clampf(sm[P_SPACE] * 2.0f, 0.f, 2.f);

    /* PITCH: knob and V/oct in one reading. A jump of more than a quarter
     * tone lands at once, smaller moves glide (cv_scaler.cc's note filter). */
    const float note     = kNoteBase + kSemitonesPerUnit * g_pitch_unit + (float)g.trans;
    const float interval = note - note_cv;
    if(interval < -0.4f || interval > 0.4f)
        note_cv = note;
    else
        note_cv += 0.1f * interval;

    elements::PerformanceState s;
    s.note       = note_cv + quadratic_bipolar(2.0f * sm[P_FINE] - 1.0f) * (2.0f / 3.3f);
    s.modulation = 0.0f;
    s.strength   = 0.5f; /* STRENGTH unpatched */
    /* Upstream reads the gate one block after the CVs, so a note's pitch is
     * in before it strikes. */
    s.gate        = prev_gate;
    prev_gate     = gate_now;
    gate_now      = hw.gate_input[DaisyPatch::GATE_IN_1].State();
    g_gate_engine = s.gate;

    /* elements.cc's input noise gates: below -80 dB an input fades out, so
     * an unpatched jack's hiss never replaces the internal exciter. */
    const float kNoiseGateThreshold = 0.0001f;
    for(size_t i = 0; i < size; i++)
    {
        const float b = in[0][i], st = in[1][i];
        float       e, gn;
        e = st * st - strike_level;
        strike_level += e * (e > 0.0f ? 0.1f : 0.0001f);
        gn = strike_level <= kNoiseGateThreshold ? (1.0f / kNoiseGateThreshold) * strike_level : 1.0f;
        strike_in[i] = gn * st;
        e = b * b - blow_level;
        blow_level += e * (e > 0.0f ? 0.1f : 0.0001f);
        gn = blow_level <= kNoiseGateThreshold ? (1.0f / kNoiseGateThreshold) * blow_level : 1.0f;
        blow_in[i] = gn * b;
    }

    part->Process(s, blow_in, strike_in, out_main, out_aux, size);

    for(size_t i = 0; i < size; i++)
    {
        const float l = clampf(out_main[i], -1.f, 1.f), r = clampf(out_aux[i], -1.f, 1.f);
        out[0][i] = l;
        out[1][i] = r;
        out[2][i] = l;
        out[3][i] = r;
    }
    g_exc_level = part->exciter_level();
    g_res_level = part->resonator_level();

    g_cpu.OnBlockEnd();
    const float ld = (float)(System::GetTick() - t0) * g_ticks_per_block_inv;
    if(ld > g_load_peak) g_load_peak = ld;
}

/* ------------------------------------------------------------------------ */
/* Knobs: CTRL 1 direct, CTRL 2-4 paged with pickup                          */

static bool  g_live[3];
static float g_knob_at[3];

static float travel(float raw) { return clampf((raw - kTravelLo) / (kTravelHi - kTravelLo), 0.f, 1.f); }

static void page_reset(void)
{
    for(int k = 0; k < 3; k++)
    {
        g_live[k]    = false;
        g_knob_at[k] = travel(hw.GetKnobValue((DaisyPatch::Ctrl)(k + 1)));
    }
}

static void knobs(void)
{
    const float pr = hw.GetKnobValue(DaisyPatch::CTRL_1);
    g_pitch_unit   = g_pitch_unit + 0.5f * (pr - g_pitch_unit);

    for(int k = 0; k < 3; k++)
    {
        const int   idx = g.page * 3 + k;
        const float v   = travel(hw.GetKnobValue((DaisyPatch::Ctrl)(k + 1)));
        if(!g_live[k])
        {
            const float cur = g_val[idx];
            if((g_knob_at[k] <= cur && v >= cur) || (g_knob_at[k] >= cur && v <= cur)
               || fabsf(v - cur) <= kPickupSlack)
                g_live[k] = true;
            g_knob_at[k] = v;
            if(!g_live[k]) continue;
        }
        g_val[idx]   = v;
        g_knob_at[k] = v;
    }
}

/* ------------------------------------------------------------------------ */
/* Menu: click to select, turn to change, click to let go (Meld ee11430)     */

enum MenuItem { MI_PAGE, MI_MODEL, MI_TRANS, MI_MODS, MI_COUNT };
static const char *const MENU_LABEL[MI_COUNT] = {"Page", "Model", "Trans", "mods"};
static int  g_sel = MI_PAGE;
static bool enc_down, enc_turned, g_editing;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void menu_edit(int inc)
{
    switch(g_sel)
    {
        case MI_PAGE:
            g.page = clampi(g.page + inc, 0, N_PAGES - 1);
            page_reset();
            break;
        case MI_MODEL: g.model = clampi(g.model + inc, 0, 3); g_model_req = g.model; break;
        case MI_TRANS: g.trans = clampi(g.trans + inc, -24, 24); break;
        default: break;
    }
}

/* Edges only: libDaisy's Pressed() lags RisingEdge by one 1 ms update and
 * drops out on a bounce, and on Tim's Patch the switch does not stay closed
 * while the knob turns (memory: push-and-turn is dead). */
static void encoder(void)
{
    if(hw.encoder.RisingEdge())
    {
        enc_down   = true;
        enc_turned = false;
    }
    const int inc = hw.encoder.Increment();
    if(inc)
    {
        if(enc_down) enc_turned = true;
        if(g_editing)
            menu_edit(inc);
        else
            g_sel = clampi(g_sel + inc, 0, MI_COUNT - 1);
    }
    if(hw.encoder.FallingEdge())
    {
        const bool click = enc_down && !enc_turned;
        enc_down         = false;
        if(!click) return;
        if(g_sel == MI_MODS)
        {
            picker::run(hw); /* modal; returns only if the user backs out */
            page_reset();    /* the knobs may have moved meanwhile */
        }
        else
            g_editing = !g_editing;
    }
}

/* ------------------------------------------------------------------------ */
/* Display                                                                   */

static const char *const NOTE_NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

static int g_cpu_avg, g_cpu_peak;

static void draw(void)
{
    char line[24];
    hw.display.Fill(false);

    /* Row 0: the note the resonator is on (MIDI numbering), and the DSP load
     * (avg/peak %, last second). */
    int n = (int)floorf(note_cv + 0.5f);
    if(n < 0) n = 0;
    if(n > 127) n = 127;
    snprintf(line, sizeof(line), "ELEMENTS %-3s%d c%02d/%02d", NOTE_NAME[n % 12], n / 12 - 1,
             g_cpu_avg > 99 ? 99 : g_cpu_avg, g_cpu_peak > 99 ? 99 : g_cpu_peak);
    hw.display.SetCursor(0, 0);
    hw.display.WriteString(line, Font_6x8, true);

    /* Rows 1-3: CTRL 2-4 on this page; '*' = not picked up yet. */
    for(int k = 0; k < 3; k++)
    {
        const int idx = g.page * 3 + k;
        snprintf(line, sizeof(line), "%d %-4s %3d%s", k + 2, PARAM_LABEL[idx], (int)(g_val[idx] * 100.0f + 0.5f),
                 g_live[k] ? "" : " *");
        hw.display.SetCursor(0, 8 + k * 8);
        hw.display.WriteString(line, Font_6x8, true);
    }

    /* Rows 4-7: the menu. */
    for(int row = 0; row < MI_COUNT; row++)
    {
        char        num[12];
        const char *value = "";
        switch(row)
        {
            case MI_PAGE: value = PAGE_NAME[g.page]; break;
            case MI_MODEL: value = MODEL_NAME[g.model]; break;
            case MI_TRANS:
                snprintf(num, sizeof(num), "%+d st", g.trans);
                value = num;
                break;
            case MI_MODS:
                snprintf(num, sizeof(num), "%dk", (int)(hw.AudioSampleRate() / 1000.0f + 0.5f));
                value = num;
                break;
        }
        snprintf(line, sizeof(line), "%c%-5s %s", row == g_sel ? (g_editing ? '*' : '>') : ' ', MENU_LABEL[row], value);
        hw.display.SetCursor(0, 32 + row * 8);
        hw.display.WriteString(line, Font_6x8, true);
    }
    hw.display.Update();
}

/* ------------------------------------------------------------------------ */

int main(void)
{
    hw.Init();
    hw.SetAudioBlockSize(BLOCK);
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_32KHZ);
    const float sr        = hw.AudioSampleRate();
    g_ticks_per_block_inv = 1.0f / ((float)System::GetTickFreq() * ((float)BLOCK / sr));
    g_cpu.Init(sr, BLOCK);

    memset(reverb_buffer, 0, sizeof(reverb_buffer));
    memset(part_mem, 0, sizeof(part_mem));
    part = new(part_mem) elements::Part();
    part->Init(reverb_buffer);
    /* Upstream seeds the per-unit "signature" (reverb and resonator
     * modulation character) from the F4's serial number; the H750's UID
     * does the same job. */
#ifdef TEST
    uint32_t seed[3] = {0x12345678u, 0x9abcdef0u, 0x0f1e2d3cu};
    part->Seed(seed, 3);
#else
    part->Seed(reinterpret_cast<uint32_t *>(UID_BASE), 3);
#endif
    g_model_req = g.model;

    hw.StartAdc();
    for(int i = 0; i < 8; i++)
    {
        hw.ProcessAllControls();
        System::Delay(1);
    }
    g_pitch_unit = hw.GetKnobValue(DaisyPatch::CTRL_1);
    page_reset();
    hw.StartAudio(AudioCallback);

    uint32_t last_draw = 0, last_cpu = 0;
    float    win_peak  = 0.0f;
    for(;;)
    {
        hw.ProcessAllControls();
        encoder();
        knobs();

        hw.gate_output.Write(g_gate_engine);
        hw.seed.dac.WriteValue(DacHandle::Channel::ONE, (uint16_t)(clampf(g_exc_level, 0.f, 1.f) * 4095.0f));
        hw.seed.dac.WriteValue(DacHandle::Channel::TWO, (uint16_t)(clampf(g_res_level, 0.f, 1.f) * 4095.0f));

        const uint32_t now = System::GetNow();
        const float    pk  = g_load_peak;
        g_load_peak        = 0.0f;
        if(pk > win_peak) win_peak = pk;
        if(now - last_cpu >= 1000)
        {
            last_cpu   = now;
            g_cpu_avg  = (int)(g_cpu.GetAvgCpuLoad() * 100.0f + 0.5f);
            g_cpu_peak = (int)(win_peak * 100.0f + 0.5f);
            win_peak   = 0.0f;
        }
        if(now - last_draw >= 33)
        {
            last_draw = now;
            draw();
        }
        System::Delay(1);
    }
}
