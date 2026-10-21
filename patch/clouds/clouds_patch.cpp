/*
 * Clouds on the Electrosmith Daisy Patch -- Mutable Instruments' texture
 * synthesizer (granular, pitch-shifter / time-stretcher, looping delay,
 * spectral), loaded from the Patch's SD-card module picker.
 *
 * The DSP is clouds::GranularProcessor from vendor/eurorack, byte-identical
 * (vendor/VENDOR.md), the same tree the Alchemy Lab build (src/clouds) runs.
 * This file is the Patch shim: what Clouds' cv_scaler.cc and ui.cc did, on
 * four knob+CV channels, an encoder, a 128x64 OLED and two gates.
 *
 * SAMPLE RATE. The codec runs at Clouds' native 32 kHz with its native
 * 32-frame block, so the parameter smoothing below runs at 1 kHz exactly as
 * cv_scaler.cc's did and nothing in the engine is rescaled. libDaisy's SAI
 * offers 32 kHz, and the Patch's codecs take their rate from the SAI clocks.
 * The Alchemy build runs it the same way; on the Patch it is UNHEARD.
 *
 *   PLAY page     knob 1 POSITION . 2 SIZE . 3 PITCH . 4 DENSITY
 *   BLEND page    knob 1 TEXTURE  . 2 BLEND (dry/wet) . 3 FEEDBACK . 4 REVERB
 *   menu          Page . Mode . Quality . Spread . Gain . Freeze . Mods
 *   in 1/2        audio in L/R          out 1/2   audio out L/R
 *   in 3/4        unused                out 3/4   in 1/2 dry, thru
 *   gate in 1     FREEZE (high = frozen, ORed with the menu's latch)
 *   gate in 2     TRIGGER (seed a grain)
 *   gate out      high while frozen
 *   CV out 1      envelope of the output    CV out 2   envelope of the input
 *
 * Knobs are paged with pickup (patch/common/patch_ui.h). Each knob's CV is
 * summed in before the ADC, so a CV modulates whichever parameter that knob
 * addresses on the page on screen; PLAY is the page to patch CV into.
 * PITCH CV is therefore not calibrated V/Oct: knob 3 spans -24..+24 semitones
 * through Clouds' quantized curve.
 *
 * Numbers that are listening decisions, not facts: the Gain default, the
 * envelope times.
 */
#include "daisy_patch.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "clouds/dsp/granular_processor.h"
#include "clouds/resources.h"
#include "stmlib/dsp/dsp.h"

#include "module_picker.h"
#include "patch_ui.h"

using namespace daisy;

static DaisyPatch hw;

static constexpr size_t kBlock = clouds::kMaxBlockSize; /* 32 */
static constexpr float  kRate  = 32000.0f;

/* Clouds' two buffers, as upstream sizes them: the recording buffer (main
 * SRAM on the F405) and the working buffer (its CCM). Both in D2 SRAM via
 * the BOOT_SRAM linker script's NOLOAD .heap section (Meld's and Belt's
 * trick): 184 KB of RAM_D2's 256 KB. NOLOAD = not zeroed; main() clears
 * them. newlib's heap base `end` lands after them and nothing here mallocs. */
static uint8_t __attribute__((section(".heap"), aligned(32))) block_mem[118784];
static uint8_t __attribute__((section(".heap"), aligned(32))) block_ccm[65536 - 128];

static clouds::GranularProcessor processor;

/* ---- settings ----------------------------------------------------------- */

enum { PG_PLAY, PG_BLEND, PG_COUNT };
static const char *const PAGE_NAME[PG_COUNT] = {"Play", "Blend"};
static const char *const KNOB_LABEL[PG_COUNT][4] = {
    {"Pos", "Size", "Ptch", "Dens"},
    {"Tex", "Blnd", "Fdbk", "Verb"},
};

static const char *const MODE_NAME[4] = {"Granular", "Stretch", "Loop dly", "Spectral"};
static const char *const MODE_HDR[4]  = {"GRANULAR", "STRETCH", "LOOP DLY", "SPECTRAL"};
static const char *const QUAL_NAME[4] = {"16b st", "16b mono", "8b st", "8b mono"};

struct Settings
{
    int mode    = 0;  /* clouds::PlaybackMode */
    int quality = 0;  /* 0..3, set_quality() */
    int spread  = 50; /* % */
    int gain_db = 0;  /* -12..+18 dB */
    bool freeze = false;
};
static Settings g;

static pui::PagedKnobs<PG_COUNT> knobs;

/* ---- control -> audio ----------------------------------------------------- */

struct Targets
{
    volatile float position = 0.5f, size = 0.5f, pitch = 0.5f, density = 0.5f;
    volatile float texture = 0.5f, dry_wet = 0.5f, feedback = 0.f, reverb = 0.f;
    volatile float spread = 0.5f, gain = 1.f;
    volatile bool  freeze = false;
};
static Targets T;

static volatile bool  G_FROZEN;
static volatile float G_PITCH; /* semitones, for the screen */
static pui::LoadMeter load;
static pui::Env       env_out, env_in;

/* cv_scaler.cc's state, carried between blocks. */
static float sm_position = 0.5f, sm_size = 0.5f, sm_density = 0.5f;
static float sm_texture = 0.5f, sm_dry_wet = 0.5f, sm_spread = 0.5f;
static float sm_feedback = 0.f, sm_reverb = 0.f;
static bool  s_trig_delay = false;

static clouds::ShortFrame s_in[kBlock], s_out[kBlock];

static inline short f2s(float x)
{
    x *= 32768.0f;
    return (short)(x > 32767.0f ? 32767.0f : x < -32768.0f ? -32768.0f : x);
}

static void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)
{
    const uint32_t t0 = System::GetTick();

    if(size > kBlock)
    {
        /* Refuse to run rather than overrun the engine's block arrays. */
        for(size_t i = 0; i < size; i++)
            for(int c = 0; c < 4; c++) out[c][i] = 0.f;
        return;
    }

    const bool freeze_gate = hw.gate_input[DaisyPatch::GATE_IN_1].State();
    const bool trig_edge   = hw.gate_input[DaisyPatch::GATE_IN_2].Trig();

    /* cv_scaler.cc's one-pole coefficients at the same 1 kHz. */
    clouds::Parameters *p = processor.mutable_parameters();
    sm_position += 0.05f * (T.position - sm_position);
    sm_density += 0.01f * (T.density - sm_density);
    sm_size += 0.01f * (T.size - sm_size);
    sm_texture += 0.01f * (T.texture - sm_texture);
    sm_dry_wet += 0.05f * (T.dry_wet - sm_dry_wet);
    sm_spread += 0.05f * (T.spread - sm_spread);
    sm_feedback += 0.05f * (T.feedback - sm_feedback);
    sm_reverb += 0.05f * (T.reverb - sm_reverb);

    p->position = sm_position;
    p->size     = sm_size;
    p->density  = sm_density;
    p->texture  = sm_texture;
    /* Upstream's crossfade reads one float past its 17-entry table at
     * exactly 1.0 (ASan, test/test_clouds.cpp): stop just short. */
    p->dry_wet       = pui::clampf(sm_dry_wet * 1.05f - 0.025f, 0.f, 0.99999f);
    p->stereo_spread = sm_spread;
    p->feedback      = sm_feedback;
    p->reverb        = sm_reverb;
    p->pitch         = pui::clampf(stmlib::Interpolate(clouds::lut_quantized_pitch, T.pitch, 1024.0f), -48.f, 48.f);
    G_PITCH          = p->pitch;

    p->freeze = T.freeze || freeze_gate;
    G_FROZEN  = p->freeze;

    /* Upstream delays the trigger by the ADC latency so the grain lands
     * with the CV that came with it; one block does the same here. */
    p->trigger   = s_trig_delay;
    p->gate      = hw.gate_input[DaisyPatch::GATE_IN_2].State();
    s_trig_delay = trig_edge;

    const float gain = T.gain;
    for(size_t i = 0; i < size; i++)
    {
        s_in[i].l = f2s(in[0][i] * gain);
        s_in[i].r = f2s(in[1][i] * gain);
    }

    processor.Process(s_in, s_out, size);

    const float k = 1.0f / 32768.0f;
    for(size_t i = 0; i < size; i++)
    {
        out[0][i] = s_out[i].l * k;
        out[1][i] = s_out[i].r * k;
        out[2][i] = in[0][i];
        out[3][i] = in[1][i];
        env_out.Process(0.5f * (out[0][i] + out[1][i]));
        env_in.Process(0.5f * (in[0][i] + in[1][i]));
    }

    load.Block(t0);
}

/* ---- knobs + settings -> targets ------------------------------------------ */

static void push_targets(void)
{
    T.position = knobs.value[PG_PLAY][0];
    T.size     = knobs.value[PG_PLAY][1];
    T.pitch    = knobs.value[PG_PLAY][2];
    T.density  = knobs.value[PG_PLAY][3];
    T.texture  = knobs.value[PG_BLEND][0];
    T.dry_wet  = knobs.value[PG_BLEND][1];
    T.feedback = knobs.value[PG_BLEND][2];
    T.reverb   = knobs.value[PG_BLEND][3];
    T.spread   = g.spread / 100.0f;
    T.gain     = powf(10.0f, g.gain_db / 20.0f);
    T.freeze   = g.freeze;
}

static int applied_mode = -1, applied_quality = -1;
static void apply_engine(void)
{
    /* set_quality() only flags a buffer reset; Prepare() performs it. */
    if(g.mode != applied_mode)
    {
        applied_mode = g.mode;
        processor.set_playback_mode(static_cast<clouds::PlaybackMode>(g.mode));
    }
    if(g.quality != applied_quality)
    {
        applied_quality = g.quality;
        processor.set_quality(g.quality);
    }
}

/* ---- menu ------------------------------------------------------------------ */

enum { MI_PAGE, MI_MODE, MI_QUAL, MI_SPREAD, MI_GAIN, MI_FREEZE, MI_MODS, MI_COUNT };
static const char *const MENU_LABEL[MI_COUNT] = {"Page", "Mode", "Qual", "Sprd", "Gain", "Frz", "Mods"};
static constexpr int     kMenuRows            = 5;
static pui::Menu         menu;

static void menu_edit(int inc)
{
    switch(menu.sel)
    {
        case MI_PAGE: knobs.SetPage(hw, pui::clampi(knobs.page + inc, 0, PG_COUNT - 1)); break;
        case MI_MODE: g.mode = pui::clampi(g.mode + inc, 0, 3); break;
        case MI_QUAL: g.quality = pui::clampi(g.quality + inc, 0, 3); break;
        case MI_SPREAD: g.spread = pui::clampi(g.spread + 5 * inc, 0, 100); break;
        case MI_GAIN: g.gain_db = pui::clampi(g.gain_db + inc, -12, 18); break;
        case MI_FREEZE: g.freeze = inc > 0; break;
    }
}

/* ---- display ---------------------------------------------------------------- */

static int knob_show(int page, int k)
{
    if(page == PG_PLAY && k == 2) return (int)lrintf(G_PITCH); /* semitones */
    return (int)(knobs.value[page][k] * 100.0f + 0.5f);
}

static void draw(void)
{
    char line[32];
    hw.display.Fill(false);

    /* Row 0: mode, page, freeze. */
    snprintf(line, sizeof(line), "%-8s %-5s %s", MODE_HDR[g.mode], PAGE_NAME[knobs.page], G_FROZEN ? "FRZ" : "");
    hw.display.SetCursor(0, 0);
    hw.display.WriteString(line, Font_6x8, true);

    /* Rows 1-2: this page's four knobs, '*' = waiting for pickup. */
    for(int r = 0; r < 2; r++)
    {
        const int a = r * 2, b = a + 1, pg = knobs.page;
        snprintf(line, sizeof(line), "%-4s%4d%c %-4s%4d%c", KNOB_LABEL[pg][a], knob_show(pg, a), knobs.Mark(a),
                 KNOB_LABEL[pg][b], knob_show(pg, b), knobs.Mark(b));
        hw.display.SetCursor(0, 8 + r * 8);
        hw.display.WriteString(line, Font_6x8, true);
    }

    /* Rows 3-7: the menu, scrolled to the cursor. */
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
            case MI_MODE: value = MODE_NAME[g.mode]; break;
            case MI_QUAL: value = QUAL_NAME[g.quality]; break;
            case MI_SPREAD: snprintf(num, sizeof(num), "%d", g.spread); break;
            case MI_GAIN: snprintf(num, sizeof(num), "%+ddB", g.gain_db); break;
            case MI_FREEZE: value = g.freeze ? "On" : "Off"; break;
            case MI_MODS:
                /* The rate the codec really runs at, and the worst audio
                 * block of the last half second (100% or more = overrun). */
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
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_32KHZ);
    const float sr = hw.AudioSampleRate();
    load.Init(sr, kBlock);
    env_out.Init(1.0f - expf(-1.0f / (0.002f * sr)), 1.0f - expf(-1.0f / (0.100f * sr)));
    env_in.Init(1.0f - expf(-1.0f / (0.002f * sr)), 1.0f - expf(-1.0f / (0.100f * sr)));

    memset(block_mem, 0, sizeof block_mem);
    memset(block_ccm, 0, sizeof block_ccm);
    processor.Init(block_mem, sizeof block_mem, block_ccm, sizeof block_ccm);

    /* Every knob addresses a parameter. BLEND page defaults (pickup from
     * boot): textured but plain grains, half wet, no feedback, some reverb.
     * The PLAY page adopts the knobs: whatever they point at is what plays,
     * as on a Clouds. (Density at noon is silence on Clouds.) */
    for(int p = 0; p < PG_COUNT; p++)
        for(int k = 0; k < 4; k++) knobs.used[p][k] = true;
    knobs.value[PG_BLEND][0] = 0.5f;
    knobs.value[PG_BLEND][1] = 0.5f;
    knobs.value[PG_BLEND][2] = 0.0f;
    knobs.value[PG_BLEND][3] = 0.3f;
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
    apply_engine();

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
            picker::run(hw); /* modal; returns only if the user backs out */
            knobs.Arm(hw);
        }

        knobs.Update(hw);
        push_targets();
        apply_engine();

        /* Clouds' main loop called Prepare() continuously: buffer resets on a
         * quality/mode change, and the phase vocoder's FFT in Spectral. */
        processor.Prepare();

        hw.gate_output.Write(G_FROZEN);
        hw.seed.dac.WriteValue(DacHandle::Channel::ONE,
                               (uint16_t)(pui::clampf(env_out.e * 2.0f, 0.f, 1.f) * 4095.0f));
        hw.seed.dac.WriteValue(DacHandle::Channel::TWO,
                               (uint16_t)(pui::clampf(env_in.e * 2.0f, 0.f, 1.f) * 4095.0f));

        const uint32_t now = System::GetNow();
        load.Take(now);
        if(now - last_draw >= 33u)
        {
            last_draw = now;
            draw();
        }
    }
}
