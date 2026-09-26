/*
 * Plaits on the Hermetic Modular Alchemy Lab V2 -- Mutable Instruments'
 * macro-oscillator (24 synthesis models in three banks, with its low-pass
 * gate and internal decay envelope) as Alchemy SDK firmware.
 *
 * The DSP is plaits::Voice, vendored byte-identical from pichenettes/eurorack
 * (vendor/VENDOR.md) apart from one shimmed header (src/shim/plaits/
 * user_data.h: no user wavetables), run at its native 48 kHz and rendered in
 * its native 12-frame calls. This file is the host shim: it does what
 * Plaits' ui.cc and settings.cc did, on six pots, three buttons and ten
 * jacks. See src/plaits/DESIGN.md.
 *
 *   PLAY   MODEL . FREQUENCY . HARMONICS . TIMBRE . MORPH . DECAY
 *   SETUP  OCTAVE . FINE . FM ATT . TIMBRE ATT . MORPH ATT . LPG COLOUR (B3)
 *   B1     STRIKE: a trigger by hand, held as a gate
 *   B2     BANK: jump MODEL to the same model in the next bank
 *   J3 trigger . J4 level . J5 V/Oct . J6 timbre . J7 morph . J8 harmonics
 *   J9 OUT . J10 AUX
 *   Settings (B2+B3 2 s): brightness, presets, TRIGGER mode, LEVEL CV;
 *   page 1 = the SD picker
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

#include "plaits/dsp/dsp.h"
#include "plaits/dsp/voice.h"
#include "stmlib/dsp/hysteresis_quantizer.h"
#include "stmlib/utils/buffer_allocator.h"

#ifndef MI_VERSION
#define MI_VERSION "0.0.0"
#endif
#ifndef MI_GIT_HASH
#define MI_GIT_HASH "dev"
#endif

using namespace alchemy;
using daisy::System;

/* ---- geometry ----------------------------------------------------------- */

/* The Voice's envelope and LPG coefficients assume plaits::kBlockSize (12)
 * frames per Render(), so a 24-frame codec block is rendered as two calls:
 * the parameters below are read at 2 kHz, the gates polled every 0.5 ms. */
static constexpr uint32_t kRender    = plaits::kBlockSize;
static constexpr uint32_t kBlockSize = 2u * kRender;

enum : uint8_t { kPagePlay = 0, kPageSetup = 1, kNumAppPages = 2 };
enum : uint8_t { kSettingsMain = 0, kSettingsFirmware = 1 };

/* CV indices: 0 = J3 ... 5 = J8. All six are read raw in the callback. */
static constexpr uint8_t kCvTrig = 0u, kCvLevel = 1u, kCvVoct = 2u;
static constexpr uint8_t kCvTimbre = 3u, kCvMorph = 4u, kCvHarm = 5u;

static constexpr int kNumEngines = 24;

/* ---- hardware + engine -------------------------------------------------- */

static AlchemyLab          hw;
static daisy::CpuLoadMeter cpu;

/* Plaits' 16 KB shared buffer, handed out by BufferAllocator to the engines
 * that need scratch (upstream: plain .bss on the STM32F373). */
static char MI_D2_BSS shared_buffer[16384];

static plaits::Voice voice;

/* ---- colours ------------------------------------------------------------ */

/* The three banks, in Plaits' own LED colours: the 1.2 models orange, the
 * classic pitched models green, the noise / physical / drum models red. */
static constexpr LedPanel::Rgb kBank[3] = {
    {0xFF, 0x70, 0x00}, {0x00, 0xFF, 0x40}, {0xFF, 0x10, 0x10}};

static constexpr LedPanel::Rgb kColFreqU  = {0xFF, 0xC0, 0x60};
static constexpr LedPanel::Rgb kColFreqD  = {0x60, 0xC0, 0xFF};
static constexpr LedPanel::Rgb kColHarm   = {0x60, 0xFF, 0xC0};
static constexpr LedPanel::Rgb kColTimbre = {0xFF, 0x60, 0xA0};
static constexpr LedPanel::Rgb kColMorph  = {0xC0, 0x60, 0xFF};
static constexpr LedPanel::Rgb kColDecay  = {0xFF, 0xFF, 0x80};
static constexpr LedPanel::Rgb kColOct    = {0x40, 0xC0, 0xFF};
static constexpr LedPanel::Rgb kColAttP   = {0xFF, 0x90, 0x20};
static constexpr LedPanel::Rgb kColAttN   = {0x20, 0x90, 0xFF};
static constexpr LedPanel::Rgb kColColour = {0x80, 0xFF, 0xFF};
static constexpr LedPanel::Rgb kColNotch  = {0xFF, 0xFF, 0xFF};
static constexpr LedPanel::Rgb kColDim    = {0x18, 0x18, 0x18};
static constexpr LedPanel::Rgb kColSetup  = {0x10, 0x30, 0x40};

static constexpr LedPanel::Rgb kIdle  = {0x00, 0x00, 0x30};
static constexpr LedPanel::Rgb kWhite = {0xFF, 0xFF, 0xFF};
static constexpr LedPanel::Rgb kRed   = {0xFF, 0x00, 0x00};

static const char* const kModelLabels[kNumEngines] = {
    "VA + VCF", "Phase distortion", "6-op FM 1", "6-op FM 2", "6-op FM 3",
    "Wave terrain", "String machine", "Chiptune",
    "Virtual analog", "Waveshaping", "2-op FM", "Formant / grain",
    "Harmonic", "Wavetable", "Chords", "Speech",
    "Swarm", "Filtered noise", "Particle", "Inharmonic string",
    "Modal resonator", "Bass drum", "Snare drum", "Hi-hat"};

/* Plaits' HARMONICS-hold octave setting, 11 zones (ui.cc): 0 is the
 * sub-audio / LFO range, 1-8 fix the octave with FREQUENCY spanning +-7
 * semitones, 9 quantizes FREQUENCY to octaves, 10 (the default) is the
 * free 8-octave sweep. */
static const char* const kOctLabels[11] = {
    "LFO", "C0", "C1", "C2", "C3", "C4", "C5", "C6", "C7", "Octaves", "Free"};

static const char* const kTrigLabels[3] = {"Auto", "Drone", "Triggered"};
static const char* const kLevelLabels[2] = {"Off", "On"};

/* ---- pages --------------------------------------------------------------- */

/* PLAY. Panel positions, front view:
 *      [P1] B1 [P2]
 *      [P3] B2 [P4]
 *      [P5] B3 [P6]                                                        */

static VirtualKnob model = VirtualKnob(kPotTopLeft, "Model")
    .Selector(kNumEngines).Labels(kModelLabels).Ident("model")
    .Help("The synthesis model: 1-8 the Plaits 1.2 bank, 9-16 the pitched "
          "classics, 17-24 noise, physical models and drums. B2 jumps to "
          "the same model in the next bank.")
    .Ring(SelectorRing(kColNotch, kColDim, kNumEngines));

static VirtualKnob frequency = VirtualKnob(kPotTopRight, "Frequency")
    .Linear(-1.f, 1.f).Ident("frequency")
    .Help("Pitch. Its range is the OCTAVE setting (Setup); J5 adds 1 V/oct.")
    .Ring(Bipolar(kColFreqU, kColFreqD, kColNotch));

static VirtualKnob harmonics = VirtualKnob(kPotMiddleLeft, "Harmonics")
    .Linear(0.f, 1.f).Ident("harmonics")
    .Help("Per model; for most, the spectral content or the chord.")
    .Ring(Level(kColHarm));

static VirtualKnob timbre = VirtualKnob(kPotMiddleRight, "Timbre")
    .Linear(0.f, 1.f).Ident("timbre")
    .Ring(Level(kColTimbre));

static VirtualKnob morph = VirtualKnob(kPotBottomLeft, "Morph")
    .Linear(0.f, 1.f).Ident("morph")
    .Ring(Level(kColMorph));

static VirtualKnob decay = VirtualKnob(kPotBottomRight, "Decay")
    .Linear(0.f, 1.f).Ident("decay")
    .Help("Decay of the low-pass gate and of the internal envelope, once "
          "the module is triggered (Plaits' MORPH-hold setting).")
    .Ring(Level(kColDecay));

/* SETUP: hold B3. The three attenuverters of the panel, and the hidden
 * settings Plaits reached by holding a button and turning a knob. */

static VirtualKnob octave = VirtualKnob(kPotTopLeft, "Octave")
    .Selector(11).Labels(kOctLabels).Ident("octave")
    .Ring(SelectorRing(kColOct, kColDim, 11));

static VirtualKnob fine = VirtualKnob(kPotTopRight, "Fine")
    .Linear(-1.f, 1.f).Ident("fine").Unit("st")
    .Help("Fine tune, +-1 semitone, in every OCTAVE range.")
    .Ring(Bipolar(kColFreqU, kColFreqD, kColNotch));

static VirtualKnob fm_att = VirtualKnob(kPotMiddleLeft, "FM")
    .Linear(-1.f, 1.f).Ident("fm_amount")
    .Help("Frequency modulation from the internal envelope once triggered "
          "(this port has no FM jack); on Speech, prosody.")
    .Ring(Bipolar(kColAttP, kColAttN, kColNotch));

static VirtualKnob timbre_att = VirtualKnob(kPotMiddleRight, "Timbre att")
    .Linear(-1.f, 1.f).Ident("timbre_amount")
    .Help("Attenuverter: scales J6 when a CV is present there, otherwise the "
          "internal envelope's pull on TIMBRE.")
    .Ring(Bipolar(kColAttP, kColAttN, kColNotch));

static VirtualKnob morph_att = VirtualKnob(kPotBottomLeft, "Morph att")
    .Linear(-1.f, 1.f).Ident("morph_amount")
    .Help("Attenuverter: J7, or the internal envelope's pull on MORPH; on "
          "Speech, the word speed.")
    .Ring(Bipolar(kColAttP, kColAttN, kColNotch));

static VirtualKnob lpg_colour = VirtualKnob(kPotBottomRight, "LPG colour")
    .Linear(0.f, 1.f).Ident("lpg_colour")
    .Help("The low-pass gate: VCFA (bright) at the left, VCA at the right.")
    .Ring(Level(kColColour));

static Page play_page = Page(kPagePlay).Name("Play").Color("#00ff40")
    .Knobs(model, frequency, harmonics, timbre, morph, decay);

static Page setup_page = Page(kPageSetup).Name("Setup").Color("#40c0ff")
    .Knobs(octave, fine, fm_att, timbre_att, morph_att, lpg_colour);

/* ---- descriptor metadata -------------------------------------------------- */

static Jack jk_in_l   ("J1",  "(unused)",  JackSig::AudioIn);
static Jack jk_in_r   ("J2",  "(unused)",  JackSig::AudioIn);
static Jack jk_trig   ("J3",  "Trigger",   JackSig::Gate);
static Jack jk_level  ("J4",  "Level",     JackSig::CvUni);
static Jack jk_voct   ("J5",  "V/Oct",     JackSig::Voct);
static Jack jk_timbre ("J6",  "Timbre",    JackSig::CvBi);
static Jack jk_morph  ("J7",  "Morph",     JackSig::CvBi);
static Jack jk_harm   ("J8",  "Harmonics", JackSig::CvBi);
static Jack jk_out    ("J9",  "Out",       JackSig::AudioOut);
static Jack jk_aux    ("J10", "Aux",       JackSig::AudioOut);

static VirtualButton bt_strike = VirtualButton("b1", "Strike")
    .Action("Press", "Trigger the model and its envelope; held, it is the gate");

static VirtualButton bt_bank = VirtualButton("b2", "Bank")
    .Action("Tap", "Same model, next bank (1.2 -> classic -> noise/drums)");

static VirtualButton bt_setup = VirtualButton("b3", "Setup")
    .Action("Hold", "Show the Setup page");

static Manual manual = Manual()
    .Tagline("Macro-oscillator: 24 models, low-pass gate, decay envelope")
    .Preamble(
        "**Plaits** is 24 synthesis models behind four controls. MODEL "
        "picks one (the ring's bright dot; B2 jumps banks, and the B2 LEDs "
        "show the bank: orange the 1.2 models, green the classics, red "
        "noise and drums). FREQUENCY is pitch (V/Oct on J5), HARMONICS, "
        "TIMBRE and MORPH shape the sound, and J6-J8 modulate them. Hold "
        "B3 for Setup: the OCTAVE range and FINE tune, the three "
        "attenuverters of the original panel, and the LPG COLOUR. Once a "
        "trigger arrives on J3 (or B1 is pressed) the low-pass gate and "
        "the decay envelope take over, set by DECAY; untriggered, the "
        "model drones. Out on J9, the model's aux output on J10. The DSP "
        "is Mutable Instruments' Plaits (Emilie Gillet, MIT), unmodified.");

/* ---- surfaces ------------------------------------------------------------ */

static ControlLoop loop(hw);
static Pager       pager = Pager(kNumAppPages, kNumPots)
                               .Shift(hw.buttons[kButtonB3], kPageSetup);

/* Home slot 11: see mi_family.h for the whole card's map. */
static mi::HomeSlot home(11u);
static Presets      presets(hw.seed.qspi);
static Settings     settings(hw, &pager);
static CvMatrix     cv_matrix(kNumCvInputs);
static SdCard       sd;
static hostlink::FsExtension fs_ext(sd);
static hostlink::Host host(presets, "plaits_alchemy", "Plaits",
                           MI_VERSION, MI_GIT_HASH);
static mi::CpuExtras extras(0x504C54u);   /* 'PLT' */
static SelectorHandle trig_mode, level_cv;

#ifdef MI_BENCH_USB
static daisy::UsbHandle          bench_usb;
static hostlink::CdcUsbTransport bench_cdc;
#endif

/* ---- control frame -> audio block ---------------------------------------- */

/*
 * The control thread writes the patch here at the frame rate; the audio
 * callback copies it into the Voice's Patch. Single floats, ints and bools:
 * a torn read is impossible on the M7.
 */
struct Targets
{
    volatile float note = 60.f, harmonics = 0.5f, timbre = 0.5f, morph = 0.5f;
    volatile float fm_amt = 0.f, timbre_amt = 0.f, morph_amt = 0.f;
    volatile float decay = 0.5f, lpg_colour = 0.f;
    volatile int   engine = 8;
    volatile int   trig_mode = 0;       /* 0 auto, 1 drone, 2 triggered */
    volatile bool  level_on = false;
    volatile bool  strike = false;      /* B1 held */
};
static Targets T;

static mi::Gate g_trig;

/* Read back by the LEDs. */
static volatile int   G_ENGINE    = 8;
static volatile bool  G_TRIGGERED = false;   /* the LPG is in use */
static volatile float G_OUT_PEAK  = 0.0f;
static volatile bool  G_GATE      = false;

/* ---- buttons (1 ms poll) -------------------------------------------------- */

static mi::Swallow swallow;
static bool        g_bank_down = false;
static volatile bool g_bank_tap = false;

static void OnPoll(uint32_t now)
{
    (void)now;
    if (settings.IsActive())
    {
        T.strike    = false;
        g_bank_down = true;   /* no bank jump from the press that leaves Settings */
        swallow.All();
        return;
    }
    T.strike = swallow.Live(hw, kButtonB1);

    /* B2 stands down while B3 is held: B2+B3 held two seconds is the
     * Settings chord. */
    const bool b3 = swallow.Live(hw, kButtonB3);
    const bool b2 = !b3 && swallow.Live(hw, kButtonB2);
    if (b2 && !g_bank_down) g_bank_tap = true;
    g_bank_down = b2;
}

/* ---- control frame (~60 Hz) ------------------------------------------------ */

static mi::StoredWatch<kNumPots> play_watch, setup_watch;
static float g_saved_peak  = 0.0f;
static bool  prev_settings = false;
static int   prev_trig = -1, prev_level = -1;

/* Plaits' octave quantizer for OCTAVE = 9 (ui.cc: 9 zones, hysteresis 0.1). */
static stmlib::HysteresisQuantizer2 octave_quantizer;

static float compute_note(float transposition, int oct, float fine_st)
{
    /* ui.cc, with fine_tune_ fixed at its centre and FINE as a trim. */
    float note;
    if (oct == 0)
        note = -48.37f + transposition * 60.0f;
    else if (oct == 9)
        note = 53.0f + 7.0f + 12.0f * (float)(
            octave_quantizer.Process(0.5f * transposition + 0.5f) - 4);
    else if (oct == 10)
        note = 60.0f + transposition * 48.0f;
    else
        note = transposition * 7.0f + (float)oct * 12.0f;
    return note + fine_st;
}

static void OnFrame(void)
{
    const uint32_t now = System::GetNow();

    if (play_watch.Changed(pager, kPagePlay)) home.Touch(now);
    if (setup_watch.Changed(pager, kPageSetup)) home.Touch(now);

    /* B2: the same model in the next bank. The stored value moves (so it
     * saves and the ring shows it) and the pot re-arms its catch. */
    if (g_bank_tap)
    {
        g_bank_tap = false;
        int e = (int)model.Value();
        e     = (e + 8) % kNumEngines;
        float phys[kNumPots];
        for (uint8_t p = 0; p < kNumPots; p++) phys[p] = hw.pots[p].Value();
        pager.SetStored(kPagePlay, kPotTopLeft, ((float)e + 0.5f) / (float)kNumEngines, phys);
        home.Touch(now);
    }

    /* Values are catch + locks, already mixed by the SDK (the CV matrix is
     * off: every jack is read in the callback with Plaits' own scaling). */
    int e = (int)model.Value();
    if (e < 0) e = 0;
    if (e >= kNumEngines) e = kNumEngines - 1;
    T.engine = e;

    int oct = (int)octave.Value();
    if (oct < 0) oct = 0;
    if (oct > 10) oct = 10;
    T.note = compute_note(mi::clampf(frequency.Value(), -1.f, 1.f), oct,
                          mi::clampf(fine.Value(), -1.f, 1.f));

    T.harmonics  = mi::clampf(harmonics.Value(), 0.f, 1.f);
    T.timbre     = mi::clampf(timbre.Value(), 0.f, 1.f);
    T.morph      = mi::clampf(morph.Value(), 0.f, 1.f);
    T.decay      = mi::clampf(decay.Value(), 0.f, 1.f);
    T.fm_amt     = mi::clampf(fm_att.Value(), -1.f, 1.f);
    T.timbre_amt = mi::clampf(timbre_att.Value(), -1.f, 1.f);
    T.morph_amt  = mi::clampf(morph_att.Value(), -1.f, 1.f);
    T.lpg_colour = mi::clampf(lpg_colour.Value(), 0.f, 1.f);

    T.trig_mode = (int)trig_mode.Value();
    T.level_on  = (int)level_cv.Value() == 1;
    if (prev_trig >= 0 && (T.trig_mode != prev_trig || (int)T.level_on != prev_level))
        home.Touch(now);
    prev_trig  = T.trig_mode;
    prev_level = (int)T.level_on;

    if (extras.Track(cpu, g_saved_peak)) home.Touch(now);

    const bool sact = settings.IsActive();
    if (prev_settings && !sact) home.Touch(now);
    prev_settings = sact;

    if (home.Service(presets, hw, now, sact || picker::Busy()))
        g_saved_peak = extras.cpu_peak;
}

/* ---- LEDs ------------------------------------------------------------------ */

/*
 * The SDK draws the rings from the knobs. Added:
 *   MODEL ring  the bank's colour on the selected model's dot
 *   B1 pair     white while the trigger (J3 or B1) is high; dim otherwise,
 *               tinted with the output level
 *   B2 pair     the active bank's colour (orange / green / red); red when
 *               the audio callback averages over 80 %
 *   B3 pair     dim Setup tint
 *   P1 ring     the previous session's worst CPU load, 2.5 s at boot
 */
static uint32_t g_readout_until = 0;
static float    g_boot_peak     = 0.0f;

static void OnRender(uint32_t t_ms)
{
    LedPanel& L = hw.leds;
    if (t_ms < g_readout_until) mi::paint_cpu_readout(hw, g_boot_peak);
    if (settings.IsActive()) return;   /* Settings owns the buttons */

    const int bank = G_ENGINE / 8;

    LedPanel::Rgb b1 = kIdle;
    if (G_GATE) b1 = kWhite;
    else
    {
        const float   pk = mi::clampf(G_OUT_PEAK, 0.f, 1.f);
        const uint8_t v  = (uint8_t)(0x10 + 0xB0 * pk);
        b1 = {v, v, (uint8_t)(v > 0x30 ? v : 0x30)};
    }
    L.SetButtonPair(kButtonB1, L.ScaleGlobal(b1));

    const LedPanel::Rgb b2 = cpu.GetAvgCpuLoad() > 0.80f ? kRed : kBank[bank < 3 ? bank : 0];
    L.SetButtonPair(kButtonB2, L.ScaleGlobal(b2));

    if (pager.Page() != kPageSetup)
        L.SetButtonPair(kButtonB3, L.ScaleGlobal(kColSetup));
}

/* Paint the selected model's dot in its bank colour. */
static void ModelOverdraw(LedPanel& panel, uint8_t pot, const ArcGeometry& geo,
                          float norm, uint32_t t_ms, void* ctx)
{
    (void)norm; (void)t_ms; (void)ctx;
    const int e   = G_ENGINE;
    const int led = (int)lroundf((float)e * (float)(geo.arc_leds - 1) / (float)(kNumEngines - 1));
    panel.SetRingByHour(pot, fmodf(geo.start_hour + geo.step_hours * (float)led, 12.0f),
                        panel.ScaleGlobal(kBank[e / 8]));
}

/* ---- audio ----------------------------------------------------------------- */

static plaits::Patch        patch;
static plaits::Modulations  mods;
static plaits::Voice::Frame frames[kRender];

/*
 * Patched-jack detection. Plaits drove a probe signal into its normalled
 * jacks to tell "patched at 0 V" from "unpatched"; the Lab has no probe, so:
 *   TIMBRE / MORPH (J6 / J7)  patched while a CV above 50 mV has been seen in
 *              the last two seconds. All the flag changes is what the
 *              attenuverter scales (the CV, or the internal envelope), so a
 *              wrong guess is harmless.
 *   TRIGGER    the Settings TRIGGER mode: Drone (never), Triggered (always),
 *              or Auto (the default): patched from the first trigger on J3
 *              or B1 since boot. Upstream an unpatched TRIGGER bypasses the
 *              LPG (a drone), a patched one closes it between hits.
 *   LEVEL      the Settings LEVEL CV switch, off by default: a patched
 *              LEVEL replaces the LPG's envelope with the CV, and 0 V there
 *              would otherwise silence the module.
 */
static uint32_t s_timbre_seen = 0, s_morph_seen = 0;
static uint32_t s_blocks      = 0;
static bool     s_auto_trig   = false;
static float    s_note_lp     = 0.f;
static float    s_peak        = 0.f;

static constexpr uint32_t kHold2s = 2u * 48000u / kBlockSize;

static void AudioCallback(daisy::AudioHandle::InputBuffer  in,
                          daisy::AudioHandle::OutputBuffer out,
                          size_t                           size)
{
    (void)in;
    cpu.OnBlockStart();
    s_blocks++;

    const bool gate = g_trig.Poll(hw) || T.strike;
    G_GATE = gate;

    if (size != kBlockSize)
    {
        for (size_t i = 0; i < size; i++) { out[0][i] = 0.f; out[1][i] = 0.f; }
        cpu.OnBlockEnd();
        return;
    }

    /* Patch: the knobs. */
    patch.note                        = T.note;
    patch.harmonics                   = T.harmonics;
    patch.timbre                      = T.timbre;
    patch.morph                       = T.morph;
    patch.frequency_modulation_amount = T.fm_amt;
    patch.timbre_modulation_amount    = T.timbre_amt;
    patch.morph_modulation_amount     = T.morph_amt;
    patch.engine                      = T.engine;
    patch.decay                       = T.decay;
    patch.lpg_colour                  = T.lpg_colour;

    /* Modulations: the jacks, scaled as Plaits' calibration defaults
     * (settings.cc) scale a +-5 V ADC span. */
    const float v_timbre = hw.cv_jacks[kCvTimbre].Volts();
    const float v_morph  = hw.cv_jacks[kCvMorph].Volts();
    if (fabsf(v_timbre) > 0.05f) s_timbre_seen = s_blocks;
    if (fabsf(v_morph) > 0.05f)  s_morph_seen  = s_blocks;

    /* V/Oct with ui.cc's one-pole (0.7 per control tick). */
    s_note_lp += 0.7f * (12.0f * hw.cv_jacks[kCvVoct].Volts() - s_note_lp);

    if (gate) s_auto_trig = true;
    const int tm = T.trig_mode;
    const bool trig_patched = tm == 2 || (tm == 0 && s_auto_trig);

    mods.engine            = 0.f;
    mods.note              = s_note_lp;
    mods.frequency         = 0.f;
    mods.harmonics         = 0.2f  * hw.cv_jacks[kCvHarm].Volts();
    mods.timbre            = 0.32f * v_timbre;
    mods.morph             = 0.32f * v_morph;
    mods.trigger           = gate ? 1.0f : 0.0f;
    mods.level             = mi::clampf(0.2f * hw.cv_jacks[kCvLevel].Volts(), 0.f, 1.f);
    mods.frequency_patched = false;
    mods.timbre_patched    = s_blocks - s_timbre_seen < kHold2s && s_timbre_seen;
    mods.morph_patched     = s_blocks - s_morph_seen < kHold2s && s_morph_seen;
    mods.trigger_patched   = trig_patched;
    mods.level_patched     = T.level_on;
    G_TRIGGERED            = trig_patched;

    const float k  = 1.0f / 32768.0f;
    float       pk = 0.f;
    for (uint32_t half = 0; half < 2; half++)
    {
        voice.Render(patch, mods, frames, kRender);
        for (uint32_t i = 0; i < kRender; i++)
        {
            const float o = (float)frames[i].out * k;
            out[0][half * kRender + i] = o;
            out[1][half * kRender + i] = (float)frames[i].aux * k;
            const float a = fabsf(o);
            if (a > pk) pk = a;
        }
    }
    s_peak     = pk > s_peak ? pk : s_peak * 0.995f;
    G_OUT_PEAK = s_peak;
    G_ENGINE   = voice.active_engine();

    cpu.OnBlockEnd();
}

/* ---- boot ------------------------------------------------------------------ */

int main(void)
{
    hw.Init(daisy::SaiHandle::Config::SampleRate::SAI_48KHZ, kBlockSize);
    cpu.Init(hw.SampleRate(), (int)hw.BlockSize());
    if (hw.BlockSize() != kBlockSize) mi::fault_forever(hw);

    /* NOLOAD section: start from zeros, as upstream's .bss did. */
    memset(shared_buffer, 0, sizeof shared_buffer);
    stmlib::BufferAllocator allocator(shared_buffer, sizeof shared_buffer);
    voice.Init(&allocator);
    octave_quantizer.Init(9, 0.01f, false);
    memset(&mods, 0, sizeof mods);
    memset(&patch, 0, sizeof patch);

    g_trig.Init(hw, kCvTrig);

    sd.Init();
    picker::Install(settings, kSettingsFirmware, sd, hw);
    settings.UseBrightness();
    settings.UsePresets(presets);
    trig_mode = settings.Page(kSettingsMain).Pot(1)
        .Selector(kTrigLabels).Default(0)
        .Ident("trigger").Name("Trigger")
        .Help("**Auto**: the module drones until the first trigger on J3 (or "
              "B1), then the low-pass gate and the decay envelope take over "
              "until the next power-up. **Drone**: never gated. "
              "**Triggered**: always gated, silent between hits.");
    level_cv = settings.Page(kSettingsMain).Pot(4)
        .Selector(kLevelLabels).Default(0)
        .Ident("level_cv").Name("Level CV")
        .Help("**On**: J4 drives the low-pass gate directly (an envelope or "
              "a VCA CV) instead of the internal envelope; 0 V is silence. "
              "**Off**: J4 is ignored.");

    /* Every jack is read raw in the callback with Plaits' own scaling. */
    for (uint8_t j = 0; j < 6; j++) cv_matrix.Jack(j).Off();

    model.Overdraw(ModelOverdraw);

    host.Product("Plaits")
        .BootSlot(home.slot)
        .Pages(play_page, setup_page)
        .Jacks(jk_in_l, jk_in_r, jk_trig, jk_level, jk_voct, jk_timbre,
               jk_morph, jk_harm, jk_out, jk_aux)
        .Buttons(bt_strike, bt_bank, bt_setup)
        .Attach(manual)
        .Extend(fs_ext);
#ifdef MI_BENCH_USB
    bench_cdc.Init(bench_usb, daisy::UsbHandle::FS_INTERNAL, "Plaits (bench)");
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
        /* First boot after a flash: Plaits' own factory state (settings.cc)
         * -- the first classic model (virtual analog), the free 8-octave
         * range, decay at noon, LPG colour VCFA -- with the attenuverters
         * at noon (off) and no fine tune. Zone i of n sits at (i+0.5)/n. */
        pager.SetStored(kPageSetup, kPotTopLeft,     10.5f / 11.0f, phys);
        pager.SetStored(kPageSetup, kPotTopRight,    0.5f, phys);
        pager.SetStored(kPageSetup, kPotMiddleLeft,  0.5f, phys);
        pager.SetStored(kPageSetup, kPotMiddleRight, 0.5f, phys);
        pager.SetStored(kPageSetup, kPotBottomLeft,  0.5f, phys);
        pager.SetStored(kPageSetup, kPotBottomRight, 0.0f, phys);

        /* The PLAY page adopts the pots -- except MODEL, which starts on
         * Plaits' factory model (8, virtual analog) and catches. */
        for (uint8_t p = 0; p < kNumPots; p++)
            pager.SetStored(kPagePlay, p, phys[p], phys);
        pager.SetStored(kPagePlay, kPotTopLeft, 8.5f / (float)kNumEngines, phys);
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

    for (;;) loop.Tick();
}
