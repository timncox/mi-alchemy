/*
 * Elements on the Hermetic Modular Alchemy Lab V2 -- Mutable Instruments'
 * modal synthesis voice (bow / blow / strike exciters into a modal or
 * string resonator, with a reverb) as Alchemy SDK firmware.
 *
 * The DSP is elements::Part, vendored byte-identical from pichenettes/
 * eurorack (vendor/VENDOR.md), run at its native 32 kHz with its native
 * 16-frame block. Its two exciter sample tables live on the SD card
 * (elements_samples.h); everything else is in the image. This file is the
 * host shim: what Elements' cv_scaler.cc and ui.cc did, on six pots across
 * three pages, three buttons and ten jacks.
 *
 *   PLAY     COARSE . FINE . GEOMETRY . BRIGHTNESS . DAMPING . POSITION
 *   EXCITER  BOW . BLOW . STRIKE . CONTOUR . FLOW . MALLET      (hold B3)
 *   TIMBRE   BOW T. . BLOW T. . STRIKE T. . SPACE . MODEL . FM  (hold B2)
 *   B1       PLAY: the gate, while held (Elements' Play button)
 *   J1 blow in . J2 strike in . J3 gate . J4 strength . J5 V/Oct . J6 FM
 *   J7/J8 CV (geometry / brightness) . J9 main . J10 aux
 *   Settings (B2+B3 2 s): brightness, presets; page 1 = the SD picker
 *
 * See DESIGN.md for every mapping and what was left out.
 */
#include <math.h>
#include <string.h>

#include "daisy_seed.h"
#include "util/CpuLoadMeter.h"
#include "ff.h"

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
#include "elements_samples.h"

#include "elements/dsp/dsp.h"
#include "elements/dsp/part.h"

#ifndef MI_VERSION
#define MI_VERSION "0.0.0"
#endif
#ifndef MI_GIT_HASH
#define MI_GIT_HASH "dev"
#endif

using namespace alchemy;
using daisy::System;

/* elements_storage.cpp: the SDRAM arrays behind elements::smp_* */
int16_t* es_sample_storage();
int16_t* es_noise_storage();

/* ---- geometry ----------------------------------------------------------- */

/* Elements' own codec block: 16 frames at 32 kHz, so the parameter reads
 * below happen at 2 kHz, as cv_scaler.cc's did. */
static constexpr uint32_t kBlockSize = elements::kMaxBlockSize;

enum : uint8_t { kPagePlay = 0, kPageExciter = 1, kPageTimbre = 2, kNumAppPages = 3 };
enum : uint8_t { kSettingsMain = 0, kSettingsFirmware = 1 };

/* CV indices: 0 = J3 ... 5 = J8. */
static constexpr uint8_t kCvGate = 0u, kCvStrength = 1u, kCvVoct = 2u, kCvFm = 3u;

/* ---- hardware + engine -------------------------------------------------- */

static AlchemyLab          hw;
static daisy::CpuLoadMeter cpu;

/* The reverb's buffer, sized as upstream (the F4's CCM), and the engine
 * itself (too big for what DTCM has left after the SDK), both in cached D2
 * SRAM. NOLOAD: zeroed in main before Init. */
static uint16_t MI_D2_BSS reverb_buffer[32768];
static elements::Part MI_D2_BSS part;

/* ---- colours ------------------------------------------------------------ */

static constexpr LedPanel::Rgb kColCoarse = {0xFF, 0x90, 0x20};
static constexpr LedPanel::Rgb kColFineU  = {0xFF, 0xC0, 0x60};
static constexpr LedPanel::Rgb kColFineD  = {0x60, 0xC0, 0xFF};
static constexpr LedPanel::Rgb kColGeom   = {0x40, 0xC0, 0xFF};
static constexpr LedPanel::Rgb kColBright = {0xFF, 0xFF, 0x80};
static constexpr LedPanel::Rgb kColDamp   = {0x60, 0xFF, 0xC0};
static constexpr LedPanel::Rgb kColPos    = {0xC0, 0x60, 0xFF};
static constexpr LedPanel::Rgb kColBow    = {0xFF, 0x40, 0x40};
static constexpr LedPanel::Rgb kColBlow   = {0x40, 0xFF, 0x40};
static constexpr LedPanel::Rgb kColStrike = {0x40, 0x80, 0xFF};
static constexpr LedPanel::Rgb kColEnv    = {0xFF, 0xFF, 0xFF};
static constexpr LedPanel::Rgb kColSpace  = {0x80, 0x80, 0xFF};
static constexpr LedPanel::Rgb kColModel  = {0xFF, 0x60, 0xA0};
static constexpr LedPanel::Rgb kColFmU    = {0xFF, 0x80, 0x00};
static constexpr LedPanel::Rgb kColFmD    = {0x00, 0x80, 0xFF};
static constexpr LedPanel::Rgb kColNotch  = {0xFF, 0xFF, 0xFF};
static constexpr LedPanel::Rgb kColDim    = {0x18, 0x18, 0x18};
static constexpr LedPanel::Rgb kColExPage = {0x40, 0x10, 0x10};
static constexpr LedPanel::Rgb kColTbPage = {0x10, 0x10, 0x40};

static constexpr LedPanel::Rgb kIdle  = {0x00, 0x00, 0x30};
static constexpr LedPanel::Rgb kWhite = {0xFF, 0xFF, 0xFF};
static constexpr LedPanel::Rgb kAmber = {0xFF, 0x80, 0x00};
static constexpr LedPanel::Rgb kRed   = {0xFF, 0x00, 0x00};
static constexpr LedPanel::Rgb kGreen = {0x00, 0xFF, 0x00};

static const char* const kModelLabels[4] = {"Modal", "String", "Strings", "Ominous"};

/* ---- pages --------------------------------------------------------------- */

/* PLAY. Panel positions, front view:
 *      [P1] B1 [P2]
 *      [P3] B2 [P4]
 *      [P5] B3 [P6]
 * Knobs hand the shim 0..1 (or the bipolar -1..1 of an attenuverter) and
 * the audio callback applies cv_scaler.cc's laws. */

static VirtualKnob coarse = VirtualKnob(kPotTopLeft, "Coarse")
    .Linear(0.f, 1.f).Ident("coarse")
    .Help("Resonator pitch over five octaves, in semitone steps. J5 adds "
          "1 V/octave.")
    .Ring(Level(kColCoarse));

static VirtualKnob fine = VirtualKnob(kPotTopRight, "Fine")
    .Linear(-1.f, 1.f).Ident("fine")
    .Help("Fine tune, +/- 2 semitones (square law: finest at noon).")
    .Ring(Bipolar(kColFineU, kColFineD, kColNotch));

static VirtualKnob geometry = VirtualKnob(kPotMiddleLeft, "Geometry")
    .Linear(0.f, 1.f).Ident("geometry")
    .Help("The resonator's structure: from plates and bars through strings "
          "to bells and inharmonic objects.")
    .Ring(Level(kColGeom));

static VirtualKnob brightness = VirtualKnob(kPotMiddleRight, "Brightness")
    .Linear(0.f, 1.f).Ident("brightness")
    .Ring(Level(kColBright));

static VirtualKnob damping = VirtualKnob(kPotBottomLeft, "Damping")
    .Linear(0.f, 1.f).Ident("damping")
    .Help("How long the resonator rings.")
    .Ring(Level(kColDamp));

static VirtualKnob position = VirtualKnob(kPotBottomRight, "Position")
    .Linear(0.f, 1.f).Ident("position")
    .Help("Where on the resonator it is excited (the comb-filtering of the "
          "modes).")
    .Ring(Level(kColPos));

/* EXCITER: hold B3. Elements' three level sliders and the envelope, flow
 * and mallet knobs. */

static VirtualKnob bow = VirtualKnob(kPotTopLeft, "Bow")
    .Linear(0.f, 1.f).Ident("bow_level")
    .Ring(Level(kColBow));

static VirtualKnob blow = VirtualKnob(kPotTopRight, "Blow")
    .Linear(0.f, 1.f).Ident("blow_level")
    .Help("Level of the blow exciter: the granular noise (or J1's audio, "
          "which replaces it when patched and loud enough).")
    .Ring(Level(kColBlow));

static VirtualKnob strike = VirtualKnob(kPotMiddleLeft, "Strike")
    .Linear(0.f, 1.f).Ident("strike_level")
    .Help("Level of the strike exciter: mallet, samples, particles (or "
          "J2's audio).")
    .Ring(Level(kColStrike));

static VirtualKnob contour = VirtualKnob(kPotMiddleRight, "Contour")
    .Linear(0.f, 1.f).Ident("contour")
    .Help("The exciter envelope, gate-length AD to a slow swell (ADSR "
          "shapes in between).")
    .Ring(Level(kColEnv));

static VirtualKnob flow = VirtualKnob(kPotBottomLeft, "Flow")
    .Linear(0.f, 1.f).Ident("flow")
    .Help("Blow meta: the noise's character, from a steady breath to "
          "granular sputter.")
    .Ring(Level(kColBlow));

static VirtualKnob mallet = VirtualKnob(kPotBottomRight, "Mallet")
    .Linear(0.f, 1.f).Ident("mallet")
    .Help("Strike meta: a mallet, then the samples, then particle "
          "(bouncing) excitation.")
    .Ring(Level(kColStrike));

/* TIMBRE: hold B2. The three exciter timbres, SPACE, the resonator model
 * and the FM attenuverter. */

static VirtualKnob bow_timbre = VirtualKnob(kPotTopLeft, "Bow timbre")
    .Linear(0.f, 1.f).Ident("bow_timbre")
    .Ring(Level(kColBow));

static VirtualKnob blow_timbre = VirtualKnob(kPotTopRight, "Blow timbre")
    .Linear(0.f, 1.f).Ident("blow_timbre")
    .Ring(Level(kColBlow));

static VirtualKnob strike_timbre = VirtualKnob(kPotMiddleLeft, "Strike timbre")
    .Linear(0.f, 1.f).Ident("strike_timbre")
    .Ring(Level(kColStrike));

static VirtualKnob space = VirtualKnob(kPotMiddleRight, "Space")
    .Linear(0.f, 2.f).Ident("space")
    .Help("Mono to stereo to reverb. The last eighth of the travel freezes "
          "the reverb (Elements reached that only with CV).")
    .Ring(Level(kColSpace));

static VirtualKnob model = VirtualKnob(kPotBottomLeft, "Model")
    .Selector(4).Labels(kModelLabels).Ident("model")
    .Help("Resonator: modal (Elements), a single string, a sympathetic "
          "string ensemble, or Ominous -- the hidden two-oscillator FM "
          "drone voice Elements shipped as an easter egg.")
    .Ring(SelectorRing(kColModel, kColDim, 4));

static VirtualKnob fm = VirtualKnob(kPotBottomRight, "FM")
    .Linear(-1.f, 1.f).Ident("fm_amount")
    .Help("Attenuverter for J6's frequency modulation (quartic: fine near "
          "noon, up to 12 semitones per volt at the ends).")
    .Ring(Bipolar(kColFmU, kColFmD, kColNotch));

static Page play_page = Page(kPagePlay).Name("Play").Color("#ff9020")
    .Knobs(coarse, fine, geometry, brightness, damping, position);

static Page exciter_page = Page(kPageExciter).Name("Exciter").Color("#ff4040")
    .Knobs(bow, blow, strike, contour, flow, mallet);

static Page timbre_page = Page(kPageTimbre).Name("Timbre").Color("#4040ff")
    .Knobs(bow_timbre, blow_timbre, strike_timbre, space, model, fm);

/* ---- descriptor metadata -------------------------------------------------- */

static Jack jk_blow   ("J1",  "Blow in",       JackSig::AudioIn);
static Jack jk_strike ("J2",  "Strike in",     JackSig::AudioIn);
static Jack jk_gate   ("J3",  "Gate",          JackSig::Trig);
static Jack jk_str    ("J4",  "Strength",      JackSig::CvBi);
static Jack jk_voct   ("J5",  "V/Oct",         JackSig::CvBi);
static Jack jk_fm     ("J6",  "FM",            JackSig::CvBi);
static Jack jk_cv_geo ("J7",  "CV Geometry",   JackSig::CvBi);
static Jack jk_cv_brt ("J8",  "CV Brightness", JackSig::CvBi);
static Jack jk_main   ("J9",  "Out main (L)",  JackSig::AudioOut);
static Jack jk_aux    ("J10", "Out aux (R)",   JackSig::AudioOut);

static VirtualButton bt_play = VirtualButton("b1", "Play")
    .Action("Hold", "Gate: the exciters play while held (J3 ORs in)");

static VirtualButton bt_timbre = VirtualButton("b2", "Timbre")
    .Action("Hold", "Show the Timbre page");

static VirtualButton bt_exciter = VirtualButton("b3", "Exciter")
    .Action("Hold", "Show the Exciter page");

static Manual manual = Manual()
    .Tagline("Modal synthesis voice: bow, blow and strike a virtual resonator")
    .Preamble(
        "**Elements** excites a physically modelled resonator with a bow, a "
        "breath of noise and a strike (mallet, samples, particles), mixed "
        "on the Exciter page (hold B3) and coloured on the Timbre page "
        "(hold B2). The Play page tunes the resonator (COARSE, FINE, V/Oct "
        "on J5) and shapes it (GEOMETRY, BRIGHTNESS, DAMPING, POSITION). "
        "Hold B1, or send a gate to J3, to play; J4 sets the strength of "
        "each hit. Patch audio into J1 or J2 and it replaces the blow or "
        "strike exciter. SPACE (Timbre page) goes from mono through "
        "stereo to reverb and, at the end, a frozen reverb. MODEL picks the "
        "modal resonator, a string, strings, or the hidden Ominous drone. "
        "The strike and blow samples load from the SD card "
        "(/mi/elements.smp); without it B1 glows amber and those exciters "
        "are silent. The DSP is Mutable Instruments' Elements (Emilie "
        "Gillet, MIT), unmodified.");

/* ---- surfaces ------------------------------------------------------------ */

static ControlLoop loop(hw);
static Pager       pager = Pager(kNumAppPages, kNumPots)
                               .Shift(hw.buttons[kButtonB3], kPageExciter)
                               .Shift(hw.buttons[kButtonB2], kPageTimbre);

/* Home slot 9: see mi_family.h for the whole card's map. */
static mi::HomeSlot home(9u);
static Presets      presets(hw.seed.qspi);
static Settings     settings(hw, &pager);
static CvMatrix     cv_matrix(kNumCvInputs);
static SdCard       sd;
static hostlink::FsExtension fs_ext(sd);
static hostlink::Host host(presets, "elements_alchemy", "Elements",
                           MI_VERSION, MI_GIT_HASH);
static mi::CpuExtras extras(0x454C45u);   /* 'ELE' */

#ifdef MI_BENCH_USB
static daisy::UsbHandle          bench_usb;
static hostlink::CdcUsbTransport bench_cdc;
#endif

/* ---- samples from the card ------------------------------------------------ */

static es::Status g_samples = es::Status::Missing;

alignas(32) static ALCHEMY_SDMMC_BSS FIL     s_smp_fil;
alignas(32) static ALCHEMY_SDMMC_BSS uint8_t s_smp_chunk[4096];

/* Boot, before the audio starts: the whole 338 KB file in 4 KB reads into
 * the DMA-reachable staging buffer, each copied on into SDRAM by the
 * loader. Anything short of an intact file zeroes both tables. */
static es::Status load_samples(void)
{
    es::Loader ld;
    int16_t* const data  = es_sample_storage();
    int16_t* const noise = es_noise_storage();
    ld.Begin(data, noise);
    es::Status st = es::Status::Missing;
    if (sd.EnsureMounted(System::GetNow()))
    {
        SdCard::BusyGuard guard(sd);
        if (f_open(&s_smp_fil, es::kPath, FA_READ) == FR_OK)
        {
            bool io_ok = true;
            for (;;)
            {
                UINT n = 0;
                if (f_read(&s_smp_fil, s_smp_chunk, sizeof s_smp_chunk, &n) != FR_OK)
                {
                    io_ok = false;
                    break;
                }
                if (n == 0 || !ld.Feed(s_smp_chunk, n)) break;
            }
            f_close(&s_smp_fil);
            st = io_ok ? ld.Finish() : es::Status::Short;
        }
    }
    if (st != es::Status::Ok)
    {
        memset(data, 0, 2u * es::kSampleCount);
        memset(noise, 0, 2u * es::kNoiseCount);
    }
    return st;
}

/* ---- control frame -> audio block ---------------------------------------- */

/*
 * The control thread writes knob targets here at the frame rate; the audio
 * callback applies cv_scaler.cc's laws and smoothing once per block.
 * Single floats and bools: a torn read is impossible on the M7.
 */
struct Targets
{
    volatile float coarse = 0.5f, fine = 0.f, geometry = 0.5f, brightness = 0.5f;
    volatile float damping = 0.5f, position = 0.5f;
    volatile float bow = 0.f, blow = 0.f, strike = 0.8f, contour = 0.5f;
    volatile float flow = 0.5f, mallet = 0.5f;
    volatile float bow_timbre = 0.5f, blow_timbre = 0.5f, strike_timbre = 0.5f;
    volatile float space = 0.5f, fm = 0.f;
    volatile int   model = 0;
    volatile bool  play_btn = false;
};
static Targets T;

static mi::Gate g_gate;

/* Read back by the LEDs. */
static volatile bool  G_GATE    = false;
static volatile float G_EXCITER = 0.f;
static volatile float G_RESON   = 0.f;

/* ---- buttons (1 ms poll) -------------------------------------------------- */

static mi::Swallow swallow;

static void OnPoll(uint32_t now)
{
    (void)now;
    if (settings.IsActive())
    {
        T.play_btn = false;
        swallow.All();
        return;
    }
    /* B1 is momentary: a tap is a short note, as on Elements. B2 and B3
     * belong to the pager (page layers). */
    T.play_btn = swallow.Live(hw, kButtonB1);
}

/* ---- control frame (~60 Hz) ------------------------------------------------ */

static mi::StoredWatch<kNumPots> play_watch, exciter_watch, timbre_watch;
static float g_saved_peak  = 0.0f;
static bool  prev_settings = false;
static int   applied_model = -1;

static void OnFrame(void)
{
    const uint32_t now = System::GetNow();

    /* Every page is worth keeping: each is restored at boot and caught. */
    if (play_watch.Changed(pager, kPagePlay)) home.Touch(now);
    if (exciter_watch.Changed(pager, kPageExciter)) home.Touch(now);
    if (timbre_watch.Changed(pager, kPageTimbre)) home.Touch(now);

    /* Values are catch + locks + CV, already mixed by the SDK. */
    T.coarse        = mi::clampf(coarse.Value(), 0.f, 1.f);
    T.fine          = mi::clampf(fine.Value(), -1.f, 1.f);
    T.geometry      = mi::clampf(geometry.Value(), 0.f, 1.f);
    T.brightness    = mi::clampf(brightness.Value(), 0.f, 1.f);
    T.damping       = mi::clampf(damping.Value(), 0.f, 1.f);
    T.position      = mi::clampf(position.Value(), 0.f, 1.f);
    T.bow           = mi::clampf(bow.Value(), 0.f, 1.f);
    T.blow          = mi::clampf(blow.Value(), 0.f, 1.f);
    T.strike        = mi::clampf(strike.Value(), 0.f, 1.f);
    T.contour       = mi::clampf(contour.Value(), 0.f, 1.f);
    T.flow          = mi::clampf(flow.Value(), 0.f, 1.f);
    T.mallet        = mi::clampf(mallet.Value(), 0.f, 1.f);
    T.bow_timbre    = mi::clampf(bow_timbre.Value(), 0.f, 1.f);
    T.blow_timbre   = mi::clampf(blow_timbre.Value(), 0.f, 1.f);
    T.strike_timbre = mi::clampf(strike_timbre.Value(), 0.f, 1.f);
    T.space         = mi::clampf(space.Value(), 0.f, 2.f);
    T.fm            = mi::clampf(fm.Value(), -1.f, 1.f);

    /* MODEL: Ominous is Elements' easter-egg voice, not a resonator model;
     * the other three are Part's resonator models. Applied here, where
     * ui.cc applied them, between blocks. */
    const int m = (int)model.Value();
    if (m != applied_model && m >= 0 && m < 4)
    {
        applied_model = m;
        part.set_easter_egg(m == 3);
        if (m < 3) part.set_resonator_model(static_cast<elements::ResonatorModel>(m));
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
 * The SDK draws the rings from the knobs, and B2 / B3 in their page colour
 * while held. Added:
 *   B1 pair   white while the gate is on; else the exciter level in
 *             orange over a dim base -- dim blue with the samples loaded,
 *             dim AMBER when /mi/elements.smp is missing or bad
 *   B2 pair   the resonator level in green (Elements' second meter); red
 *             when the audio callback averages over 80 %
 *   B3 pair   dim Exciter tint
 *   P1 ring   the previous session's worst CPU load, 2.5 s at boot
 *   P2 ring   the samples file, 2.5 s at boot: full green loaded, a red
 *             pip missing, full red present but refused
 */
static uint32_t g_readout_until = 0;
static float    g_boot_peak     = 0.0f;

static LedPanel::Rgb mix(const LedPanel::Rgb& a, const LedPanel::Rgb& b, float t)
{
    t = mi::clampf(t, 0.f, 1.f);
    return {(uint8_t)(a.r + (b.r - a.r) * t), (uint8_t)(a.g + (b.g - a.g) * t),
            (uint8_t)(a.b + (b.b - a.b) * t)};
}

static void OnRender(uint32_t t_ms)
{
    LedPanel& L = hw.leds;
    if (t_ms < g_readout_until)
    {
        mi::paint_cpu_readout(hw, g_boot_peak);
        L.ClearRing(kPotTopRight);
        if (g_samples == es::Status::Ok)
            mi::paint_fill(hw, kPotTopRight, 1.0f, kGreen);
        else if (g_samples == es::Status::Missing)
            L.SetRingByHour(kPotTopRight, hw.Arc().start_hour, L.ScaleGlobal(kRed));
        else
            mi::paint_fill(hw, kPotTopRight, 1.0f, kRed);
    }
    if (settings.IsActive()) return;   /* Settings owns the buttons */

    const bool          ok   = g_samples == es::Status::Ok;
    const LedPanel::Rgb base = ok ? kIdle : LedPanel::Rgb{0x30, 0x14, 0x00};
    L.SetButtonPair(kButtonB1, L.ScaleGlobal(G_GATE ? kWhite : mix(base, kAmber, G_EXCITER)));

    const uint8_t pg = pager.Page();
    if (pg != kPageTimbre)
    {
        const LedPanel::Rgb b2 = cpu.GetAvgCpuLoad() > 0.80f
                                     ? kRed
                                     : mix(kColTbPage, kGreen, G_RESON);
        L.SetButtonPair(kButtonB2, L.ScaleGlobal(b2));
    }
    if (pg != kPageExciter)
        L.SetButtonPair(kButtonB3, L.ScaleGlobal(kColExPage));
}

/* ---- audio ----------------------------------------------------------------- */

static float blow_in[kBlockSize], strike_in[kBlockSize];
static float out_main[kBlockSize], out_aux[kBlockSize];

/* cv_scaler.cc's and elements.cc's state, carried between blocks. */
static float sm[18];
static bool  sm_primed    = false;
static float coarse_raw   = 30.f, coarse_q = 30.f;
static float note_cv      = 0.f, modulation = 0.f;
static float strike_level = 0.f, blow_level = 0.f;   /* input noise gates */
static bool  prev_gate    = false, gate_now = false;

static inline float quadratic_bipolar(float x)   /* LAW_QUADRATIC_BIPOLAR */
{
    const float x2 = x * x * 3.3f;
    return x < 0.f ? -x2 : x2;
}

static inline float quartic_bipolar(float x)     /* LAW_QUARTIC_BIPOLAR */
{
    const float x2 = x * x, x4 = x2 * x2 * 3.3f;
    return x < 0.f ? -x4 : x4;
}

static void AudioCallback(daisy::AudioHandle::InputBuffer  in,
                          daisy::AudioHandle::OutputBuffer out,
                          size_t                           size)
{
    cpu.OnBlockStart();

    const bool jack_gate = g_gate.Poll(hw);

    if (size > kBlockSize)
    {
        /* Refuse to run rather than overrun the engine's block arrays. */
        for (size_t i = 0; i < size; i++) { out[0][i] = in[0][i]; out[1][i] = in[1][i]; }
        cpu.OnBlockEnd();
        return;
    }

    /* Front-panel pots were low-passed at 0.01 per read upstream; the
     * targets here get the same, the bindings with a CV input a faster
     * 0.05 (upstream applied the CV part unfiltered). */
    const float tgt[18] = {T.geometry, T.brightness, T.damping, T.position,
                           T.bow, T.blow, T.strike, T.contour, T.flow, T.mallet,
                           T.bow_timbre, T.blow_timbre, T.strike_timbre, T.space,
                           T.fine, T.fm, 0.f, 0.f};
    static const float kCoef[18] = {0.05f, 0.05f, 0.05f, 0.01f, 0.01f, 0.01f,
                                    0.01f, 0.01f, 0.05f, 0.05f, 0.05f, 0.05f,
                                    0.05f, 0.01f, 0.01f, 0.01f, 0.f, 0.f};
    for (int i = 0; i < 16; i++)
    {
        if (!sm_primed) sm[i] = tgt[i];
        sm[i] += kCoef[i] * (tgt[i] - sm[i]);
    }
    sm_primed = true;

    elements::Patch* p = part.mutable_patch();
    p->resonator_geometry     = mi::clampf(sm[0], 0.f, 0.9995f);
    p->resonator_brightness   = mi::clampf(sm[1], 0.f, 0.9995f);
    p->resonator_damping      = mi::clampf(sm[2], 0.f, 0.9995f);
    p->resonator_position     = mi::clampf(sm[3], 0.f, 0.9995f);
    p->exciter_bow_level      = sm[4];
    p->exciter_blow_level     = sm[5];
    p->exciter_strike_level   = sm[6];
    p->exciter_envelope_shape = sm[7];
    p->exciter_blow_meta      = mi::clampf(sm[8], 0.f, 0.9995f);
    p->exciter_strike_meta    = mi::clampf(sm[9], 0.f, 0.9995f);
    p->exciter_bow_timbre     = mi::clampf(sm[10], 0.f, 0.9995f);
    p->exciter_blow_timbre    = mi::clampf(sm[11], 0.f, 0.9995f);
    p->exciter_strike_timbre  = mi::clampf(sm[12], 0.f, 0.995f);
    p->space                  = mi::clampf(sm[13], 0.f, 2.f);

    /* COARSE: LAW_QUANTIZED_NOTE, 5 octaves with 0.3-semitone hysteresis. */
    coarse_raw += 0.5f * (60.0f * T.coarse - coarse_raw);
    {
        const float hyst = coarse_raw - coarse_q > 0.0f ? -0.3f : +0.3f;
        coarse_q = (float)(int)(coarse_raw + hyst + 0.5f);
    }

    /* V/Oct: upstream's default calibration puts 0 V at note 24.1
     * (66.67 - 84.26 x the 0.505 ADC reading at 0 V); a jump of more than
     * a quarter tone lands at once, smaller moves glide. */
    const float note = 24.1f + 12.0f * hw.cv_jacks[kCvVoct].Volts();
    const float interval = note - note_cv;
    if (interval < -0.4f || interval > 0.4f) note_cv = note;
    else note_cv += 0.1f * interval;

    /* FM: the attenuverter (quartic) times J6. Upstream's 49.5 x ADC units
     * at full scale is ~12 semitones per volt at the jack. */
    const float mod = quartic_bipolar(sm[15]) / 3.3f * 12.0f * hw.cv_jacks[kCvFm].Volts();
    modulation += 0.5f * (mod - modulation);

    elements::PerformanceState s;
    s.note       = note_cv + coarse_q + 19.0f + quadratic_bipolar(sm[14]) * (2.0f / 3.3f);
    s.modulation = mi::clampf(modulation, -60.f, 60.f);
    /* STRENGTH: upstream 1 - ADC, 0.5 at 0 V; here +/-5 V spans 0..1. */
    s.strength = mi::clampf(0.5f + 0.1f * hw.cv_jacks[kCvStrength].Volts(), 0.f, 1.f);
    /* Upstream reads the gate one block after the CVs so a note's pitch is
     * in before it strikes. */
    s.gate    = prev_gate;
    prev_gate = gate_now;
    gate_now  = jack_gate || T.play_btn;
    G_GATE    = s.gate;

    /* Inputs through elements.cc's noise gates: below -80 dB an input
     * fades out, so an unpatched jack's hiss never replaces the internal
     * exciter. J1 -> blow, J2 -> strike. */
    const float kNoiseGateThreshold = 0.0001f;
    for (size_t i = 0; i < size; i++)
    {
        const float b = in[0][i], st = in[1][i];
        float       e, g;
        e = st * st - strike_level;
        strike_level += e * (e > 0.0f ? 0.1f : 0.0001f);
        g = strike_level <= kNoiseGateThreshold ? (1.0f / kNoiseGateThreshold) * strike_level : 1.0f;
        strike_in[i] = g * st;
        e = b * b - blow_level;
        blow_level += e * (e > 0.0f ? 0.1f : 0.0001f);
        g = blow_level <= kNoiseGateThreshold ? (1.0f / kNoiseGateThreshold) * blow_level : 1.0f;
        blow_in[i] = g * b;
    }

    part.Process(s, blow_in, strike_in, out_main, out_aux, size);

    for (size_t i = 0; i < size; i++)
    {
        out[0][i] = mi::clampf(out_main[i], -1.f, 1.f);
        out[1][i] = mi::clampf(out_aux[i], -1.f, 1.f);
    }
    G_EXCITER = part.exciter_level();
    G_RESON   = part.resonator_level();

    cpu.OnBlockEnd();
}

/* ---- boot ------------------------------------------------------------------ */

int main(void)
{
    hw.Init(daisy::SaiHandle::Config::SampleRate::SAI_32KHZ, kBlockSize);
    cpu.Init(hw.SampleRate(), (int)hw.BlockSize());
    if (hw.BlockSize() != kBlockSize) mi::fault_forever(hw);

    /* NOLOAD section: start from silence, not from whatever was there. */
    memset(reverb_buffer, 0, sizeof reverb_buffer);
    memset(static_cast<void*>(&part), 0, sizeof part);
    part.Init(reverb_buffer);
    /* Upstream seeds the per-unit "signature" (reverb and resonator
     * modulation character) from the F4's serial number; the H750's UID
     * does the same job. */
    part.Seed(reinterpret_cast<uint32_t*>(UID_BASE), 3);

    g_gate.Init(hw, kCvGate);

    sd.Init();
    g_samples = load_samples();
    picker::Install(settings, kSettingsFirmware, sd, hw);
    settings.UseBrightness();
    settings.UsePresets(presets);

    /* CV: J3 gate, J4 strength, J5 V/Oct and J6 FM are read raw in the
     * callback; J7/J8 modulate through the SDK's matrix (re-routable from
     * the web programmer). */
    cv_matrix.Jack(0).Off();
    cv_matrix.Jack(1).Off();
    cv_matrix.Jack(2).Off();
    cv_matrix.Jack(3).Off();
    cv_matrix.Jack(4).To(geometry);
    cv_matrix.Jack(5).To(brightness);

    host.Product("Elements")
        .BootSlot(home.slot)
        .Pages(play_page, exciter_page, timbre_page)
        .Jacks(jk_blow, jk_strike, jk_gate, jk_str, jk_voct, jk_fm,
               jk_cv_geo, jk_cv_brt, jk_main, jk_aux)
        .Buttons(bt_play, bt_timbre, bt_exciter)
        .Attach(manual)
        .Extend(fs_ext);
#ifdef MI_BENCH_USB
    bench_cdc.Init(bench_usb, daisy::UsbHandle::FS_INTERNAL, "Elements (bench)");
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
        /* First boot after a flash: Elements' "init" sound -- a struck
         * modal resonator with a short contour, a little reverb. */
        pager.SetStored(kPageExciter, kPotTopLeft,     0.0f, phys);  /* bow    */
        pager.SetStored(kPageExciter, kPotTopRight,    0.0f, phys);  /* blow   */
        pager.SetStored(kPageExciter, kPotMiddleLeft,  0.8f, phys);  /* strike */
        pager.SetStored(kPageExciter, kPotMiddleRight, 0.2f, phys);  /* contour */
        pager.SetStored(kPageExciter, kPotBottomLeft,  0.5f, phys);  /* flow   */
        pager.SetStored(kPageExciter, kPotBottomRight, 0.3f, phys);  /* mallet */
        pager.SetStored(kPageTimbre, kPotTopLeft,     0.5f, phys);
        pager.SetStored(kPageTimbre, kPotTopRight,    0.5f, phys);
        pager.SetStored(kPageTimbre, kPotMiddleLeft,  0.5f, phys);
        pager.SetStored(kPageTimbre, kPotMiddleRight, 0.3f, phys);   /* space 0.6 */
        pager.SetStored(kPageTimbre, kPotBottomLeft,  0.5f / 4.0f, phys); /* modal */
        pager.SetStored(kPageTimbre, kPotBottomRight, 0.5f, phys);   /* no FM */

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
        .Use(exciter_page)
        .Use(timbre_page)
        .Use(host)
        .OnFrame(OnFrame)
        .OnPoll(OnPoll)
        .OnRender(OnRender);

    for (;;) loop.Tick();
}
