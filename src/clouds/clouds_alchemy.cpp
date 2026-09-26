/*
 * Clouds on the Hermetic Modular Alchemy Lab V2 -- Mutable Instruments'
 * texture synthesizer (granular, pitch-shifter / time-stretcher, looping
 * delay, spectral) as Alchemy SDK firmware.
 *
 * The DSP is clouds::GranularProcessor, vendored byte-identical from
 * pichenettes/eurorack (vendor/VENDOR.md), run at its native 32 kHz with its
 * native 32-frame block. This file is the host shim: it does what Clouds'
 * cv_scaler.cc and ui.cc did, on six pots, three buttons and ten jacks.
 *
 *   PLAY   POSITION . SIZE . PITCH . DENSITY . TEXTURE . BLEND (dry/wet)
 *   SETUP  MODE . QUALITY . SPREAD . FEEDBACK . REVERB . IN GAIN  (hold B3)
 *   B1     FREEZE: hold for momentary, tap to latch (J3 ORs in)
 *   B2     SEED: fire a grain now (J4's trigger, by hand)
 *   J3 freeze gate . J4 trigger . J5 V/Oct . J6-J8 CV position/size/density
 *   Settings (B2+B3 2 s): brightness, presets; page 1 = the SD picker
 *
 * Clouds' BLEND knob cycled through four parameters (dry/wet, spread,
 * feedback, reverb) by a button; here they each have a pot. See DESIGN.md.
 */
#include <math.h>
#include <string.h>

#include "daisy_seed.h"
#include "util/CpuLoadMeter.h"

#include "alchemy/hw/alchemy_lab.h"
#include "alchemy/host_link/cdc_transport.h"
#include "alchemy/host_link/host.h"
#include "alchemy/storage/fs_extension.h"
#include "alchemy/storage/sd_card.h"
#include "alchemy/surface/control_loop.h"
#include "alchemy/surface/cv_matrix.h"
#include "alchemy/surface/jack.h"
#include "alchemy/surface/manual.h"
#include "alchemy/surface/page.h"
#include "alchemy/surface/pager.h"
#include "alchemy/surface/presets.h"
#include "alchemy/surface/settings.h"
#include "alchemy/surface/virtual_button.h"
#include "alchemy/surface/virtual_knob.h"

#include "mi_family.h"
#include "picker.h"

#include "clouds/dsp/granular_processor.h"
#include "clouds/resources.h"
#include "stmlib/dsp/dsp.h"

#ifndef MI_VERSION
#define MI_VERSION "0.0.0"
#endif
#ifndef MI_GIT_HASH
#define MI_GIT_HASH "dev"
#endif

using namespace alchemy;
using daisy::System;

/* ---- geometry ----------------------------------------------------------- */

/* Clouds' own codec block: 32 frames at 32 kHz, so the parameter reads
 * below happen at 1 kHz exactly as cv_scaler.cc's did, and every smoothing
 * coefficient carries over unchanged. */
static constexpr uint32_t kBlockSize = clouds::kMaxBlockSize;

enum : uint8_t { kPagePlay = 0, kPageSetup = 1, kNumAppPages = 2 };
enum : uint8_t { kSettingsMain = 0, kSettingsFirmware = 1 };

/* CV indices: 0 = J3 ... 5 = J8. */
static constexpr uint8_t kCvFreeze = 0u, kCvTrig = 1u, kCvVoct = 2u;

/* ---- hardware + engine -------------------------------------------------- */

static AlchemyLab          hw;
static daisy::CpuLoadMeter cpu;

/* Clouds' two buffers, sized as upstream: the recording buffer (118,784 B,
 * main SRAM on the STM32F405) and the working buffer (65,408 B, the F4's
 * CCM). Both in cached D2 SRAM (184 KB of its 224 KB): DTCM would be the
 * CCM's twin, but the SDK and libDaisy leave it only ~48 KB. */
static uint8_t MI_D2_BSS block_mem[118784];
static uint8_t MI_D2_BSS block_ccm[65536 - 128];

static clouds::GranularProcessor processor;

/* ---- colours ------------------------------------------------------------ */

static constexpr LedPanel::Rgb kColPos    = {0x40, 0xC0, 0xFF};
static constexpr LedPanel::Rgb kColSize   = {0x60, 0xFF, 0xC0};
static constexpr LedPanel::Rgb kColPitchU = {0xFF, 0x90, 0x20};
static constexpr LedPanel::Rgb kColPitchD = {0x20, 0x90, 0xFF};
static constexpr LedPanel::Rgb kColDensU  = {0xFF, 0xFF, 0x80};
static constexpr LedPanel::Rgb kColDensD  = {0xC0, 0x60, 0xFF};
static constexpr LedPanel::Rgb kColTex    = {0xFF, 0x60, 0xA0};
static constexpr LedPanel::Rgb kColBlend  = {0xFF, 0xC0, 0x60};
static constexpr LedPanel::Rgb kColMode   = {0x00, 0xFF, 0x60};
static constexpr LedPanel::Rgb kColQual   = {0xFF, 0x40, 0x40};
static constexpr LedPanel::Rgb kColSpread = {0x80, 0xFF, 0xFF};
static constexpr LedPanel::Rgb kColFb     = {0xFF, 0x80, 0x00};
static constexpr LedPanel::Rgb kColVerb   = {0x80, 0x80, 0xFF};
static constexpr LedPanel::Rgb kColGain   = {0xFF, 0xFF, 0xFF};
static constexpr LedPanel::Rgb kColNotch  = {0xFF, 0xFF, 0xFF};
static constexpr LedPanel::Rgb kColDim    = {0x18, 0x18, 0x18};
static constexpr LedPanel::Rgb kColSetup  = {0x10, 0x30, 0x40};

static constexpr LedPanel::Rgb kIdle   = {0x00, 0x00, 0x30};
static constexpr LedPanel::Rgb kWhite  = {0xFF, 0xFF, 0xFF};
static constexpr LedPanel::Rgb kGreen  = {0x00, 0xFF, 0x00};
static constexpr LedPanel::Rgb kAmber  = {0xFF, 0x80, 0x00};
static constexpr LedPanel::Rgb kRed    = {0xFF, 0x00, 0x00};

static const char* const kModeLabels[4] = {"Granular", "Stretch", "Looping delay", "Spectral"};
static const char* const kQualLabels[4] = {"16-bit stereo", "16-bit mono", "8-bit stereo", "8-bit mono"};

/* ---- pages --------------------------------------------------------------- */

/* PLAY. Panel positions, front view:
 *      [P1] B1 [P2]
 *      [P3] B2 [P4]
 *      [P5] B3 [P6]
 * Every knob hands the engine 0..1, the range cv_scaler.cc worked in. */

static VirtualKnob position = VirtualKnob(kPotTopLeft, "Position")
    .Linear(0.f, 1.f).Ident("position")
    .Help("Where in the recording the grains are taken from: now at the "
          "left, the oldest audio at the right.")
    .Ring(Level(kColPos));

static VirtualKnob size = VirtualKnob(kPotTopRight, "Size")
    .Linear(0.f, 1.f).Ident("size")
    .Help("Grain size. In Looping delay it is the loop length; in Spectral "
          "the FFT window.")
    .Ring(Level(kColSize));

static VirtualKnob pitch = VirtualKnob(kPotMiddleLeft, "Pitch")
    .Linear(0.f, 1.f).Ident("pitch")
    .Help("Transposition, -2 to +2 octaves, with semitone detents near the "
          "middle (Clouds' quantized pitch curve). J5 adds 1 V/octave.")
    .Ring(Bipolar(kColPitchU, kColPitchD, kColNotch));

static VirtualKnob density = VirtualKnob(kPotMiddleRight, "Density")
    .Linear(0.f, 1.f).Ident("density")
    .Help("Grain rate. Noon is silence; clockwise is regular, "
          "counter-clockwise random.")
    .Ring(Bipolar(kColDensU, kColDensD, kColNotch));

static VirtualKnob texture = VirtualKnob(kPotBottomLeft, "Texture")
    .Linear(0.f, 1.f).Ident("texture")
    .Help("Grain envelope, square to smooth; past 3 o'clock it adds a "
          "diffuser.")
    .Ring(Level(kColTex));

static VirtualKnob blend = VirtualKnob(kPotBottomRight, "Blend")
    .Linear(0.f, 1.f).Ident("dry_wet")
    .Help("Dry / wet.")
    .Ring(Level(kColBlend));

/* SETUP: hold B3. The mode and quality buttons of Clouds, and the three
 * parameters its BLEND knob used to cycle to. */

static VirtualKnob mode = VirtualKnob(kPotTopLeft, "Mode")
    .Selector(4).Labels(kModeLabels).Ident("mode")
    .Ring(SelectorRing(kColMode, kColDim, 4));

static VirtualKnob quality = VirtualKnob(kPotTopRight, "Quality")
    .Selector(4).Labels(kQualLabels).Ident("quality")
    .Help("Buffer format: 16-bit stereo holds 1 s, mono 2 s, 8-bit "
          "(mu-law, the lo-fi sound) 2 s stereo, 4 s mono.")
    .Ring(SelectorRing(kColQual, kColDim, 4));

static VirtualKnob spread = VirtualKnob(kPotMiddleLeft, "Spread")
    .Linear(0.f, 1.f).Ident("stereo_spread")
    .Ring(Level(kColSpread));

static VirtualKnob feedback = VirtualKnob(kPotMiddleRight, "Feedback")
    .Linear(0.f, 1.f).Ident("feedback")
    .Ring(Level(kColFb));

static VirtualKnob reverb = VirtualKnob(kPotBottomLeft, "Reverb")
    .Linear(0.f, 1.f).Ident("reverb")
    .Ring(Level(kColVerb));

static VirtualKnob in_gain = VirtualKnob(kPotBottomRight, "In gain")
    .Linear(0.f, 1.f).Ident("in_gain")
    .Help("Input gain, -12 dB to +12 dB, 0 dB at noon.")
    .Ring(Bipolar(kColGain, kColGain, kColNotch));

static Page play_page = Page(kPagePlay).Name("Play").Color("#40c0ff")
    .Knobs(position, size, pitch, density, texture, blend);

static Page setup_page = Page(kPageSetup).Name("Setup").Color("#00ff60")
    .Knobs(mode, quality, spread, feedback, reverb, in_gain);

/* ---- descriptor metadata -------------------------------------------------- */

static Jack jk_in_l   ("J1",  "In L",        JackSig::AudioIn);
static Jack jk_in_r   ("J2",  "In R",        JackSig::AudioIn);
static Jack jk_freeze ("J3",  "Freeze",      JackSig::Trig);
static Jack jk_trig   ("J4",  "Trigger",     JackSig::Trig);
static Jack jk_voct   ("J5",  "V/Oct",       JackSig::CvBi);
static Jack jk_cv_pos ("J6",  "CV Position", JackSig::CvBi);
static Jack jk_cv_siz ("J7",  "CV Size",     JackSig::CvBi);
static Jack jk_cv_den ("J8",  "CV Density",  JackSig::CvBi);
static Jack jk_out_l  ("J9",  "Out L",       JackSig::AudioOut);
static Jack jk_out_r  ("J10", "Out R",       JackSig::AudioOut);

static VirtualButton bt_freeze = VirtualButton("b1", "Freeze")
    .Action("Hold", "Freeze while held")
    .Action("Tap", "Latch freeze on / off");

static VirtualButton bt_seed = VirtualButton("b2", "Seed")
    .Action("Tap", "Fire a grain now (the trigger input, by hand)");

static VirtualButton bt_setup = VirtualButton("b3", "Setup")
    .Action("Hold", "Show the Setup page");

static Manual manual = Manual()
    .Tagline("Texture synthesizer: granular, stretch, looping delay, spectral")
    .Preamble(
        "**Clouds** records J1/J2 into a buffer and plays it back as a cloud "
        "of grains. POSITION picks where in the recording, SIZE how long "
        "each grain is, PITCH transposes (V/Oct on J5), DENSITY sets how "
        "many (noon is none), TEXTURE shapes them, BLEND mixes dry and wet. "
        "Hold B3 for the Setup page: MODE (granular, pitch-shifter / "
        "time-stretcher, looping delay, spectral), QUALITY (the buffer's "
        "bit depth and channels), the stereo SPREAD, FEEDBACK and REVERB "
        "Clouds' BLEND knob used to cycle to, and IN GAIN. B1 freezes the "
        "buffer (hold, or tap to latch; a gate on J3 does the same); B2 "
        "fires a grain, like a trigger on J4. The DSP is Mutable "
        "Instruments' Clouds (Emilie Gillet, MIT), unmodified.");

/* ---- surfaces ------------------------------------------------------------ */

static ControlLoop loop(hw);
static Pager       pager = Pager(kNumAppPages, kNumPots)
                               .Shift(hw.buttons[kButtonB3], kPageSetup);

/* Home slot 8: see mi_family.h for the whole card's map. */
static mi::HomeSlot home(8u);
static Presets      presets(hw.seed.qspi);
static Settings     settings(hw, &pager);
static CvMatrix     cv_matrix(kNumCvInputs);
static SdCard       sd;
static hostlink::FsExtension fs_ext(sd);
static hostlink::Host host(presets, "clouds_alchemy", "Clouds",
                           MI_VERSION, MI_GIT_HASH);
static mi::CpuExtras extras(0x434C44u);   /* 'CLD' */

#ifdef MI_BENCH_USB
static daisy::UsbHandle          bench_usb;
static hostlink::CdcUsbTransport bench_cdc;
#endif

/* ---- control frame -> audio block ---------------------------------------- */

/*
 * The control thread writes knob targets here at the frame rate; the audio
 * callback smooths toward them once per block with cv_scaler.cc's own
 * coefficients (its reads were once per 32-frame block at 32 kHz as well).
 * Single floats and bools: a torn read is impossible on the M7.
 */
struct Targets
{
    volatile float position = 0.5f, size = 0.5f, pitch = 0.5f, density = 0.5f;
    volatile float texture = 0.5f, dry_wet = 0.5f, spread = 0.f, feedback = 0.f;
    volatile float reverb = 0.f, gain = 1.f;
    volatile bool  freeze_btn = false;
    volatile bool  seed_btn = false;   /* one-shot, cleared by the callback */
};
static Targets T;

static mi::Gate g_freeze, g_trig;

/* Read back by the LEDs. */
static volatile bool  G_FROZEN  = false;
static volatile float G_IN_PEAK = 0.0f;
static volatile uint32_t G_SEED_T = 0;

/* ---- buttons (1 ms poll) -------------------------------------------------- */

static mi::Toggle  tg_freeze;
static mi::Swallow swallow;
static bool        g_seed_down = false;

static void OnPoll(uint32_t now)
{
    if (settings.IsActive())
    {
        /* Settings owns the buttons; drop momentaries, keep latches. */
        tg_freeze.Reset();
        T.freeze_btn = tg_freeze.latch;
        g_seed_down  = true;   /* no seed from the press that leaves Settings */
        swallow.All();
        return;
    }
    T.freeze_btn = tg_freeze.Poll(swallow.Live(hw, kButtonB1), now);

    /* B2 stands down while B3 is held: B2+B3 held two seconds is the
     * Settings chord. */
    const bool b3 = swallow.Live(hw, kButtonB3);
    const bool b2 = !b3 && swallow.Live(hw, kButtonB2);
    if (b2 && !g_seed_down) T.seed_btn = true;
    g_seed_down = b2;
}

/* ---- control frame (~60 Hz) ------------------------------------------------ */

static mi::StoredWatch<kNumPots> play_watch;
static float g_saved_peak  = 0.0f;
static bool  prev_settings = false;
static int   applied_mode = -1, applied_quality = -1;

static void OnFrame(void)
{
    const uint32_t now = System::GetNow();

    if (play_watch.Changed(pager, kPagePlay)) home.Touch(now);

    /* Values are catch + locks + CV, already mixed by the SDK. */
    T.position = mi::clampf(position.Value(), 0.f, 1.f);
    T.size     = mi::clampf(size.Value(), 0.f, 1.f);
    T.pitch    = mi::clampf(pitch.Value(), 0.f, 1.f);
    T.density  = mi::clampf(density.Value(), 0.f, 1.f);
    T.texture  = mi::clampf(texture.Value(), 0.f, 1.f);
    T.dry_wet  = mi::clampf(blend.Value(), 0.f, 1.f);
    T.spread   = mi::clampf(spread.Value(), 0.f, 1.f);
    T.feedback = mi::clampf(feedback.Value(), 0.f, 1.f);
    T.reverb   = mi::clampf(reverb.Value(), 0.f, 1.f);
    /* -12..+12 dB, 0 dB at noon */
    T.gain = powf(4.0f, 2.0f * mi::clampf(in_gain.Value(), 0.f, 1.f) - 1.0f);

    /* SETUP selectors: each change is worth a flash write. set_quality()
     * only flags a buffer reset; Prepare() in the main loop performs it. */
    const int m = (int)mode.Value(), q = (int)quality.Value();
    if (m != applied_mode && m >= 0 && m < 4)
    {
        if (applied_mode >= 0) home.Touch(now);
        applied_mode = m;
        processor.set_playback_mode(static_cast<clouds::PlaybackMode>(m));
    }
    if (q != applied_quality && q >= 0 && q < 4)
    {
        if (applied_quality >= 0) home.Touch(now);
        applied_quality = q;
        processor.set_quality(q);
    }
    {
        static float seen[4] = {-1.f, -1.f, -1.f, -1.f};
        const float  v[4]    = {T.spread, T.feedback, T.reverb, in_gain.Value()};
        for (int i = 0; i < 4; i++)
        {
            if (seen[i] >= 0.f && fabsf(v[i] - seen[i]) > 0.01f) home.Touch(now);
            if (seen[i] < 0.f || fabsf(v[i] - seen[i]) > 0.01f) seen[i] = v[i];
        }
    }

    if (extras.Track(cpu, g_saved_peak)) home.Touch(now);

    const bool sact = settings.IsActive();
    if (prev_settings && !sact) home.Touch(now);   /* brightness etc. */
    prev_settings = sact;

    if (home.Service(presets, hw, now, sact || picker::Busy()))
        g_saved_peak = extras.cpu_peak;
}

/* ---- LEDs ------------------------------------------------------------------ */

/*
 * The SDK draws the rings from the knobs. Added:
 *   B1 pair   white while frozen, else dim
 *   B2 pair   the input meter (green, amber past -3 dBFS, red at clip);
 *             white for 60 ms when a grain is seeded; red when the audio
 *             callback averages over 80 %
 *   B3 pair   dim Setup tint
 *   P1 ring   the previous session's worst CPU load, 2.5 s at boot
 */
static uint32_t g_readout_until = 0;
static float    g_boot_peak     = 0.0f;

static void OnRender(uint32_t t_ms)
{
    LedPanel& L = hw.leds;
    if (t_ms < g_readout_until) mi::paint_cpu_readout(hw, g_boot_peak);
    if (settings.IsActive()) return;   /* Settings owns the buttons */

    L.SetButtonPair(kButtonB1, L.ScaleGlobal(G_FROZEN ? kWhite : kIdle));

    LedPanel::Rgb b2;
    const float pk = G_IN_PEAK;
    if (cpu.GetAvgCpuLoad() > 0.80f)   b2 = kRed;
    else if (t_ms - G_SEED_T < 60u)    b2 = kWhite;
    else if (pk > 0.98f)               b2 = kRed;
    else if (pk > 0.70f)               b2 = kAmber;
    else
    {
        const uint8_t g = (uint8_t)(0x10 + 0xE0 * mi::clampf(pk / 0.70f, 0.f, 1.f));
        b2 = {0x00, g, 0x00};
    }
    L.SetButtonPair(kButtonB2, L.ScaleGlobal(b2));

    if (pager.Page() != kPageSetup)
        L.SetButtonPair(kButtonB3, L.ScaleGlobal(kColSetup));
}

/* ---- audio ----------------------------------------------------------------- */

static clouds::ShortFrame s_in[kBlockSize], s_out[kBlockSize];

/* cv_scaler.cc's state, carried between blocks. */
static float sm_position = 0.5f, sm_size = 0.5f, sm_density = 0.5f;
static float sm_texture = 0.5f, sm_dry_wet = 0.5f, sm_spread = 0.f;
static float sm_feedback = 0.f, sm_reverb = 0.f, sm_note = 0.f;
static float sm_peak = 0.f;
static bool  s_trig_delay = false;

static void AudioCallback(daisy::AudioHandle::InputBuffer  in,
                          daisy::AudioHandle::OutputBuffer out,
                          size_t                           size)
{
    cpu.OnBlockStart();

    /* The gates want edges, so they are polled here, not at the frame. */
    const bool freeze_gate = g_freeze.Poll(hw);
    const bool trig_gate   = g_trig.Poll(hw);
    const bool trig_edge   = g_trig.Take();

    if (size > kBlockSize)
    {
        /* Refuse to run rather than overrun the engine's block arrays. */
        for (size_t i = 0; i < size; i++) { out[0][i] = in[0][i]; out[1][i] = in[1][i]; }
        cpu.OnBlockEnd();
        return;
    }

    /* Parameters, as cv_scaler.cc: the same one-pole coefficients at the
     * same 1 kHz. */
    clouds::Parameters* p = processor.mutable_parameters();
    sm_position += 0.05f * (T.position - sm_position);
    sm_density  += 0.01f * (T.density  - sm_density);
    sm_size     += 0.01f * (T.size     - sm_size);
    sm_texture  += 0.01f * (T.texture  - sm_texture);
    sm_dry_wet  += 0.05f * (T.dry_wet  - sm_dry_wet);
    sm_spread   += 0.05f * (T.spread   - sm_spread);
    sm_feedback += 0.05f * (T.feedback - sm_feedback);
    sm_reverb   += 0.05f * (T.reverb   - sm_reverb);

    p->position      = sm_position;
    p->size          = sm_size;
    p->density       = sm_density;
    p->texture       = sm_texture;
    p->dry_wet       = mi::clampf(sm_dry_wet * 1.05f - 0.025f, 0.f, 1.f);
    p->stereo_spread = sm_spread;
    p->feedback      = sm_feedback;
    p->reverb        = sm_reverb;

    /* Pitch: the knob through Clouds' quantized curve (it is unsmoothed
     * upstream too), plus V/Oct on J5 -- jumps of more than half a
     * semitone land at once, smaller moves glide. */
    p->pitch = stmlib::Interpolate(clouds::lut_quantized_pitch, T.pitch, 1024.0f);
    const float note = 12.0f * hw.cv_jacks[kCvVoct].Volts();
    if (fabsf(note - sm_note) > 0.5f) sm_note = note;
    else sm_note += 0.2f * (note - sm_note);
    p->pitch = mi::clampf(p->pitch + sm_note, -48.0f, 48.0f);

    p->freeze = T.freeze_btn || freeze_gate;
    G_FROZEN  = p->freeze;

    /* Trigger: the jack's edge or B2. Upstream delays the trigger by the
     * ADC latency so the grain lands with the CV that came with it; one
     * block of delay does the same here. */
    const bool seed = trig_edge || T.seed_btn;
    if (T.seed_btn) { T.seed_btn = false; G_SEED_T = System::GetNow(); }
    p->trigger   = s_trig_delay;
    p->gate      = trig_gate;
    s_trig_delay = seed;

    /* float -1..1 -> ShortFrame, with IN GAIN; the input meter follows the
     * scaled input, as Clouds' did. */
    const float gain = T.gain;
    float       pk   = 0.f;
    for (size_t i = 0; i < size; i++)
    {
        const float l = in[0][i] * gain, r = in[1][i] * gain;
        s_in[i].l = mi::f2i(l);
        s_in[i].r = mi::f2i(r);
        const float a = fmaxf(fabsf(l), fabsf(r));
        if (a > pk) pk = a;
    }
    sm_peak   = pk > sm_peak ? pk : sm_peak * 0.97f;
    G_IN_PEAK = sm_peak;

    processor.Process(s_in, s_out, size);

    const float k = 1.0f / 32768.0f;
    for (size_t i = 0; i < size; i++)
    {
        out[0][i] = (float)s_out[i].l * k;
        out[1][i] = (float)s_out[i].r * k;
    }

    cpu.OnBlockEnd();
}

/* ---- boot ------------------------------------------------------------------ */

int main(void)
{
    hw.Init(daisy::SaiHandle::Config::SampleRate::SAI_32KHZ, kBlockSize);
    cpu.Init(hw.SampleRate(), (int)hw.BlockSize());
    if (hw.BlockSize() != kBlockSize) mi::fault_forever(hw);

    /* NOLOAD sections: start from silence, not from whatever was there. */
    memset(block_mem, 0, sizeof block_mem);
    memset(block_ccm, 0, sizeof block_ccm);
    processor.Init(block_mem, sizeof block_mem, block_ccm, sizeof block_ccm);

    g_freeze.Init(hw, kCvFreeze);
    g_trig.Init(hw, kCvTrig);

    sd.Init();
    picker::Install(settings, kSettingsFirmware, sd, hw);
    settings.UseBrightness();
    settings.UsePresets(presets);

    /* CV: J3/J4 are gates and J5 is V/Oct, all read raw in the callback;
     * J6-J8 modulate through the SDK's matrix (re-routable from the web
     * programmer). */
    cv_matrix.Jack(0).Off();
    cv_matrix.Jack(1).Off();
    cv_matrix.Jack(2).Off();
    cv_matrix.Jack(3).To(position);
    cv_matrix.Jack(4).To(size);
    cv_matrix.Jack(5).To(density);

    host.Product("Clouds")
        .BootSlot(home.slot)
        .Pages(play_page, setup_page)
        .Jacks(jk_in_l, jk_in_r, jk_freeze, jk_trig, jk_voct, jk_cv_pos,
               jk_cv_siz, jk_cv_den, jk_out_l, jk_out_r)
        .Buttons(bt_freeze, bt_seed, bt_setup)
        .Attach(manual)
        .Extend(fs_ext);
#ifdef MI_BENCH_USB
    bench_cdc.Init(bench_usb, daisy::UsbHandle::FS_INTERNAL, "Clouds (bench)");
    host.Transport(bench_cdc);
#endif

    presets.Manage(pager);
    presets.Manage(settings);
    presets.Manage(extras);
    presets.Init();
    const bool had_boot = home.Boot(presets);

    float phys[kNumPots];
    for (int i = 0; i < 8; i++)
    {
        hw.ProcessAllControls();
        System::Delay(1);
    }
    for (uint8_t p = 0; p < kNumPots; p++) phys[p] = hw.pots[p].Value();

    if (!had_boot)
    {
        /* First boot after a flash: granular, 16-bit stereo, a little
         * spread and reverb, no feedback, unity gain. Zone i of n sits at
         * (i + 0.5) / n. */
        pager.SetStored(kPageSetup, kPotTopLeft,     0.5f / 4.0f, phys);
        pager.SetStored(kPageSetup, kPotTopRight,    0.5f / 4.0f, phys);
        pager.SetStored(kPageSetup, kPotMiddleLeft,  0.5f, phys);
        pager.SetStored(kPageSetup, kPotMiddleRight, 0.0f, phys);
        pager.SetStored(kPageSetup, kPotBottomLeft,  0.3f, phys);
        pager.SetStored(kPageSetup, kPotBottomRight, 0.5f, phys);

        /* The PLAY page adopts the pots: whatever a pot points at is what
         * the module does. With a saved state each pot must catch its
         * value instead, since the pots are shared by every firmware on
         * the card and point wherever the last one left them. */
        for (uint8_t p = 0; p < kNumPots; p++)
            pager.SetStored(kPagePlay, p, phys[p], phys);
    }

    g_boot_peak     = extras.cpu_peak;
    extras.cpu_peak = 0.0f;
    extras.cpu_avg  = 0.0f;
    g_readout_until = System::GetNow() + 2500u;

    hw.StartAudio(AudioCallback);
    cpu.Reset();

    loop.Use(pager)
        .Use(settings)
        .Use(cv_matrix)
        .Use(play_page)
        .Use(setup_page)
        .Use(host)
        .OnFrame(OnFrame)
        .OnPoll(OnPoll)
        .OnRender(OnRender);

    /* Clouds' main loop called Prepare() continuously: it performs buffer
     * resets on a quality or mode change and, in Spectral mode, the phase
     * vocoder's FFT work. */
    for (;;)
    {
        loop.Tick();
        processor.Prepare();
    }
}
