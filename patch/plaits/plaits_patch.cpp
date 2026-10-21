/*
 * Plaits on the Daisy Patch -- Mutable Instruments' macro-oscillator (24
 * synthesis models in three banks, with its low-pass gate and internal
 * decay envelope), loadable from the Patch's SD-card module picker.
 *
 * The DSP is plaits::Voice, vendored byte-identical from pichenettes/
 * eurorack under ../../vendor/eurorack (VENDOR.md) apart from one shimmed
 * header (shim/plaits/user_data.h: no user wavetables). It runs at its
 * native 48 kHz -- the Patch codecs' default rate, so nothing is resampled
 * -- in its native 12-frame Render() calls, two per 24-frame codec block.
 *
 * Panel (each knob is summed with its CV jack before the ADC, so every knob
 * is its param's CV input too, and none is paged):
 *
 *   CTRL 1   FREQUENCY -- five octaves, and the V/oct input
 *   CTRL 2   HARMONICS        CTRL 3   TIMBRE        CTRL 4   MORPH
 *   GATE IN 1   TRIG: strikes the model, the low-pass gate and the envelope
 *   OUT 1 / 2   OUT / AUX;  OUT 3 / 4 the same
 *   CV OUT 1    the OUT level (envelope follower), 0..5 V
 *   CV OUT 2    the AUX level, 0..5 V
 *   GATE OUT    the trigger as the engine saw it (Gate In 1, 5 ms min)
 *   Encoder     click to select a row ('*'), turn to change, click to let
 *               go: Model / Octave / Decay / Colour / Trig / mods (click on
 *               mods = the module picker)
 *
 * What Plaits reached by holding a button and turning a knob is here as
 * menu rows: DECAY (its MORPH-hold setting) and LPG COLOUR. The FM, TIMBRE
 * and MORPH attenuverters have no jack to scale on the Patch (the CVs are
 * summed into the knobs) and are left at noon = off, so the internal
 * envelope never pulls on TIMBRE / MORPH / pitch.
 *
 * Listening decisions, not facts: kSemitonesPerUnit (the V/oct scale of the
 * summed channel), the knob travel window, the CV-out envelope times.
 */
#include "daisy_patch.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "plaits/dsp/dsp.h"
#include "plaits/dsp/voice.h"
#include "stmlib/utils/buffer_allocator.h"
#include "module_picker.h"

using namespace daisy;

DaisyPatch hw;

static constexpr size_t kRender = plaits::kBlockSize; /* 12 */
/* The Voice's envelope and LPG coefficients assume kBlockSize frames per
 * Render(), so a 24-frame codec block is two calls: params at 2 kHz. */
static constexpr size_t BLOCK = 2 * kRender;

/* FREQUENCY channel: one unit = 60 semitones (5 octaves), so a CV summed
 * into it reads as 12 semitones per volt IF the summing network maps 5 V to
 * full scale. UNCALIBRATED -- check with a tuner and a known 1 V step. */
static constexpr float kSemitonesPerUnit = 60.0f;
/* note at CTRL 1 = 0, Octave 0: C1 (MIDI 24); noon is C3 + 6 = F#3 region. */
static constexpr float kNoteBase = 24.0f;
/* Plaits' pitch maths (dsp.h a0) assumes its codec's real rate, 47,872.34
 * Hz; at 48 kHz every note would be 4.6 cents sharp (mi-alchemy c70925e). */
static const float kSrCorrection = 12.0f * log2f(48000.0f / plaits::kCorrectedSampleRate);

/* Knob travel for HARMONICS / TIMBRE / MORPH: a real Patch knob stops short
 * of both ends (memory: knobs don't reach full scale), so 0.02..0.97 of the
 * travel spans the param. Not applied to FREQUENCY, where volts are volts. */
static constexpr float kTravelLo = 0.02f, kTravelHi = 0.97f;

static constexpr int kNumEngines = 24;
static const char *const MODEL_NAME[kNumEngines] = {
    "VA+VCF",     "PhaseDist",  "6op FM 1",   "6op FM 2",   "6op FM 3",  "WaveTerr",
    "StringMach", "Chiptune",   "VirtAnalog", "Waveshaper", "2op FM",    "Formant",
    "Harmonic",   "Wavetable",  "Chords",     "Speech",     "Swarm",     "Filt noise",
    "Particle",   "Inharm str", "Modal reso", "Bass drum",  "Snare",     "Hi-hat"};
static const char *const TRIG_NAME[3] = {"Auto", "Drone", "Trig"};

struct Settings
{
    int model;  /* 0..23 */
    int octave; /* -2..+3 */
    int decay;  /* 0..100 */
    int colour; /* 0..100: 0 = VCFA (bright), 100 = VCA */
    int trig;   /* 0 auto (drone until the first trigger), 1 drone, 2 triggered */
};
/* Plaits' factory state (settings.cc): the first classic model (virtual
 * analog), decay at noon, LPG colour VCFA. */
static Settings g = {8, 0, 50, 0, 0};

/* ------------------------------------------------------------------------ */
/* Engine                                                                    */

/* Plaits' 16 KB shared buffer (BufferAllocator scratch for the engines that
 * need it) and the Voice in D2 SRAM, the NOLOAD .heap section BOOT_SRAM's
 * linker script leaves empty (Belt / Meld do the same); zeroed before Init,
 * as upstream's .bss was. */
static char __attribute__((section(".heap"), aligned(32))) shared_buffer[16384];
static uint8_t __attribute__((section(".heap"), aligned(32))) voice_mem[sizeof(plaits::Voice)];
static plaits::Voice *voice;

static plaits::Patch        patch;
static plaits::Modulations  mods;
static plaits::Voice::Frame frames[kRender];

static volatile float g_freq_unit = 0.5f; /* CTRL 1 */
static volatile float g_knob[3]   = {0.5f, 0.5f, 0.5f};
static volatile float g_note      = 60.0f;
static volatile float g_env[2];
static volatile bool  g_gate_seen;
static volatile int   g_active_engine = 8;

static bool  s_auto_trig;
static float s_env_a, s_env_r;

static float          g_ticks_per_block_inv;
static volatile float g_load_peak;
static CpuLoadMeter   g_cpu;

static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

static void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)
{
    (void)in;
    const uint32_t t0 = System::GetTick();
    g_cpu.OnBlockStart();
    if(size != BLOCK)
    {
        for(size_t i = 0; i < size; i++)
            for(int c = 0; c < 4; c++) out[c][i] = 0.0f;
        g_cpu.OnBlockEnd();
        return;
    }

    const bool gate = hw.gate_input[DaisyPatch::GATE_IN_1].State();
    if(gate) s_auto_trig = true;
    const bool trig_patched = g.trig == 2 || (g.trig == 0 && s_auto_trig);
    if(gate) g_gate_seen = true;

    const float note = kNoteBase + kSemitonesPerUnit * g_freq_unit + 12.0f * (float)g.octave - kSrCorrection;
    g_note           = note;

    patch.note                        = note;
    patch.harmonics                   = g_knob[0];
    patch.timbre                      = g_knob[1];
    patch.morph                       = g_knob[2];
    patch.frequency_modulation_amount = 0.0f;
    patch.timbre_modulation_amount    = 0.0f;
    patch.morph_modulation_amount     = 0.0f;
    patch.engine                      = g.model;
    patch.decay                       = (float)g.decay / 100.0f;
    patch.lpg_colour                  = (float)g.colour / 100.0f;

    /* Everything the Patch has arrives through the knobs, so the CV side of
     * Modulations is empty except the trigger. */
    mods.engine            = 0.0f;
    mods.note              = 0.0f;
    mods.frequency         = 0.0f;
    mods.harmonics         = 0.0f;
    mods.timbre            = 0.0f;
    mods.morph             = 0.0f;
    mods.trigger           = gate ? 1.0f : 0.0f;
    mods.level             = 0.0f;
    mods.frequency_patched = false;
    mods.timbre_patched    = false;
    mods.morph_patched     = false;
    mods.trigger_patched   = trig_patched;
    mods.level_patched     = false;

    const float k = 1.0f / 32768.0f;
    float       e0 = g_env[0], e1 = g_env[1];
    for(size_t half = 0; half < 2; half++)
    {
        voice->Render(patch, mods, frames, kRender);
        for(size_t i = 0; i < kRender; i++)
        {
            const float o = (float)frames[i].out * k, a = (float)frames[i].aux * k;
            const size_t j = half * kRender + i;
            out[0][j] = o;
            out[1][j] = a;
            out[2][j] = o;
            out[3][j] = a;
            const float ao = fabsf(o), aa = fabsf(a);
            e0 += (ao > e0 ? s_env_a : s_env_r) * (ao - e0);
            e1 += (aa > e1 ? s_env_a : s_env_r) * (aa - e1);
        }
    }
    g_env[0]        = e0;
    g_env[1]        = e1;
    g_active_engine = voice->active_engine();

    g_cpu.OnBlockEnd();
    const float ld = (float)(System::GetTick() - t0) * g_ticks_per_block_inv;
    if(ld > g_load_peak) g_load_peak = ld;
}

/* ------------------------------------------------------------------------ */
/* Menu: click to select, turn to change, click to let go (Meld ee11430)     */

enum MenuItem { MI_MODEL, MI_OCT, MI_DECAY, MI_COLOUR, MI_TRIG, MI_MODS, MI_COUNT };
static const char *const MENU_LABEL[MI_COUNT] = {"Model", "Octave", "Decay", "Colour", "Trig", "mods"};
static int  g_sel = MI_MODEL;
static bool enc_down, enc_turned, g_editing;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void menu_edit(int inc)
{
    switch(g_sel)
    {
        case MI_MODEL: g.model = clampi(g.model + inc, 0, kNumEngines - 1); break;
        case MI_OCT: g.octave = clampi(g.octave + inc, -2, 3); break;
        case MI_DECAY: g.decay = clampi(g.decay + 2 * inc, 0, 100); break;
        case MI_COLOUR: g.colour = clampi(g.colour + 2 * inc, 0, 100); break;
        case MI_TRIG: g.trig = clampi(g.trig + inc, 0, 2); break;
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
            picker::run(hw); /* modal; returns only if the user backs out */
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

    /* Row 0: the note (MIDI numbering), and the DSP load (avg/peak %). */
    int n = (int)floorf(g_note + kSrCorrection + 0.5f);
    n     = clampi(n, 0, 127);
    snprintf(line, sizeof(line), "PLAITS %-2s%d   c%02d/%02d", NOTE_NAME[n % 12], n / 12 - 1,
             clampi(g_cpu_avg, 0, 99), clampi(g_cpu_peak, 0, 99));
    hw.display.SetCursor(0, 0);
    hw.display.WriteString(line, Font_6x8, true);

    /* Row 1: CTRL 2-4. */
    snprintf(line, sizeof(line), "H%3d  T%3d  M%3d", (int)(g_knob[0] * 100.0f + 0.5f),
             (int)(g_knob[1] * 100.0f + 0.5f), (int)(g_knob[2] * 100.0f + 0.5f));
    hw.display.SetCursor(0, 8);
    hw.display.WriteString(line, Font_6x8, true);

    /* Rows 2-7: the menu. */
    for(int row = 0; row < MI_COUNT; row++)
    {
        char        num[16];
        const char *value = num;
        switch(row)
        {
            case MI_MODEL: snprintf(num, sizeof(num), "%2d %s", g.model + 1, MODEL_NAME[g.model]); break;
            case MI_OCT: snprintf(num, sizeof(num), "%+d", g.octave); break;
            case MI_DECAY: snprintf(num, sizeof(num), "%d", g.decay); break;
            case MI_COLOUR: snprintf(num, sizeof(num), "%d %s", g.colour, g.colour < 50 ? "vcfa" : "vca"); break;
            case MI_TRIG:
                snprintf(num, sizeof(num), "%s%s", TRIG_NAME[g.trig],
                         g.trig == 0 ? (s_auto_trig ? " (lpg)" : " (drone)") : "");
                break;
            case MI_MODS: snprintf(num, sizeof(num), "%dk", (int)(hw.AudioSampleRate() / 1000.0f + 0.5f)); break;
        }
        snprintf(line, sizeof(line), "%c%-6s %s", row == g_sel ? (g_editing ? '*' : '>') : ' ', MENU_LABEL[row], value);
        hw.display.SetCursor(0, 16 + row * 8);
        hw.display.WriteString(line, Font_6x8, true);
    }
    hw.display.Update();
}

/* ------------------------------------------------------------------------ */

static float travel(float raw) { return clampf((raw - kTravelLo) / (kTravelHi - kTravelLo), 0.f, 1.f); }

int main(void)
{
    hw.Init();
    hw.SetAudioBlockSize(BLOCK);
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
    const float sr        = hw.AudioSampleRate();
    g_ticks_per_block_inv = 1.0f / ((float)System::GetTickFreq() * ((float)BLOCK / sr));
    g_cpu.Init(sr, BLOCK);
    s_env_a = 1.0f - expf(-1.0f / (0.002f * sr)); /* 2 ms attack */
    s_env_r = 1.0f - expf(-1.0f / (0.100f * sr)); /* 100 ms release */

    memset(shared_buffer, 0, sizeof(shared_buffer));
    memset(voice_mem, 0, sizeof(voice_mem));
    stmlib::BufferAllocator allocator(shared_buffer, sizeof(shared_buffer));
    voice = new(voice_mem) plaits::Voice();
    voice->Init(&allocator);
    memset(&patch, 0, sizeof(patch));
    memset(&mods, 0, sizeof(mods));

    hw.StartAdc();
    for(int i = 0; i < 8; i++)
    {
        hw.ProcessAllControls();
        System::Delay(1);
    }
    hw.StartAudio(AudioCallback);

    uint32_t last_draw = 0, last_cpu = 0, gate_until = 0;
    float    win_peak  = 0.0f;
    for(;;)
    {
        hw.ProcessAllControls();
        encoder();

        /* Knobs: CV already summed in. Light smoothing; the Voice
         * interpolates the rest across each block. */
        g_freq_unit = g_freq_unit + 0.5f * (hw.GetKnobValue(DaisyPatch::CTRL_1) - g_freq_unit);
        for(int k = 0; k < 3; k++)
        {
            const float v = travel(hw.GetKnobValue((DaisyPatch::Ctrl)(k + 1)));
            g_knob[k]     = g_knob[k] + 0.3f * (v - g_knob[k]);
            if(fabsf(v - g_knob[k]) < 0.001f) g_knob[k] = v;
        }

        const uint32_t now = System::GetNow();
        if(g_gate_seen)
        {
            g_gate_seen = false;
            gate_until  = now + 5;
        }
        hw.gate_output.Write(now < gate_until || hw.gate_input[DaisyPatch::GATE_IN_1].State());
        hw.seed.dac.WriteValue(DacHandle::Channel::ONE, (uint16_t)(clampf(g_env[0], 0.f, 1.f) * 4095.0f));
        hw.seed.dac.WriteValue(DacHandle::Channel::TWO, (uint16_t)(clampf(g_env[1], 0.f, 1.f) * 4095.0f));

        const float pk = g_load_peak;
        g_load_peak    = 0.0f;
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
