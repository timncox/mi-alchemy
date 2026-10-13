/*
 * Beads on the Hermetic Modular Alchemy Lab V2 -- Mutable Instruments'
 * texture synthesizer (granular processor, delay, wavetable synth), the
 * last Mutable module to go open source (pichenettes/eurorack d8f4739,
 * 2026-10-12), as Alchemy SDK firmware.
 *
 * The DSP is beads::Processor and beads::Synth, vendored from pichenettes/
 * eurorack with one flag changed (vendor/VENDOR.md): upstream's 48 kHz
 * codec path instead of its 24 kHz one. Beads' own GainControl (auto input
 * gain) runs ahead of it. This file is the host shim: what Beads'
 * cv_reader.cc, ui.cc and beads.cc did, on six pots, three buttons and ten
 * jacks.
 *
 *   PLAY   TIME . SIZE . PITCH . SHAPE . DENSITY . DRY/WET
 *   MOD    TIME~ . SIZE~ . PITCH~ . SHAPE~ . FEEDBACK . REVERB   (hold B3)
 *   SETUP  QUALITY . INPUT . IN GAIN . SEED IN . OUTPUT    (hold B3, then B1)
 *   B1     FREEZE: tap to latch, hold for momentary (J3 ORs in)
 *   B2     SEED: grains while held; unlocks the DENSITY clock; hold 4 s
 *          (or B2+B1) to lock it again
 *   J3 freeze . J4 seed . J5 V/Oct . J6-J8 CV time/size/density
 *   Settings (B2+B3 2 s): brightness, presets; page 1 = the SD picker
 *
 * Beads' four small attenuverters (the "~" knobs) set how much each main
 * knob is randomized, a different flavour either side of noon. See
 * DESIGN.md.
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

#include "beads/dsp/processor.h"
#include "beads/dsp/synth.h"
#include "beads/gain_control.h"
#include "stmlib/dsp/dsp.h"
#include "stmlib/utils/gate_flags.h"

#include "beads_laws.h"
#include "resample_3_2.h"

#ifndef MI_VERSION
#define MI_VERSION "0.0.0"
#endif
#ifndef MI_GIT_HASH
#define MI_GIT_HASH "dev"
#endif

using namespace alchemy;
using daisy::System;

/* ---- geometry ----------------------------------------------------------- */

/* Beads' block rate: beads.cc runs its codec at kBlockRate = 4 kHz blocks,
 * 12 frames at 48 kHz. The SAI stays at 48 kHz for every quality (see
 * resample_3_2.h for the one that wants 32 kHz). */
static constexpr uint32_t kBlockSize = 12u;
static constexpr float    kBlockRate = 4000.0f;
static constexpr uint32_t kBlock32   = kBlockSize * 2u / 3u;   /* 8 */

enum : uint8_t { kPagePlay = 0, kPageMod = 1, kPageSetup = 2, kNumAppPages = 3 };
enum : uint8_t { kSettingsMain = 0, kSettingsFirmware = 1 };

/* CV indices: 0 = J3 ... 5 = J8. */
static constexpr uint8_t kCvFreeze = 0u, kCvSeed = 1u, kCvVoct = 2u;

/* SETUP selectors. QUALITY zones run hi-fi -> lo-fi; Beads' Media enum
 * runs the other way (DIRTY_TAPE = 0 ... DIGITAL = 3). */
enum : int { kInStereo = 0, kInMono = 1, kInSynth = 2 };
enum : int { kSeedAuto = 0, kSeedOff = 1, kSeedJ4 = 2 };

/* ---- hardware + engine -------------------------------------------------- */

static AlchemyLab          hw;
static daisy::CpuLoadMeter cpu;

/* Beads' buffers, sized as beads.cc: 768 KB of delay/recording memory
 * (512 KB in the H7's D1 and 256 KB in D2 upstream) and a 64 KB reverb
 * buffer (D3). The Lab's D1 holds the firmware image, so the delay memory
 * is in SDRAM (cached, write-back); the reverb fits in D2. */
static uint8_t delay_buffer_1[512 * 1024]
    __attribute__((section(".sdram_bss"), aligned(32)));
static uint8_t delay_buffer_2[256 * 1024]
    __attribute__((section(".sdram_bss"), aligned(32)));
static uint16_t MI_D2_BSS reverb_buffer[32768];

static beads::Processor   processor;
static beads::Synth       synth;
static beads::GainControl gain_control;
static mi_beads::Resample32k src32;

/* ---- colours ------------------------------------------------------------ */

static constexpr LedPanel::Rgb kColTime   = {0x40, 0xC0, 0xFF};
static constexpr LedPanel::Rgb kColSize   = {0x60, 0xFF, 0xC0};
static constexpr LedPanel::Rgb kColPitchU = {0xFF, 0x90, 0x20};
static constexpr LedPanel::Rgb kColPitchD = {0x20, 0x90, 0xFF};
static constexpr LedPanel::Rgb kColShape  = {0xFF, 0x60, 0xA0};
static constexpr LedPanel::Rgb kColDensU  = {0xFF, 0xFF, 0x80};
static constexpr LedPanel::Rgb kColDensD  = {0xC0, 0x60, 0xFF};
static constexpr LedPanel::Rgb kColMix    = {0xFF, 0xC0, 0x60};
static constexpr LedPanel::Rgb kColWalk   = {0x40, 0xFF, 0x80};
static constexpr LedPanel::Rgb kColJitter = {0xFF, 0x40, 0xFF};
static constexpr LedPanel::Rgb kColFb     = {0xFF, 0x80, 0x00};
static constexpr LedPanel::Rgb kColVerb   = {0x80, 0x80, 0xFF};
static constexpr LedPanel::Rgb kColQual   = {0xFF, 0x40, 0x40};
static constexpr LedPanel::Rgb kColInput  = {0x00, 0xFF, 0x60};
static constexpr LedPanel::Rgb kColGain   = {0xFF, 0xFF, 0xFF};
static constexpr LedPanel::Rgb kColSeedIn = {0xFF, 0xFF, 0x00};
static constexpr LedPanel::Rgb kColOut    = {0x80, 0xFF, 0xFF};
static constexpr LedPanel::Rgb kColNotch  = {0xFF, 0xFF, 0xFF};
static constexpr LedPanel::Rgb kColDim    = {0x18, 0x18, 0x18};
static constexpr LedPanel::Rgb kColMod    = {0x40, 0x10, 0x40};

static constexpr LedPanel::Rgb kIdle   = {0x00, 0x00, 0x30};
static constexpr LedPanel::Rgb kWhite  = {0xFF, 0xFF, 0xFF};
static constexpr LedPanel::Rgb kAmber  = {0xFF, 0x80, 0x00};
static constexpr LedPanel::Rgb kRed    = {0xFF, 0x00, 0x00};
static constexpr LedPanel::Rgb kManual = {0x30, 0x00, 0x60};

static const char* const kQualLabels[4] = {
    "Hi-fi (48 kHz 16-bit)", "Clouds (32 kHz 12-bit)",
    "Clean lo-fi (24 kHz 12-bit)", "Tape (24 kHz 8-bit, wow)"};
static const char* const kInputLabels[3] = {"Stereo", "Mono (J1)", "Synth (wavetables)"};
static const char* const kSeedLabels[3]  = {"Auto", "Off", "J4"};
static const char* const kOutLabels[2]   = {"Stereo", "Mono + grain gate (J10)"};

/* ---- pages --------------------------------------------------------------- */

/* PLAY. Panel positions, front view:
 *      [P1] B1 [P2]
 *      [P3] B2 [P4]
 *      [P5] B3 [P6]
 * Every knob hands the engine 0..1, the range Beads' pots read in, except
 * PITCH, which goes through Beads' detune law (beads_laws.h). */

static VirtualKnob time_k = VirtualKnob(kPotTopLeft, "Time")
    .Linear(0.f, 1.f).Ident("time")
    .Help("Where the grains read from: just now at the left, the oldest "
          "audio at the right. In delay mode (SIZE fully clockwise) the "
          "delay time, quantized to a clock on J4.")
    .Ring(Level(kColTime));

static VirtualKnob size_k = VirtualKnob(kPotTopRight, "Size")
    .Linear(0.f, 1.f).Ident("size")
    .Help("Grain length. Left of 10 o'clock the grains play backwards; "
          "fully clockwise switches to delay mode.")
    .Ring(Level(kColSize));

static VirtualKnob pitch_k = VirtualKnob(kPotMiddleLeft, "Pitch")
    .Linear(0.f, 1.f).Ident("pitch")
    .Help("Transposition, -2 to +2 octaves, with detents at unison, "
          "semitone, minor and major third, fifth, octave and two octaves "
          "(Beads' pitch curve). J5 adds 1 V/octave.")
    .Ring(Bipolar(kColPitchU, kColPitchD, kColNotch));

static VirtualKnob shape_k = VirtualKnob(kPotMiddleRight, "Shape")
    .Linear(0.f, 1.f).Ident("shape")
    .Help("Grain envelope: square, then smooth, then reversed ramp shapes.")
    .Ring(Level(kColShape));

static VirtualKnob density_k = VirtualKnob(kPotBottomLeft, "Density")
    .Linear(0.f, 1.f).Ident("density")
    .Help("Grain rate. Noon is none; counter-clockwise regular, clockwise "
          "random. With a clock on J4: divisions CCW, probability CW.")
    .Ring(Bipolar(kColDensU, kColDensD, kColNotch));

static VirtualKnob dry_wet_k = VirtualKnob(kPotBottomRight, "Dry/Wet")
    .Linear(0.f, 1.f).Ident("dry_wet")
    .Ring(Level(kColMix));

/* MOD: hold B3. Beads' four attenuverters, beside the knob each one
 * randomizes, and the FEEDBACK and REVERB knobs. */

static VirtualKnob time_amt = VirtualKnob(kPotTopLeft, "Time random")
    .Linear(0.f, 1.f).Ident("time_amount")
    .Help("How much TIME is randomized from grain to grain: clockwise of "
          "noon an even spread, counter-clockwise rarer, bigger jumps. In "
          "delay mode, a slow wander of the tap.")
    .Ring(Bipolar(kColWalk, kColJitter, kColNotch));

static VirtualKnob size_amt = VirtualKnob(kPotTopRight, "Size random")
    .Linear(0.f, 1.f).Ident("size_amount")
    .Help("How much SIZE is randomized from grain to grain: clockwise an "
          "even spread, counter-clockwise rarer, bigger jumps.")
    .Ring(Bipolar(kColWalk, kColJitter, kColNotch));

static VirtualKnob pitch_amt = VirtualKnob(kPotMiddleLeft, "Pitch random")
    .Linear(0.f, 1.f).Ident("pitch_amount")
    .Help("How far PITCH drifts, up to two octaves, as a random walk: "
          "faster clockwise of noon, slower counter-clockwise.")
    .Ring(Bipolar(kColWalk, kColJitter, kColNotch));

static VirtualKnob shape_amt = VirtualKnob(kPotMiddleRight, "Shape random")
    .Linear(0.f, 1.f).Ident("shape_amount")
    .Help("How much SHAPE is randomized from grain to grain: clockwise an "
          "even spread, counter-clockwise rarer, bigger jumps.")
    .Ring(Bipolar(kColWalk, kColJitter, kColNotch));

static VirtualKnob feedback_k = VirtualKnob(kPotBottomLeft, "Feedback")
    .Linear(0.f, 1.f).Ident("feedback")
    .Help("Output back into the recording. In Synth input it picks the "
          "wavetable bank instead.")
    .Ring(Level(kColFb));

static VirtualKnob reverb_k = VirtualKnob(kPotBottomRight, "Reverb")
    .Linear(0.f, 1.f).Ident("reverb")
    .Ring(Level(kColVerb));

/* SETUP: hold B3, then hold B1 as well. Beads' MEDIA button, what its
 * normalization probes and gain buttons decided, and its gate-output mode. */

static VirtualKnob quality_k = VirtualKnob(kPotTopLeft, "Quality")
    .Selector(4).Labels(kQualLabels).Ident("quality")
    .Help("The recording medium. Hi-fi holds 4 s stereo; Clouds is 12-bit "
          "at 32 kHz; Clean lo-fi 12-bit at 24 kHz; Tape 8-bit mu-law with "
          "wow, flutter and soft clipping. Changing it clears the buffer, "
          "so it waits while frozen.")
    .Ring(SelectorRing(kColQual, kColDim, 4));

static VirtualKnob input_k = VirtualKnob(kPotTopRight, "Input")
    .Selector(3).Labels(kInputLabels).Ident("input")
    .Help("Stereo J1/J2; Mono (J1 only, which doubles the recording "
          "time); or Synth: no input, the grains play Beads' wavetables "
          "(FEEDBACK picks the bank).")
    .Ring(SelectorRing(kColInput, kColDim, 3));

static VirtualKnob in_gain_k = VirtualKnob(kPotMiddleLeft, "In gain")
    .Linear(0.f, 1.f).Ident("in_gain")
    .Help("Fully counter-clockwise: automatic (Beads learns the level for "
          "5 s after a change). Anywhere else: manual, x1 to x40.")
    .Ring(Level(kColGain));

static VirtualKnob seed_in_k = VirtualKnob(kPotMiddleRight, "Seed in")
    .Selector(3).Labels(kSeedLabels).Ident("seed_in")
    .Help("What J4 does. J4: a clock or gates there seed the grains and "
          "DENSITY divides or thins them. Auto: J4 counts as patched while "
          "it has had a pulse in the last 8 s. Off: J4 ignored.")
    .Ring(SelectorRing(kColSeedIn, kColDim, 3));

static VirtualKnob output_k = VirtualKnob(kPotBottomLeft, "Output")
    .Selector(2).Labels(kOutLabels).Ident("output")
    .Help("Stereo, or Beads' gate-output mode: a mono mix on J9 and a gate "
          "per grain on J10.")
    .Ring(SelectorRing(kColOut, kColDim, 2));

static Page play_page = Page(kPagePlay).Name("Play").Color("#40c0ff")
    .Knobs(time_k, size_k, pitch_k, shape_k, density_k, dry_wet_k);

static Page mod_page = Page(kPageMod).Name("Mod").Color("#ff40ff")
    .Knobs(time_amt, size_amt, pitch_amt, shape_amt, feedback_k, reverb_k);

static Page setup_page = Page(kPageSetup).Name("Setup").Color("#00ff60")
    .Knobs(quality_k, input_k, in_gain_k, seed_in_k, output_k);

/* ---- descriptor metadata -------------------------------------------------- */

static Jack jk_in_l   ("J1",  "In L",         JackSig::AudioIn);
static Jack jk_in_r   ("J2",  "In R",         JackSig::AudioIn);
static Jack jk_freeze ("J3",  "Freeze",       JackSig::Trig);
static Jack jk_seed   ("J4",  "Seed",         JackSig::Trig);
static Jack jk_voct   ("J5",  "V/Oct",        JackSig::CvBi);
static Jack jk_cv_time("J6",  "CV Time",      JackSig::CvBi);
static Jack jk_cv_size("J7",  "CV Size",      JackSig::CvBi);
static Jack jk_cv_dens("J8",  "CV Density",   JackSig::CvBi);
static Jack jk_out_l  ("J9",  "Out L",        JackSig::AudioOut);
static Jack jk_out_r  ("J10", "Out R / gate", JackSig::AudioOut);

static VirtualButton bt_freeze = VirtualButton("b1", "Freeze")
    .Action("Tap", "Latch freeze on / off")
    .Action("Hold", "Freeze while held")
    .Action("Hold B3 + B1", "Show the Setup page");

static VirtualButton bt_seed = VirtualButton("b2", "Seed")
    .Action("Hold", "Grains while held (unlocks the DENSITY clock)")
    .Action("Hold 4 s", "Lock: DENSITY runs the grains again")
    .Action("B2 + B1", "Lock, at once");

static VirtualButton bt_mod = VirtualButton("b3", "Mod")
    .Action("Hold", "Show the Mod page (randomness, feedback, reverb)");

static Manual manual = Manual()
    .Tagline("Texture synthesizer: granular, delay, wavetables")
    .Preamble(
        "**Beads** records J1/J2 and plays the recording back as grains. "
        "TIME picks where in the recording, SIZE how long each grain is "
        "(backwards at the left; fully right is delay mode), PITCH "
        "transposes (V/Oct on J5), SHAPE sets the envelope, DENSITY how "
        "many (noon is none), DRY/WET mixes. Hold B3 for the Mod page: how "
        "much each of TIME, SIZE, PITCH and SHAPE is randomized, plus "
        "FEEDBACK and REVERB. Hold B3 and then B1 for Setup: QUALITY (the "
        "recording medium), INPUT (stereo, mono, or the wavetable synth), "
        "IN GAIN (auto or manual), what J4 does, and the gate-output mode. "
        "B1 freezes (J3 too). B2 makes grains while held, taking over "
        "from DENSITY; hold it 4 s, or press B1 while holding it, to hand "
        "them back. The DSP is Mutable Instruments' Beads (Emilie Gillet, "
        "MIT), run on upstream's 48 kHz path.");

/* ---- surfaces ------------------------------------------------------------ */

static ControlLoop loop(hw);
/* B1 reaches SETUP only from MOD (B3 held), so from PLAY it stays FREEZE;
 * B2+B3 stays free for the Settings chord. */
static Pager pager = Pager(kNumAppPages, kNumPots)
                         .Shift(hw.buttons[kButtonB3], kPageMod)
                         .Shift(hw.buttons[kButtonB1], kPageSetup).From(kPageMod);

/* Home slot: PROVISIONAL. Slot 1 was the last free one, but Hypjolin
 * (hypjolin-alchemy) took it the same day, so the card is full and the two
 * would overwrite each other's saved state. Which slot Beads gets is Tim's
 * call (memory reference_alchemy_lab_slots.md). */
static mi::HomeSlot home(1u);
static Presets      presets(hw.seed.qspi);
static Settings     settings(hw, &pager);
static CvMatrix     cv_matrix(kNumCvInputs);
static SdCard       sd;
static hostlink::FsExtension fs_ext(sd);
static hostlink::Host host(presets, "beads_alchemy", "Beads",
                           MI_VERSION, MI_GIT_HASH);
static mi::CpuExtras extras(0x424544u);   /* 'BED' */

#ifdef MI_BENCH_USB
static daisy::UsbHandle          bench_usb;
static hostlink::CdcUsbTransport bench_cdc;
#endif

/* ---- control frame -> audio block ---------------------------------------- */

/*
 * The control thread writes knob targets here at the frame rate; the audio
 * callback smooths toward them once per 4 kHz block, as Beads' pot channels
 * did (cv_reader.cc). Single floats, ints and bools: a torn read is
 * impossible on the M7.
 */
struct Targets
{
    volatile float time = 0.5f, size = 0.5f, pitch = 0.f, shape = 0.5f;
    volatile float density = 0.5f, dry_wet = 0.5f;
    volatile float time_amt = 0.f, size_amt = 0.f, pitch_amt = 0.f, shape_amt = 0.f;
    volatile float feedback = 0.f, reverb = 0.f;
    volatile int   media = beads::Processor::MEDIA_DIGITAL;
    volatile int   input = kInStereo;
    volatile int   seed_in = kSeedAuto;
    volatile bool  gate_out = false;
    volatile uint8_t gain = 0;   /* 0 = auto, else Beads' manual 1..255 */
    volatile bool  freeze_btn = false;
    volatile bool  seed_btn = false;
    volatile bool  seed_locked = true;
};
static Targets T;

static mi::Gate g_freeze, g_seed;

/* Read back by the LEDs. */
static volatile bool     G_FROZEN  = false;
static volatile float    G_IN_PEAK = 0.0f;
static volatile bool     G_PENDING = false;   /* a QUALITY/INPUT change waits */

/* ---- buttons (1 ms poll) -------------------------------------------------- */

static constexpr uint32_t kSeedLockMs = 4000u;   /* ui.cc long_press_duration */

static mi::Toggle  tg_freeze;
static mi::Swallow swallow;
static bool        g_b2_down = false, g_b2_chorded = false;
static bool        g_lock_at_press = true;
static uint32_t    g_b2_since = 0;

static void OnPoll(uint32_t now)
{
    if (settings.IsActive())
    {
        /* Settings owns the buttons; drop momentaries, keep latches. */
        tg_freeze.Reset();
        T.freeze_btn = tg_freeze.latch;
        T.seed_btn   = false;
        g_b2_down    = true;   /* the press that leaves Settings does nothing */
        g_b2_chorded = true;
        swallow.All();
        return;
    }

    /* While B3 holds the Mod page, B1 is the Setup shift, not FREEZE, and
     * B2 stands down (B2+B3 held 2 s is the Settings chord). Either one
     * held across B3 stays dead until it is released. */
    const bool b3 = swallow.Live(hw, kButtonB3);
    if (b3)
    {
        if (hw.buttons[kButtonB1].Pressed()) swallow.mask |= (uint8_t)(1u << kButtonB1);
        if (hw.buttons[kButtonB2].Pressed())
        {
            /* B2 was the start of the Settings chord (or of a trip to the
             * Mod page), not a seed: undo the unlock its press made, or
             * DENSITY would stop making grains with nothing on the panel
             * to say why. */
            if (g_b2_down && !g_b2_chorded) T.seed_locked = g_lock_at_press;
            g_b2_chorded = true;
            swallow.mask |= (uint8_t)(1u << kButtonB2);
        }
    }
    const bool b2 = swallow.Live(hw, kButtonB2);
    const bool b1 = swallow.Live(hw, kButtonB1);

    /* B2 + B1 = seed lock (ui.cc: SEED held, FREEZE pressed). B1's press
     * is then not a freeze. */
    if (b2 && b1 && !g_b2_chorded)
    {
        g_b2_chorded  = true;
        T.seed_locked = true;
        T.seed_btn    = false;
        swallow.All();
        tg_freeze.Reset();
        T.freeze_btn = tg_freeze.latch;
        return;
    }
    if (!g_b2_chorded) T.freeze_btn = tg_freeze.Poll(b1, now);

    if (b2 && !g_b2_down)
    {
        /* ui.cc OnSwitchPressed: a press unlocks, and seeds while held. */
        g_lock_at_press = T.seed_locked;
        g_b2_since    = now;
        g_b2_chorded  = false;
        T.seed_locked = false;
    }
    if (!b2 && g_b2_down && !g_b2_chorded && now - g_b2_since >= kSeedLockMs)
        T.seed_locked = true;   /* OnSwitchReleased: a long hold locks */
    if (!b2 && !b1) g_b2_chorded = false;
    T.seed_btn = b2 && !g_b2_chorded;
    g_b2_down  = b2;
}

/* ---- control frame (~60 Hz) ------------------------------------------------ */

static mi::StoredWatch<kNumPots> play_watch, mod_watch, setup_watch;
static float g_saved_peak  = 0.0f;
static bool  prev_settings = false;

static void OnFrame(void)
{
    const uint32_t now = System::GetNow();

    if (play_watch.Changed(pager, kPagePlay)) home.Touch(now);
    if (mod_watch.Changed(pager, kPageMod)) home.Touch(now);
    if (setup_watch.Changed(pager, kPageSetup)) home.Touch(now);

    /* Values are catch + locks + CV, already mixed by the SDK. */
    T.time      = mi::clampf(time_k.Value(), 0.f, 1.f);
    T.size      = mi::clampf(size_k.Value(), 0.f, 1.f);
    T.pitch     = mi_beads::DetuneLaw(mi::clampf(pitch_k.Value(), 0.f, 1.f));
    T.shape     = mi::clampf(shape_k.Value(), 0.f, 1.f);
    T.density   = mi::clampf(density_k.Value(), 0.f, 1.f);
    T.dry_wet   = mi::clampf(dry_wet_k.Value(), 0.f, 1.f);
    T.time_amt  = mi_beads::AttenuverterLaw(mi::clampf(time_amt.Value(), 0.f, 1.f));
    T.size_amt  = mi_beads::AttenuverterLaw(mi::clampf(size_amt.Value(), 0.f, 1.f));
    T.pitch_amt = mi_beads::AttenuverterLaw(mi::clampf(pitch_amt.Value(), 0.f, 1.f));
    T.shape_amt = mi_beads::AttenuverterLaw(mi::clampf(shape_amt.Value(), 0.f, 1.f));
    T.feedback  = mi::clampf(feedback_k.Value(), 0.f, 1.f);
    T.reverb    = mi::clampf(reverb_k.Value(), 0.f, 1.f);

    const int q = (int)quality_k.Value();
    if (q >= 0 && q < 4) T.media = beads::Processor::MEDIA_DIGITAL - q;
    const int in = (int)input_k.Value();
    if (in >= 0 && in < 3) T.input = in;
    const int si = (int)seed_in_k.Value();
    if (si >= 0 && si < 3) T.seed_in = si;
    T.gate_out = (int)output_k.Value() == 1;
    T.gain     = mi_beads::GainSetting(mi::clampf(in_gain_k.Value(), 0.f, 1.f));

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
 *   B2 pair   seed: the input meter (green, amber, red at clip) while
 *             DENSITY runs the grains (locked); violet when B2 has taken
 *             over, white while held; red when the audio callback averages
 *             over 80 %
 *   B3 pair   dim Mod tint; amber while a QUALITY or INPUT change waits
 *             for FREEZE to let go
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
    if (cpu.GetAvgCpuLoad() > 0.80f) b2 = kRed;
    else if (T.seed_btn)             b2 = kWhite;
    else if (!T.seed_locked)         b2 = kManual;
    else if (pk > 0.98f)             b2 = kRed;
    else if (pk > 0.70f)             b2 = kAmber;
    else
    {
        const uint8_t g = (uint8_t)(0x10 + 0xE0 * mi::clampf(pk / 0.70f, 0.f, 1.f));
        b2 = {0x00, g, 0x00};
    }
    L.SetButtonPair(kButtonB2, L.ScaleGlobal(b2));

    if (pager.Page() == kPagePlay)
        L.SetButtonPair(kButtonB3, L.ScaleGlobal(G_PENDING ? kAmber : kColMod));
}

/* ---- audio ----------------------------------------------------------------- */

using beads::FloatFrame;
static_assert(sizeof(FloatFrame) == sizeof(mi_beads::StereoF), "frame layout");

/* beads.cc's state, carried between blocks. */
static float sm_time = 0.5f, sm_size = 0.5f, sm_pitch = 0.f, sm_shape = 0.5f;
static float sm_density = 0.5f, sm_dry_wet = 0.5f;
static float sm_time_amt = 0.f, sm_size_amt = 0.f, sm_pitch_amt = 0.f, sm_shape_amt = 0.f;
static float sm_feedback = 0.f, sm_reverb = 0.f;
static float sm_peak = 0.f;
static int   s_transition = 0;          /* blocks of silence after a change */
static bool  s_synth = false;
static stmlib::GateFlags s_prev_seed = stmlib::GATE_FLAG_LOW;
static uint32_t s_since_seed_edge = 0xFFFFFFFFu;   /* blocks */

/* Smoothing per 4 kHz block. Beads' pots run a 0.05 one-pole at this rate
 * on a 16-bit ADC; the SDK hands over ~60 Hz steps, so 0.01 (a 25 ms time
 * constant) rounds them off. */
static constexpr float kSmooth = 0.01f;

static void Reconfigure(int media, int input)
{
    const bool synth_mode = input == kInSynth;
    const bool mono       = input == kInMono;
    /* beads.cc resets the AGC when mono flips or synth mode ends; decided
     * before Configure() overwrites processor.mono(). */
    const bool reset_gain = mono != processor.mono() || synth_mode != s_synth;
    /* beads.cc reconfigures only the processor; the synth keeps its state
     * (Synth::Reset() is declared upstream but never defined). */
    if (!synth_mode)
        processor.Configure(static_cast<beads::Processor::Media>(media), mono);
    /* beads.cc ConfigureAndStartCodec: soft clipping by medium; the codec
     * itself stays at 48 kHz here. */
    gain_control.Init(
        48000.0f,
        media == beads::Processor::MEDIA_DIRTY_TAPE
            ? beads::GainControl::SOFT_CLIPPING_MODE_ON
            : (media == beads::Processor::MEDIA_CLEAN_TAPE
                   ? beads::GainControl::SOFT_CLIPPING_MODE_DYNAMIC
                   : beads::GainControl::SOFT_CLIPPING_MODE_OFF),
        reset_gain);
    src32.Reset();
    s_synth      = synth_mode;
    s_transition = (int)(kBlockRate / 4.0f);   /* codec_transition_frames */
}

static beads::Codec::Frame s_in[kBlockSize];
static FloatFrame          f_in[kBlockSize], f_out[kBlockSize];
static FloatFrame          e_in[kBlock32], e_out[kBlock32];
static int                 s_cfg_media = -1, s_cfg_input = -1;

static void AudioCallback(daisy::AudioHandle::InputBuffer  in,
                          daisy::AudioHandle::OutputBuffer out,
                          size_t                           size)
{
    cpu.OnBlockStart();

    const bool freeze_gate = g_freeze.Poll(hw);
    const bool seed_gate   = g_seed.Poll(hw);
    if (g_seed.Take()) s_since_seed_edge = 0;
    else if (s_since_seed_edge != 0xFFFFFFFFu) s_since_seed_edge++;

    if (size != kBlockSize)
    {
        /* Refuse to run rather than overrun the engine's block arrays. */
        for (size_t i = 0; i < size; i++) { out[0][i] = in[0][i]; out[1][i] = in[1][i]; }
        cpu.OnBlockEnd();
        return;
    }

    const bool frozen = T.freeze_btn || freeze_gate;
    G_FROZEN = frozen;

    /* MonitorStateChange(): a new medium or input reconfigures the
     * processor (clearing its buffer), but never while frozen. */
    const int media = T.media, input = T.input;
    const bool changed = media != s_cfg_media || input != s_cfg_input;
    G_PENDING = frozen && changed && s_cfg_media >= 0;
    if (changed && (!frozen || s_cfg_media < 0))   /* boot configures anyway */
    {
        Reconfigure(media, input);
        s_cfg_media = media;
        s_cfg_input = input;
    }

    /* Pots, as cv_reader.cc + beads.cc Process(). */
    sm_time      += kSmooth * (T.time      - sm_time);
    sm_size      += kSmooth * (T.size      - sm_size);
    sm_pitch     += kSmooth * (T.pitch     - sm_pitch);
    sm_shape     += kSmooth * (T.shape     - sm_shape);
    sm_density   += kSmooth * (T.density   - sm_density);
    sm_dry_wet   += kSmooth * (T.dry_wet   - sm_dry_wet);
    sm_time_amt  += kSmooth * (T.time_amt  - sm_time_amt);
    sm_size_amt  += kSmooth * (T.size_amt  - sm_size_amt);
    sm_pitch_amt += kSmooth * (T.pitch_amt - sm_pitch_amt);
    sm_shape_amt += kSmooth * (T.shape_amt - sm_shape_amt);
    sm_feedback  += kSmooth * (T.feedback  - sm_feedback);
    sm_reverb    += kSmooth * (T.reverb    - sm_reverb);

    /* No normalization probes on the Lab: every RandomizedParameter is
     * "unpatched" (has_cv false), so its attenuverter is a randomness
     * amount, as on an unpatched Beads. CV reaches TIME / SIZE / DENSITY
     * through the SDK's matrix (it moves the knob), and V/Oct is added to
     * PITCH's base the way Synth::Render folds a patched pitch CV in. */
    beads::Parameters p;
    p.time  = {sm_time,  sm_time_amt,  0.f, false};
    p.size  = {sm_size,  sm_size_amt,  0.f, false};
    p.shape = {sm_shape, sm_shape_amt, 0.f, false};
    const float voct = mi::clampf(12.0f * hw.cv_jacks[kCvVoct].Volts(), -60.f, 60.f);
    p.pitch = {sm_pitch + voct, sm_pitch_amt, 0.f, false};
    p.monophonic_mode = false;   /* ENABLE_ADVANCED_FEATURES is off upstream */

    beads::ModulatedParameter d;
    d.base = sm_density;
    d.cv   = 0.0f;

    beads::Processor::MixParameters m;
    m.feedback      = mi::clampf(sm_feedback * 1.005f, 0.f, 1.f);
    m.dry_wet       = mi::clampf(sm_dry_wet * 1.005f, 0.f, 1.f);
    m.reverb_amount = mi::clampf(sm_reverb * 1.005f, 0.f, 1.f);
    m.output_gate   = T.gate_out;

    /* Freeze, and the seed input as per-sample gate flags (read once per
     * block; 0.25 ms). Auto counts J4 as patched while it has had an edge
     * in the last 8 s. */
    bool freeze[kBlockSize];
    for (size_t i = 0; i < size; i++) freeze[i] = frozen;

    const int  seed_in = T.seed_in;
    const bool j4 = seed_in == kSeedJ4
                    || (seed_in == kSeedAuto
                        && s_since_seed_edge < (uint32_t)(8.0f * kBlockRate));
    stmlib::GateFlags  seed[kBlockSize];
    stmlib::GateFlags* seed_input = nullptr;
    if (j4)
    {
        for (size_t i = 0; i < size; i++)
            s_prev_seed = seed[i] = stmlib::ExtractGateFlags(s_prev_seed, seed_gate);
        seed_input = seed;
    }
    else
    {
        s_prev_seed = stmlib::GATE_FLAG_LOW;
    }

    const beads::SeedButtonState seed_state =
        T.seed_locked ? beads::SEED_BUTTON_STATE_LOCKED
                      : (T.seed_btn ? beads::SEED_BUTTON_STATE_ON
                                    : beads::SEED_BUTTON_STATE_OFF);

    /* Input: float -> the codec's 32-bit frames -> GainControl (auto or
     * manual gain, soft clipping by medium, mono copy). The meter follows
     * the gained input, as Beads' VU did. */
    const bool mono = s_cfg_input == kInMono;
    for (size_t i = 0; i < size; i++)
    {
        s_in[i].l = (int32_t)(mi::clampf(in[0][i], -1.f, 0.9999999f) * 2147483648.0f);
        s_in[i].r = (int32_t)(mi::clampf(in[1][i], -1.f, 0.9999999f) * 2147483648.0f);
    }
    gain_control.Process(s_transition == 0, T.gain, s_synth, s_synth || mono,
                         s_in, f_in, size);
    float pk = 0.f;
    for (size_t i = 0; i < size; i++)
    {
        const float a = fmaxf(fabsf(f_in[i].l), fabsf(f_in[i].r));
        if (a > pk) pk = a;
    }
    sm_peak   = pk > sm_peak ? pk : sm_peak * 0.99f;
    G_IN_PEAK = sm_peak;

    if (s_transition > 0)
    {
        --s_transition;
        for (size_t i = 0; i < size; i++) out[0][i] = out[1][i] = 0.f;
        cpu.OnBlockEnd();
        return;
    }

    if (s_synth)
    {
        synth.Render(seed_state, seed_input, freeze,
                     static_cast<beads::Synth::Resolution>(s_cfg_media),
                     d, p, m, f_out, size);
    }
    else if (processor.sample_rate() == 32000.0f)
    {
        /* The Clouds quality: run the engine at its 32 kHz, 8 frames per
         * block. Gates and freeze go at 32 kHz too, from the same block. */
        const size_t n = src32.Down(reinterpret_cast<const mi_beads::StereoF*>(f_in),
                                    reinterpret_cast<mi_beads::StereoF*>(e_in), size);
        processor.Process(seed_state, seed_input, freeze, d, p, m, e_in, e_out, n);
        src32.Up(reinterpret_cast<const mi_beads::StereoF*>(e_out),
                 reinterpret_cast<mi_beads::StereoF*>(f_out), n);
        if (T.gate_out)
        {
            /* The gate is a logic level, not audio: hold, don't filter. */
            for (size_t i = 0; i < size; i++) f_out[i].r = e_out[i * 2u / 3u].r;
        }
    }
    else
    {
        processor.Process(seed_state, seed_input, freeze, d, p, m, f_in, f_out, size);
    }

    for (size_t i = 0; i < size; i++)
    {
        out[0][i] = mi::clampf(f_out[i].l, -1.0f, 0.999999f);
        /* Beads' codec inverts; it writes -0.8 for a high gate. The Lab's
         * J10 is not inverted, so a high gate is +0.8 here. */
        out[1][i] = T.gate_out ? (f_out[i].r != 0.f ? 0.8f : 0.f)
                               : mi::clampf(f_out[i].r, -1.0f, 0.999999f);
    }

    cpu.OnBlockEnd();
}

/* ---- boot ------------------------------------------------------------------ */

int main(void)
{
    hw.Init(daisy::SaiHandle::Config::SampleRate::SAI_48KHZ, kBlockSize);
    cpu.Init(hw.SampleRate(), (int)hw.BlockSize());
    if (hw.BlockSize() != kBlockSize) mi::fault_forever(hw);

    /* NOLOAD sections: start from silence, not from whatever was there. */
    memset(delay_buffer_1, 0, sizeof delay_buffer_1);
    memset(delay_buffer_2, 0, sizeof delay_buffer_2);
    memset(reverb_buffer, 0, sizeof reverb_buffer);

    /* beads.cc Init(): three views of the 768 KB, as upstream. */
    uint8_t* delay_buffers[3] = {delay_buffer_1, delay_buffer_1 + 256 * 1024,
                                 delay_buffer_2};
    processor.Init(delay_buffers, reverb_buffer);
    synth.Init(reverb_buffer);
    src32.Init();
    gain_control.Init(48000.0f, beads::GainControl::SOFT_CLIPPING_MODE_OFF, true);

    g_freeze.Init(hw, kCvFreeze);
    g_seed.Init(hw, kCvSeed);

    sd.Init();
    picker::Install(settings, kSettingsFirmware, sd, hw);
    settings.UseBrightness();
    settings.UsePresets(presets);

    /* CV: J3/J4 are gates and J5 is V/Oct, all read raw in the callback;
     * J6-J8 modulate through the SDK's matrix (re-routable in the web
     * programmer, e.g. to SHAPE or a randomness amount). */
    cv_matrix.Jack(0).Off();
    cv_matrix.Jack(1).Off();
    cv_matrix.Jack(2).Off();
    cv_matrix.Jack(3).To(time_k);
    cv_matrix.Jack(4).To(size_k);
    cv_matrix.Jack(5).To(density_k);

    host.Product("Beads")
        .BootSlot(home.slot)
        .Pages(play_page, mod_page, setup_page)
        .Jacks(jk_in_l, jk_in_r, jk_freeze, jk_seed, jk_voct, jk_cv_time,
               jk_cv_size, jk_cv_dens, jk_out_l, jk_out_r)
        .Buttons(bt_freeze, bt_seed, bt_mod)
        .Attach(manual)
        .Extend(fs_ext);
#ifdef MI_BENCH_USB
    bench_cdc.Init(bench_usb, daisy::UsbHandle::FS_INTERNAL, "Beads (bench)");
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
        /* First boot after a flash, as a fresh Beads (settings.cc): hi-fi,
         * auto gain, stereo, no randomness, a little reverb. Zone i of n
         * sits at (i + 0.5) / n. */
        pager.SetStored(kPageMod, kPotTopLeft,     0.5f, phys);
        pager.SetStored(kPageMod, kPotTopRight,    0.5f, phys);
        pager.SetStored(kPageMod, kPotMiddleLeft,  0.5f, phys);
        pager.SetStored(kPageMod, kPotMiddleRight, 0.5f, phys);
        pager.SetStored(kPageMod, kPotBottomLeft,  0.0f, phys);
        pager.SetStored(kPageMod, kPotBottomRight, 0.3f, phys);

        pager.SetStored(kPageSetup, kPotTopLeft,     0.5f / 4.0f, phys);
        pager.SetStored(kPageSetup, kPotTopRight,    0.5f / 3.0f, phys);
        pager.SetStored(kPageSetup, kPotMiddleLeft,  0.0f, phys);
        pager.SetStored(kPageSetup, kPotMiddleRight, 0.5f / 3.0f, phys);
        pager.SetStored(kPageSetup, kPotBottomLeft,  0.5f / 2.0f, phys);

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
        .Use(mod_page)
        .Use(setup_page)
        .Use(host)
        .OnFrame(OnFrame)
        .OnPoll(OnPoll)
        .OnRender(OnRender);

    for (;;) loop.Tick();
}
