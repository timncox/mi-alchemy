/*
 * Beads on the Electrosmith Daisy Patch -- Mutable Instruments' texture
 * synthesizer (granular processor, delay, wavetable synth), loaded from the
 * Patch's SD-card module picker.
 *
 * The DSP is beads::Processor, beads::Synth and beads::GainControl from
 * vendor/eurorack (d8f4739), with the one flag the Alchemy Lab build also
 * changes (vendor/VENDOR.md): upstream's 48 kHz codec path, not its 24 kHz
 * one. This file is the Patch shim, modelled on src/beads/beads_alchemy.cpp:
 * what Beads' cv_reader.cc, ui.cc and beads.cc did, on four knob+CV
 * channels, an encoder, a 128x64 OLED and two gates.
 *
 * SAMPLE RATE. The codec runs at 48 kHz with 12-frame blocks (Beads' 4 kHz
 * block rate), as on the Lab. The engine runs its hi-fi and lo-fi qualities
 * from 48 kHz itself; the "Clouds" quality wants 32 kHz, and gets it from
 * the Lab build's 48<->32 kHz resampler (src/beads/resample_3_2.h), so the
 * SAI is never restarted. 4,000 audio interrupts a second plus libDaisy's
 * per-block 4-channel conversion is the CPU unknown: the Mods row shows the
 * worst block.
 *
 *   PLAY page   knob 1 TIME . 2 SIZE . 3 PITCH . 4 DENSITY
 *   MIX page    knob 1 SHAPE . 2 DRY/WET . 3 FEEDBACK . 4 REVERB
 *   RAND page   the four attenuverters: TIME~ . SIZE~ . PITCH~ . SHAPE~
 *               (randomness; noon is none, either side a different flavour)
 *   menu        Page . Quality . Input . Gain . Seed . Out . Freeze . Mods
 *   in 1/2      audio in L/R (Input Mono: in 1 only)
 *   out 1/2     audio out L/R (Out Gate: both carry the mono mix)
 *   out 3/4     in 1/2 dry, thru
 *   gate in 1   FREEZE (high = frozen, ORed with the menu's latch)
 *   gate in 2   SEED: a clock or gates seed the grains, DENSITY divides
 *               (CCW) or thins (CW) them. Seed Auto = counts as patched for
 *               8 s after a pulse; Off = ignored; Gate2 = always
 *   gate out    Out Gate: a gate per grain (Beads' gate-output mode);
 *               Out Stereo: high while frozen
 *   CV out 1    envelope of the output    CV out 2   envelope of the input
 *
 * Knobs are paged with pickup (patch/common/patch_ui.h); each knob's CV is
 * summed in before the ADC and so modulates whatever that knob addresses on
 * the page shown. PITCH CV is not calibrated V/Oct.
 *
 * There is no SEED button on the Patch: the seed state is always LOCKED
 * (DENSITY runs the grains), as an unpressed Beads boots.
 */
#include "daisy_patch.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "beads/dsp/processor.h"
#include "beads/dsp/synth.h"
#include "beads/gain_control.h"
#include "stmlib/dsp/dsp.h"
#include "stmlib/utils/gate_flags.h"

#include "beads_laws.h"
#include "resample_3_2.h"

#include "module_picker.h"
#include "patch_ui.h"

using namespace daisy;

static DaisyPatch hw;

static constexpr size_t kBlock     = 12;  /* Beads' 4 kHz block rate at 48 kHz */
static constexpr float  kBlockRate = 4000.0f;
static constexpr size_t kBlock32   = kBlock * 2 / 3;

/* Beads' buffers, sized as beads.cc: 768 KB of delay/recording memory in
 * SDRAM (cached) and the 64 KB reverb buffer in D2 SRAM through the
 * BOOT_SRAM linker script's NOLOAD .heap section. main() clears both. */
static uint8_t delay_buffer_1[512 * 1024] __attribute__((section(".sdram_bss"), aligned(32)));
static uint8_t delay_buffer_2[256 * 1024] __attribute__((section(".sdram_bss"), aligned(32)));
static uint16_t __attribute__((section(".heap"), aligned(32))) reverb_buffer[32768];

static beads::Processor     processor;
static beads::Synth         synth;
static beads::GainControl   gain_control;
static mi_beads::Resample32k src32;

/* ---- settings ----------------------------------------------------------- */

enum { PG_PLAY, PG_MIX, PG_RAND, PG_COUNT };
static const char *const PAGE_NAME[PG_COUNT] = {"Play", "Mix", "Rand"};
static const char *const KNOB_LABEL[PG_COUNT][4] = {
    {"Time", "Size", "Ptch", "Dens"},
    {"Shap", "Wet", "Fdbk", "Verb"},
    {"Tim~", "Siz~", "Pch~", "Shp~"},
};

/* QUALITY runs hi-fi -> lo-fi; Beads' Media enum runs the other way. */
static const char *const QUAL_NAME[4] = {"Hi-fi", "Clouds", "Lo-fi", "Tape"};
static const char *const QUAL_HDR[4]  = {"HIFI", "CLOUD", "LOFI", "TAPE"}; /* <= 5: row 0 is 21 columns */
enum { IN_STEREO, IN_MONO, IN_SYNTH };
static const char *const INPUT_NAME[3] = {"Stereo", "Mono", "Synth"};
enum { SEED_AUTO, SEED_OFF, SEED_G2 };
static const char *const SEED_NAME[3] = {"Auto", "Off", "Gate2"};
static const char *const OUT_NAME[2]  = {"Stereo", "Gate"};

struct Settings
{
    int  quality = 0;      /* 0 hi-fi .. 3 tape */
    int  input   = IN_STEREO;
    int  gain    = 0;      /* 0 = auto, 1..20 manual */
    int  seed    = SEED_AUTO;
    int  out     = 0;      /* 0 stereo, 1 gate */
    bool freeze  = false;
};
static Settings g;

static pui::PagedKnobs<PG_COUNT> knobs;

/* ---- control -> audio ----------------------------------------------------- */

struct Targets
{
    volatile float time = 0.5f, size = 0.5f, pitch = 0.f, shape = 0.5f;
    volatile float density = 0.5f, dry_wet = 0.5f;
    volatile float time_amt = 0.f, size_amt = 0.f, pitch_amt = 0.f, shape_amt = 0.f;
    volatile float feedback = 0.f, reverb = 0.f;
    volatile int   media    = beads::Processor::MEDIA_DIGITAL;
    volatile int   input    = IN_STEREO;
    volatile int   seed_in  = SEED_AUTO;
    volatile bool  gate_out = false;
    volatile uint8_t gain   = 0;
    volatile bool  freeze   = false;
};
static Targets T;

static volatile bool     G_FROZEN, G_PENDING;
static volatile uint32_t G_GATE_UNTIL;
static pui::LoadMeter    load;
static pui::Env          env_out, env_in;

/* beads.cc's state, carried between blocks. */
static float sm_time = 0.5f, sm_size = 0.5f, sm_pitch = 0.f, sm_shape = 0.5f;
static float sm_density = 0.5f, sm_dry_wet = 0.5f;
static float sm_time_amt = 0.f, sm_size_amt = 0.f, sm_pitch_amt = 0.f, sm_shape_amt = 0.f;
static float sm_feedback = 0.f, sm_reverb = 0.f;
static int   s_transition = 0;
static bool  s_synth      = false;
static stmlib::GateFlags s_prev_seed = stmlib::GATE_FLAG_LOW;
static uint32_t s_since_seed_edge    = 0xFFFFFFFFu; /* blocks */
static bool     s_seed_was           = false;

/* Beads' pots run a 0.05 one-pole at 4 kHz on a 16-bit ADC; the Patch's
 * knobs arrive at the main-loop rate in steps, so 0.01 (25 ms) as on the
 * Lab rounds them off. */
static constexpr float kSmooth = 0.01f;

static void Reconfigure(int media, int input)
{
    const bool synth_mode = input == IN_SYNTH;
    const bool mono       = input == IN_MONO;
    /* beads.cc resets the AGC when mono flips or synth mode ends. */
    const bool reset_gain = mono != processor.mono() || synth_mode != s_synth;
    if(!synth_mode) processor.Configure(static_cast<beads::Processor::Media>(media), mono);
    gain_control.Init(48000.0f,
                      media == beads::Processor::MEDIA_DIRTY_TAPE
                          ? beads::GainControl::SOFT_CLIPPING_MODE_ON
                          : (media == beads::Processor::MEDIA_CLEAN_TAPE
                                 ? beads::GainControl::SOFT_CLIPPING_MODE_DYNAMIC
                                 : beads::GainControl::SOFT_CLIPPING_MODE_OFF),
                      reset_gain);
    src32.Reset();
    s_synth      = synth_mode;
    s_transition = (int)(kBlockRate / 4.0f); /* codec_transition_frames */
}

using beads::FloatFrame;
static_assert(sizeof(FloatFrame) == sizeof(mi_beads::StereoF), "frame layout");

static beads::Codec::Frame s_in[kBlock];
static FloatFrame          f_in[kBlock], f_out[kBlock];
static FloatFrame          e_in[kBlock32], e_out[kBlock32];
static int                 s_cfg_media = -1, s_cfg_input = -1;

static void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)
{
    const uint32_t t0 = System::GetTick();

    if(size != kBlock)
    {
        for(size_t i = 0; i < size; i++)
            for(int c = 0; c < 4; c++) out[c][i] = 0.f;
        return;
    }

    const bool freeze_gate = hw.gate_input[DaisyPatch::GATE_IN_1].State();
    const bool seed_gate   = hw.gate_input[DaisyPatch::GATE_IN_2].State();
    if(seed_gate && !s_seed_was)
        s_since_seed_edge = 0;
    else if(s_since_seed_edge != 0xFFFFFFFFu)
        s_since_seed_edge++;
    s_seed_was = seed_gate;

    const bool frozen = T.freeze || freeze_gate;
    G_FROZEN          = frozen;

    /* MonitorStateChange(): a new medium or input reconfigures (clearing the
     * buffer), but never while frozen. Boot configures anyway. */
    const int  media = T.media, input = T.input;
    const bool changed = media != s_cfg_media || input != s_cfg_input;
    G_PENDING          = frozen && changed && s_cfg_media >= 0;
    if(changed && (!frozen || s_cfg_media < 0))
    {
        Reconfigure(media, input);
        s_cfg_media = media;
        s_cfg_input = input;
    }

    sm_time += kSmooth * (T.time - sm_time);
    sm_size += kSmooth * (T.size - sm_size);
    sm_pitch += kSmooth * (T.pitch - sm_pitch);
    sm_shape += kSmooth * (T.shape - sm_shape);
    sm_density += kSmooth * (T.density - sm_density);
    sm_dry_wet += kSmooth * (T.dry_wet - sm_dry_wet);
    sm_time_amt += kSmooth * (T.time_amt - sm_time_amt);
    sm_size_amt += kSmooth * (T.size_amt - sm_size_amt);
    sm_pitch_amt += kSmooth * (T.pitch_amt - sm_pitch_amt);
    sm_shape_amt += kSmooth * (T.shape_amt - sm_shape_amt);
    sm_feedback += kSmooth * (T.feedback - sm_feedback);
    sm_reverb += kSmooth * (T.reverb - sm_reverb);

    /* No normalization probes: every RandomizedParameter is "unpatched"
     * (has_cv false), so its attenuverter is a randomness amount, as on an
     * unpatched Beads. CV arrives summed into the knobs. */
    beads::Parameters p;
    p.time            = {sm_time, sm_time_amt, 0.f, false};
    p.size            = {sm_size, sm_size_amt, 0.f, false};
    p.shape           = {sm_shape, sm_shape_amt, 0.f, false};
    p.pitch           = {sm_pitch, sm_pitch_amt, 0.f, false};
    p.monophonic_mode = false;

    beads::ModulatedParameter d;
    d.base = sm_density;
    d.cv   = 0.0f;

    beads::Processor::MixParameters m;
    m.feedback      = pui::clampf(sm_feedback * 1.005f, 0.f, 1.f);
    m.dry_wet       = pui::clampf(sm_dry_wet * 1.005f, 0.f, 1.f);
    m.reverb_amount = pui::clampf(sm_reverb * 1.005f, 0.f, 1.f);
    m.output_gate   = T.gate_out;

    bool freeze[kBlock];
    for(size_t i = 0; i < size; i++) freeze[i] = frozen;

    const int  seed_in = T.seed_in;
    const bool g2      = seed_in == SEED_G2
                    || (seed_in == SEED_AUTO && s_since_seed_edge < (uint32_t)(8.0f * kBlockRate));
    stmlib::GateFlags  seed[kBlock];
    stmlib::GateFlags *seed_input = nullptr;
    if(g2)
    {
        for(size_t i = 0; i < size; i++) s_prev_seed = seed[i] = stmlib::ExtractGateFlags(s_prev_seed, seed_gate);
        seed_input = seed;
    }
    else
    {
        s_prev_seed = stmlib::GATE_FLAG_LOW;
    }

    /* Input: float -> the codec's 32-bit frames -> GainControl (auto or
     * manual gain, soft clipping by medium, mono copy). */
    const bool mono = s_cfg_input == IN_MONO;
    for(size_t i = 0; i < size; i++)
    {
        s_in[i].l = (int32_t)(pui::clampf(in[0][i], -1.f, 0.9999999f) * 2147483648.0f);
        s_in[i].r = (int32_t)(pui::clampf(in[1][i], -1.f, 0.9999999f) * 2147483648.0f);
    }
    gain_control.Process(s_transition == 0, T.gain, s_synth, s_synth || mono, s_in, f_in, size);

    bool silent = false;
    if(s_transition > 0)
    {
        --s_transition;
        silent = true;
    }
    else if(s_synth)
    {
        synth.Render(beads::SEED_BUTTON_STATE_LOCKED, seed_input, freeze,
                     static_cast<beads::Synth::Resolution>(s_cfg_media), d, p, m, f_out, size);
    }
    else if(processor.sample_rate() == 32000.0f)
    {
        /* The Clouds quality: the engine at its 32 kHz, 8 frames a block. */
        const size_t n = src32.Down(reinterpret_cast<const mi_beads::StereoF *>(f_in),
                                    reinterpret_cast<mi_beads::StereoF *>(e_in), size);
        processor.Process(beads::SEED_BUTTON_STATE_LOCKED, seed_input, freeze, d, p, m, e_in, e_out, n);
        src32.Up(reinterpret_cast<const mi_beads::StereoF *>(e_out), reinterpret_cast<mi_beads::StereoF *>(f_out), n);
        if(T.gate_out)
            for(size_t i = 0; i < size; i++) f_out[i].r = e_out[i * 2u / 3u].r; /* a logic level: hold */
    }
    else
    {
        processor.Process(beads::SEED_BUTTON_STATE_LOCKED, seed_input, freeze, d, p, m, f_in, f_out, size);
    }

    bool grain = false;
    for(size_t i = 0; i < size; i++)
    {
        float l = 0.f, r = 0.f;
        if(!silent)
        {
            l = pui::clampf(f_out[i].l, -1.0f, 0.999999f);
            if(T.gate_out)
            {
                /* Beads writes the gate (-0.8) where R was; it goes to the
                 * Patch's gate out instead, and R carries the mono mix. */
                if(f_out[i].r != 0.f) grain = true;
                r = l;
            }
            else
            {
                r = pui::clampf(f_out[i].r, -1.0f, 0.999999f);
            }
        }
        out[0][i] = l;
        out[1][i] = r;
        out[2][i] = in[0][i];
        out[3][i] = in[1][i];
        env_out.Process(0.5f * (l + r));
        env_in.Process(0.5f * (in[0][i] + in[1][i]));
    }
    if(grain) G_GATE_UNTIL = System::GetNow() + 2u;

    load.Block(t0);
}

/* ---- knobs + settings -> targets ------------------------------------------ */

static uint8_t gain_setting(int gidx)
{
    /* 0 = AUTO; 1..20 = Beads' manual 1..255 (GainControl: x1..x40 on a
     * fourth-power curve). */
    if(gidx <= 0) return 0;
    return (uint8_t)(1.0f + (gidx - 1) * 254.0f / 19.0f + 0.5f);
}

static void push_targets(void)
{
    const auto &v = knobs.value;
    T.time      = v[PG_PLAY][0];
    T.size      = v[PG_PLAY][1];
    T.pitch     = mi_beads::DetuneLaw(v[PG_PLAY][2]);
    T.density   = v[PG_PLAY][3];
    T.shape     = v[PG_MIX][0];
    T.dry_wet   = v[PG_MIX][1];
    T.feedback  = v[PG_MIX][2];
    T.reverb    = v[PG_MIX][3];
    T.time_amt  = mi_beads::AttenuverterLaw(v[PG_RAND][0]);
    T.size_amt  = mi_beads::AttenuverterLaw(v[PG_RAND][1]);
    T.pitch_amt = mi_beads::AttenuverterLaw(v[PG_RAND][2]);
    T.shape_amt = mi_beads::AttenuverterLaw(v[PG_RAND][3]);
    T.media     = beads::Processor::MEDIA_DIGITAL - g.quality;
    T.input     = g.input;
    T.seed_in   = g.seed;
    T.gate_out  = g.out == 1;
    T.gain      = gain_setting(g.gain);
    T.freeze    = g.freeze;
}

/* ---- menu ------------------------------------------------------------------ */

enum { MI_PAGE, MI_QUAL, MI_INPUT, MI_GAIN, MI_SEED, MI_OUT, MI_FREEZE, MI_MODS, MI_COUNT };
static const char *const MENU_LABEL[MI_COUNT] = {"Page", "Qual", "Input", "Gain", "Seed", "Out", "Frz", "Mods"};
static constexpr int     kMenuRows            = 5;
static pui::Menu         menu;

static void menu_edit(int inc)
{
    switch(menu.sel)
    {
        case MI_PAGE: knobs.SetPage(hw, pui::clampi(knobs.page + inc, 0, PG_COUNT - 1)); break;
        case MI_QUAL: g.quality = pui::clampi(g.quality + inc, 0, 3); break;
        case MI_INPUT: g.input = pui::clampi(g.input + inc, 0, 2); break;
        case MI_GAIN: g.gain = pui::clampi(g.gain + inc, 0, 20); break;
        case MI_SEED: g.seed = pui::clampi(g.seed + inc, 0, 2); break;
        case MI_OUT: g.out = pui::clampi(g.out + inc, 0, 1); break;
        case MI_FREEZE: g.freeze = inc > 0; break;
    }
}

/* ---- display ---------------------------------------------------------------- */

static int knob_show(int page, int k)
{
    const float v = knobs.value[page][k];
    if(page == PG_PLAY && k == 2) return (int)lrintf(mi_beads::DetuneLaw(v)); /* semitones */
    if(page == PG_RAND) return (int)lrintf(100.0f * mi_beads::AttenuverterLaw(v)); /* -100..100 */
    return (int)(v * 100.0f + 0.5f);
}

static void draw(void)
{
    char line[32];
    hw.display.Fill(false);

    /* Row 0: BEADS, quality, page, freeze (WAIT = a quality/input change is
     * held back until FREEZE lets go). */
    snprintf(line, sizeof(line), "BEADS %-5s %-4s %s", QUAL_HDR[g.quality], PAGE_NAME[knobs.page],
             G_PENDING ? "WAIT" : G_FROZEN ? "FRZ" : "");
    hw.display.SetCursor(0, 0);
    hw.display.WriteString(line, Font_6x8, true);

    for(int r = 0; r < 2; r++)
    {
        const int a = r * 2, b = a + 1, pg = knobs.page;
        snprintf(line, sizeof(line), "%-4s%4d%c %-4s%4d%c", KNOB_LABEL[pg][a], knob_show(pg, a), knobs.Mark(a),
                 KNOB_LABEL[pg][b], knob_show(pg, b), knobs.Mark(b));
        hw.display.SetCursor(0, 8 + r * 8);
        hw.display.WriteString(line, Font_6x8, true);
    }

    menu.Scroll(kMenuRows);
    for(int row = 0; row < kMenuRows; row++)
    {
        const int item = menu.top + row;
        if(item >= MI_COUNT) break;
        char        num[16];
        const char *value = num;
        switch(item)
        {
            case MI_PAGE: value = PAGE_NAME[knobs.page]; break;
            case MI_QUAL: value = QUAL_NAME[g.quality]; break;
            case MI_INPUT: value = INPUT_NAME[g.input]; break;
            case MI_GAIN:
                if(g.gain == 0)
                    value = "Auto";
                else
                    snprintf(num, sizeof(num), "Man %d", g.gain);
                break;
            case MI_SEED: value = SEED_NAME[g.seed]; break;
            case MI_OUT: value = OUT_NAME[g.out]; break;
            case MI_FREEZE: value = g.freeze ? "On" : "Off"; break;
            case MI_MODS:
                snprintf(num, sizeof(num), "%dk %d%%", (int)(hw.AudioSampleRate() / 1000.0f + 0.5f),
                         (int)(load.shown * 100.0f));
                break;
        }
        snprintf(line, sizeof(line), "%c%-5s %s", menu.Cursor(item), MENU_LABEL[item], value);
        hw.display.SetCursor(0, 24 + row * 8);
        hw.display.WriteString(line, Font_6x8, true);
    }
    hw.display.Update();
}

/* ---- main ---------------------------------------------------------------------- */

int main(void)
{
    hw.Init();
    hw.SetAudioBlockSize(kBlock);
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
    const float sr = hw.AudioSampleRate();
    load.Init(sr, kBlock);
    env_out.Init(1.0f - expf(-1.0f / (0.002f * sr)), 1.0f - expf(-1.0f / (0.100f * sr)));
    env_in.Init(1.0f - expf(-1.0f / (0.002f * sr)), 1.0f - expf(-1.0f / (0.100f * sr)));

    memset(delay_buffer_1, 0, sizeof delay_buffer_1);
    memset(delay_buffer_2, 0, sizeof delay_buffer_2);
    memset(reverb_buffer, 0, sizeof reverb_buffer);

    /* beads.cc Init(): three views of the 768 KB, as upstream. */
    uint8_t *delay_buffers[3] = {delay_buffer_1, delay_buffer_1 + 256 * 1024, delay_buffer_2};
    processor.Init(delay_buffers, reverb_buffer);
    synth.Init(reverb_buffer);
    src32.Init();
    gain_control.Init(48000.0f, beads::GainControl::SOFT_CLIPPING_MODE_OFF, true);

    /* Defaults as a fresh Beads (settings.cc): no randomness, a little
     * reverb, smooth grains, half wet. The PLAY page adopts the knobs.
     * (Density at noon is silence on Beads.) */
    for(int p = 0; p < PG_COUNT; p++)
        for(int k = 0; k < 4; k++) knobs.used[p][k] = true;
    knobs.value[PG_MIX][0]  = 0.5f;
    knobs.value[PG_MIX][1]  = 0.5f;
    knobs.value[PG_MIX][2]  = 0.0f;
    knobs.value[PG_MIX][3]  = 0.3f;
    for(int k = 0; k < 4; k++) knobs.value[PG_RAND][k] = 0.5f;
    hw.StartAdc();
    for(int i = 0; i < 8; i++)
    {
        hw.ProcessAllControls();
        System::Delay(1);
    }
    for(int k = 0; k < 4; k++)
        knobs.value[PG_PLAY][k] = pui::knob_law(hw.GetKnobValue((DaisyPatch::Ctrl)k));
    knobs.SetPage(hw, PG_PLAY);
    for(int k = 0; k < 4; k++) knobs.live[k] = true;

    menu.count = MI_COUNT;
    push_targets();

    hw.StartAudio(AudioCallback);

    uint32_t last_draw = 0;
    for(;;)
    {
        hw.ProcessAllControls();

        bool      click_mods;
        const int edit = menu.Poll(hw, MI_MODS, &click_mods);
        if(edit) menu_edit(edit);
        if(click_mods)
        {
            picker::run(hw);
            knobs.Arm(hw);
        }

        knobs.Update(hw);
        push_targets();

        const uint32_t now = System::GetNow();
        hw.gate_output.Write(T.gate_out ? (int32_t)(G_GATE_UNTIL - now) > 0 : G_FROZEN);
        hw.seed.dac.WriteValue(DacHandle::Channel::ONE,
                               (uint16_t)(pui::clampf(env_out.e * 2.0f, 0.f, 1.f) * 4095.0f));
        hw.seed.dac.WriteValue(DacHandle::Channel::TWO,
                               (uint16_t)(pui::clampf(env_in.e * 2.0f, 0.f, 1.f) * 4095.0f));

        load.Take(now);
        if(now - last_draw >= 33u)
        {
            last_draw = now;
            draw();
        }
        System::Delay(1);
    }
}
