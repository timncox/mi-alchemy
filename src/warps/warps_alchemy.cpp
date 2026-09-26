/*
 * Warps on the Hermetic Modular Alchemy Lab V2 -- Mutable Instruments'
 * meta-modulator, in Matthias Puech's Parasites build (nine modes: the
 * stock Warps algorithms as META plus Doppler, fold, Chebyshev, frequency
 * shifter, bitcrusher, comparator, vocoder and delay), as Alchemy SDK
 * firmware.
 *
 * The DSP is warps::Modulator from mqtthiqs/parasites @ 32fa66f, vendored
 * via ~/tim-os/meld (vendor/VENDOR.md: one marked fix), run at its native
 * 96 kHz with its native 60-frame block. The parameter mapping is Parasites'
 * cv_scaler.cc, in warps_params.h (shared with the native test).
 *
 *   PLAY   ALGORITHM . TIMBRE . LEVEL 1 . LEVEL 2 . MODE . CARRIER
 *   SETUP  IN GAIN . TUNE                                   (hold B3)
 *   B1     next CARRIER state (Warps' button)
 *   B2     next MODE
 *   J1 carrier in . J2 modulator in . J3 CV algorithm . J4 CV timbre
 *   J5 V/Oct (internal carrier) . J6/J7 CV level 1/2 . J8 unassigned
 *   J9 out . J10 aux out
 *   Settings (B2+B3 2 s): brightness, presets; page 1 = the SD picker
 *
 * See DESIGN.md.
 */
#include <math.h>
#include <string.h>
#include <new>

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

#include "warps/dsp/modulator.h"
#include "warps/dsp/parameters.h"
#include "warps_params.h"

#ifndef MI_VERSION
#define MI_VERSION "0.0.0"
#endif
#ifndef MI_GIT_HASH
#define MI_GIT_HASH "dev"
#endif

using namespace alchemy;
using daisy::System;

/* ---- geometry ----------------------------------------------------------- */

/* Warps' own block: 60 frames at 96 kHz, so parameter reads happen at
 * 1.6 kHz as on the module and cv_scaler.cc's coefficients carry over. */
static constexpr uint32_t kBlockSize = 60u;
static_assert(kBlockSize <= warps::kMaxBlockSize, "block larger than Warps expects");

enum : uint8_t { kPagePlay = 0, kPageSetup = 1, kNumAppPages = 2 };
enum : uint8_t { kSettingsMain = 0, kSettingsFirmware = 1 };

/* CV index of the V/Oct jack: 0 = J3 ... 5 = J8. */
static constexpr uint8_t kCvVoct = 2u;

static constexpr int kNumModes = 9;

/* ---- hardware + engine -------------------------------------------------- */

static AlchemyLab          hw;
static daisy::CpuLoadMeter cpu;

/* The Modulator (123,296 B) in cached D2 SRAM: Warps kept it in the F4's
 * main SRAM, and the vocoder's 20-band filter bank and the delay line are
 * read every sample. Placement-new into zeroed memory, because Init()
 * leaves some previous_parameters_ fields untouched and relies on zeroed
 * statics (meld's lesson). */
static uint8_t MI_D2_BSS g_modmem[sizeof(warps::Modulator)];
static warps::Modulator* modulator = nullptr;

/* ---- colours ------------------------------------------------------------ */

static constexpr LedPanel::Rgb kColAlgo   = {0xFF, 0xA0, 0x30};
static constexpr LedPanel::Rgb kColTimbre = {0x40, 0xC0, 0xFF};
static constexpr LedPanel::Rgb kColLvl1   = {0x60, 0xFF, 0x90};
static constexpr LedPanel::Rgb kColLvl2   = {0xFF, 0x60, 0xB0};
static constexpr LedPanel::Rgb kColMode   = {0xC0, 0x80, 0xFF};
static constexpr LedPanel::Rgb kColCarr   = {0xFF, 0xFF, 0x80};
static constexpr LedPanel::Rgb kColGain   = {0xFF, 0xFF, 0xFF};
static constexpr LedPanel::Rgb kColTuneU  = {0xFF, 0xC0, 0x60};
static constexpr LedPanel::Rgb kColTuneD  = {0x60, 0xC0, 0xFF};
static constexpr LedPanel::Rgb kColNotch  = {0xFF, 0xFF, 0xFF};
static constexpr LedPanel::Rgb kColDim    = {0x18, 0x18, 0x18};
static constexpr LedPanel::Rgb kColSetup  = {0x30, 0x20, 0x10};

static constexpr LedPanel::Rgb kIdle = {0x00, 0x00, 0x30};
static constexpr LedPanel::Rgb kRed  = {0xFF, 0x00, 0x00};

/* Warps' button LED for the internal carrier: off = external (the jack),
 * then green, yellow, red for sine, triangle, saw. In Doppler the same four
 * states are the room size, in Delay the loop topology. */
static constexpr LedPanel::Rgb kCarrierLed[4] = {
    {0x00, 0x00, 0x30}, {0x00, 0xFF, 0x00}, {0xFF, 0xC0, 0x00}, {0xFF, 0x00, 0x00}};

/* One colour per mode for B2, ordered as the MODE selector. */
static constexpr LedPanel::Rgb kModeLed[kNumModes] = {
    {0xFF, 0xFF, 0xFF},   /* Meta: stock Warps */
    {0x00, 0xC0, 0xFF},   /* Doppler */
    {0xFF, 0x80, 0x00},   /* Fold */
    {0xFF, 0x40, 0x40},   /* Chebyshev */
    {0x40, 0xFF, 0x40},   /* Frequency shifter */
    {0xFF, 0xFF, 0x00},   /* Bitcrusher */
    {0xFF, 0x00, 0xFF},   /* Comparator */
    {0x80, 0x40, 0xFF},   /* Vocoder */
    {0x00, 0xFF, 0xC0}};  /* Delay */

/* The MODE selector lists META first (the stock Warps, the natural home),
 * then Parasites' eight in their enum order. */
static const warps::FeatureMode kModeOrder[kNumModes] = {
    warps::FEATURE_MODE_META, warps::FEATURE_MODE_DOPPLER, warps::FEATURE_MODE_FOLD,
    warps::FEATURE_MODE_CHEBYSCHEV, warps::FEATURE_MODE_FREQUENCY_SHIFTER,
    warps::FEATURE_MODE_BITCRUSHER, warps::FEATURE_MODE_COMPARATOR,
    warps::FEATURE_MODE_VOCODER, warps::FEATURE_MODE_DELAY};
static const char* const kModeLabels[kNumModes] = {
    "Meta", "Doppler", "Fold", "Chebyshev", "Freq shift", "Bitcrush", "Comparator",
    "Vocoder", "Delay"};
static const char* const kCarrierLabels[4] = {"Ext / 1", "Sine / 2", "Tri / 3", "Saw / 4"};

/* ---- pages --------------------------------------------------------------- */

/* PLAY. Panel positions, front view:
 *      [P1] B1 [P2]
 *      [P3] B2 [P4]
 *      [P5] B3 [P6]                                                        */

static VirtualKnob algorithm = VirtualKnob(kPotTopLeft, "Algorithm")
    .Linear(0.f, 1.f).Ident("algorithm")
    .Help("Warps' big knob. In META it sweeps the stock algorithms: "
          "crossfade, fold, analog ring mod, digital ring mod, XOR, "
          "comparator. In the Parasites modes it is that mode's main "
          "control (see the manual). J3 adds CV.")
    .Ring(Level(kColAlgo));

static VirtualKnob timbre = VirtualKnob(kPotTopRight, "Timbre")
    .Linear(0.f, 1.f).Ident("timbre")
    .Help("Warps' small knob: the algorithm's parameter. J4 adds CV.")
    .Ring(Level(kColTimbre));

static VirtualKnob level1 = VirtualKnob(kPotMiddleLeft, "Level 1")
    .Linear(0.f, 1.f).Ident("level1")
    .Help("Carrier level (drive = level squared, as on Warps). With the "
          "internal carrier on, also its pitch: 60 semitones of travel, "
          "plus V/Oct on J5. J6 adds CV.")
    .Ring(Level(kColLvl1));

static VirtualKnob level2 = VirtualKnob(kPotMiddleRight, "Level 2")
    .Linear(0.f, 1.f).Ident("level2")
    .Help("Modulator level (drive = level squared). J7 adds CV.")
    .Ring(Level(kColLvl2));

static VirtualKnob mode = VirtualKnob(kPotBottomLeft, "Mode")
    .Selector(kNumModes).Labels(kModeLabels).Ident("mode")
    .Help("META is stock Warps; the rest are Parasites' modes. B2 steps "
          "through them too.")
    .Ring(SelectorRing(kColMode, kColDim, kNumModes));

static VirtualKnob carrier = VirtualKnob(kPotBottomRight, "Carrier")
    .Selector(4).Labels(kCarrierLabels).Ident("carrier")
    .Help("Warps' button. Usually the internal carrier: external (J1), "
          "sine, triangle, saw. In Doppler it is the room size, in Delay "
          "the loop topology (open, dual, tape, ping-pong). B1 steps it.")
    .Ring(SelectorRing(kColCarr, kColDim, 4));

/* SETUP: hold B3. */

static VirtualKnob in_gain = VirtualKnob(kPotTopLeft, "In gain")
    .Linear(0.f, 1.f).Ident("in_gain")
    .Help("Input gain for J1 and J2, -12 dB to +12 dB, 0 dB at noon. Warps' "
          "drive, fold and comparator respond to level, so this changes "
          "the sound, not just the volume.")
    .Ring(Bipolar(kColGain, kColGain, kColNotch));

static VirtualKnob tune = VirtualKnob(kPotTopRight, "Tune")
    .Linear(-1.f, 1.f).Ident("tune")
    .Help("Internal carrier transpose, -24 to +24 semitones in semitone "
          "steps, 0 at noon.")
    .Ring(Bipolar(kColTuneU, kColTuneD, kColNotch));

static Page play_page = Page(kPagePlay).Name("Play").Color("#ffa030")
    .Knobs(algorithm, timbre, level1, level2, mode, carrier);

static Page setup_page = Page(kPageSetup).Name("Setup").Color("#302010")
    .Knobs(in_gain, tune);

/* ---- descriptor metadata -------------------------------------------------- */

static Jack jk_carrier ("J1",  "Carrier in",   JackSig::AudioIn);
static Jack jk_modin   ("J2",  "Modulator in", JackSig::AudioIn);
static Jack jk_cv_alg  ("J3",  "CV Algorithm", JackSig::CvBi);
static Jack jk_cv_tim  ("J4",  "CV Timbre",    JackSig::CvBi);
static Jack jk_voct    ("J5",  "V/Oct",        JackSig::CvBi);
static Jack jk_cv_l1   ("J6",  "CV Level 1",   JackSig::CvBi);
static Jack jk_cv_l2   ("J7",  "CV Level 2",   JackSig::CvBi);
static Jack jk_cv_free ("J8",  "CV (free)",    JackSig::CvBi);
static Jack jk_out     ("J9",  "Out",          JackSig::AudioOut);
static Jack jk_aux     ("J10", "Aux",          JackSig::AudioOut);

static VirtualButton bt_carrier = VirtualButton("b1", "Carrier")
    .Action("Tap", "Next CARRIER state (Warps' button)");

static VirtualButton bt_mode = VirtualButton("b2", "Mode")
    .Action("Tap", "Next MODE");

static VirtualButton bt_setup = VirtualButton("b3", "Setup")
    .Action("Hold", "Show the Setup page");

static Manual manual = Manual()
    .Tagline("Meta-modulator: Warps with the Parasites modes")
    .Preamble(
        "**Warps** combines a carrier on J1 with a modulator on J2. In META "
        "the ALGORITHM knob sweeps the stock Warps algorithms (crossfade, "
        "fold, analog and digital ring modulation, XOR, comparator) and "
        "TIMBRE is each one's parameter. LEVEL 1 and LEVEL 2 drive the two "
        "inputs. CARRIER (or B1) switches on the internal oscillator, which "
        "LEVEL 1 then also tunes, with V/Oct on J5. MODE (or B2) picks one "
        "of the Parasites modes: Doppler, fold, Chebyshev, frequency "
        "shifter, bitcrusher, comparator, vocoder and delay, each giving "
        "the four knobs its own meaning. Out on J9, aux on J10. The DSP is "
        "Mutable Instruments' Warps (Emilie Gillet) with Matthias Puech's "
        "Parasites, MIT.");

/* ---- surfaces ------------------------------------------------------------ */

static ControlLoop loop(hw);
static Pager       pager = Pager(kNumAppPages, kNumPots)
                               .Shift(hw.buttons[kButtonB3], kPageSetup);

/* Home slot 7: see mi_family.h for the whole card's map. */
static mi::HomeSlot home(7u);
static Presets      presets(hw.seed.qspi);
static Settings     settings(hw, &pager);
static CvMatrix     cv_matrix(kNumCvInputs);
static SdCard       sd;
static hostlink::FsExtension fs_ext(sd);
static hostlink::Host host(presets, "warps_alchemy", "Warps",
                           MI_VERSION, MI_GIT_HASH);
static mi::CpuExtras extras(0x575250u);   /* 'WRP' */

#ifdef MI_BENCH_USB
static daisy::UsbHandle          bench_usb;
static hostlink::CdcUsbTransport bench_cdc;
#endif

/* ---- control frame -> audio block ---------------------------------------- */

/* Written by the control thread at the frame rate, smoothed per block in
 * the callback with cv_scaler.cc's pot coefficients (0.33 x its BIND lp:
 * levels 0.5, algorithm and parameter 0.08) at the same 1.6 kHz. */
struct Targets
{
    volatile float algorithm = 0.f, timbre = 0.5f, level1 = 0.5f, level2 = 0.5f;
    volatile float gain = 1.f, tune = 0.f;
    volatile int   carrier = 0;
};
static Targets T;

static volatile int G_MODE_IDX = 0;   /* index into kModeOrder, for the LEDs */

/* ---- buttons (1 ms poll) -------------------------------------------------- */

static mi::Swallow swallow;
static bool        g_b1_down = false, g_b2_down = false;
static volatile bool g_carrier_tap = false, g_mode_tap = false;

static void OnPoll(uint32_t now)
{
    (void)now;
    if (settings.IsActive())
    {
        g_b1_down = g_b2_down = true;   /* no tap from the press that leaves */
        swallow.All();
        return;
    }
    const bool b1 = swallow.Live(hw, kButtonB1);
    if (b1 && !g_b1_down) g_carrier_tap = true;
    g_b1_down = b1;

    /* B2 stands down while B3 is held: B2+B3 held two seconds is the
     * Settings chord. */
    const bool b3 = swallow.Live(hw, kButtonB3);
    const bool b2 = !b3 && swallow.Live(hw, kButtonB2);
    if (b2 && !g_b2_down) g_mode_tap = true;
    g_b2_down = b2;
}

/* ---- control frame (~60 Hz) ------------------------------------------------ */

static mi::StoredWatch<kNumPots> play_watch, setup_watch;
static float g_saved_peak  = 0.0f;
static bool  prev_settings = false;
static int   applied_mode  = -1;

/* Step a PLAY selector from a button: the stored value moves, so it saves
 * and the ring shows it, and the pot re-arms its catch. */
static void step_selector(uint8_t pot, int value, int zones)
{
    float phys[kNumPots];
    for (uint8_t p = 0; p < kNumPots; p++) phys[p] = hw.pots[p].Value();
    pager.SetStored(kPagePlay, pot, ((float)value + 0.5f) / (float)zones, phys);
}

static void OnFrame(void)
{
    const uint32_t now = System::GetNow();

    if (g_carrier_tap)
    {
        g_carrier_tap = false;
        step_selector(kPotBottomRight, ((int)carrier.Value() + 1) & 3, 4);
    }
    if (g_mode_tap)
    {
        g_mode_tap = false;
        step_selector(kPotBottomLeft, ((int)mode.Value() + 1) % kNumModes, kNumModes);
    }

    if (play_watch.Changed(pager, kPagePlay)) home.Touch(now);
    if (setup_watch.Changed(pager, kPageSetup)) home.Touch(now);

    /* Values are catch + locks + CV, already mixed by the SDK. */
    T.algorithm = mi::clampf(algorithm.Value(), 0.f, 1.f);
    T.timbre    = mi::clampf(timbre.Value(), 0.f, 1.f);
    T.level1    = mi::clampf(level1.Value(), 0.f, 1.f);
    T.level2    = mi::clampf(level2.Value(), 0.f, 1.f);
    T.gain      = powf(4.0f, 2.0f * mi::clampf(in_gain.Value(), 0.f, 1.f) - 1.0f);
    T.tune      = roundf(24.0f * mi::clampf(tune.Value(), -1.f, 1.f));

    const int c = (int)carrier.Value();
    if (c >= 0 && c < 4) T.carrier = c;

    /* set_feature_mode() from the control thread, as Parasites' own UI
     * calls it from its main loop. */
    const int m = (int)mode.Value();
    if (m != applied_mode && m >= 0 && m < kNumModes)
    {
        applied_mode = m;
        G_MODE_IDX   = m;
        modulator->set_feature_mode(kModeOrder[m]);
    }

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
 *   B1 pair   the CARRIER state in Warps' button colours: dim = external,
 *             green / yellow / red = states 2 / 3 / 4
 *   B2 pair   the MODE's colour; red when the callback averages over 80 %
 *   B3 pair   dim Setup tint
 *   P1 ring   the previous session's worst CPU load, 2.5 s at boot
 */
static uint32_t g_readout_until = 0;
static float    g_boot_peak     = 0.0f;

static void OnRender(uint32_t t_ms)
{
    LedPanel& L = hw.leds;
    if (t_ms < g_readout_until) mi::paint_cpu_readout(hw, g_boot_peak);
    if (settings.IsActive()) return;

    L.SetButtonPair(kButtonB1, L.ScaleGlobal(kCarrierLed[T.carrier & 3]));
    const int mi_ = G_MODE_IDX;
    L.SetButtonPair(kButtonB2, L.ScaleGlobal(cpu.GetAvgCpuLoad() > 0.80f
                                                 ? kRed
                                                 : kModeLed[mi_ >= 0 && mi_ < kNumModes ? mi_ : 0]));
    if (pager.Page() != kPageSetup)
        L.SetButtonPair(kButtonB3, L.ScaleGlobal(kColSetup));
}

/* ---- audio ----------------------------------------------------------------- */

static warps::ShortFrame s_in[kBlockSize], s_out[kBlockSize];

static mi_warps::Panel s_panel;
static float           s_note_cv = 0.f;

static void AudioCallback(daisy::AudioHandle::InputBuffer  in,
                          daisy::AudioHandle::OutputBuffer out,
                          size_t                           size)
{
    cpu.OnBlockStart();

    if (size > kBlockSize)
    {
        for (size_t i = 0; i < size; i++) { out[0][i] = in[0][i]; out[1][i] = in[1][i]; }
        cpu.OnBlockEnd();
        return;
    }

    /* Parasites' cv_scaler smoothing, once per 60-frame block as there. */
    s_panel.level[0]  += 0.165f  * (T.level1    - s_panel.level[0]);
    s_panel.level[1]  += 0.165f  * (T.level2    - s_panel.level[1]);
    s_panel.algorithm += 0.0264f * (T.algorithm - s_panel.algorithm);
    s_panel.timbre    += 0.0264f * (T.timbre    - s_panel.timbre);
    s_panel.carrier    = T.carrier;
    s_panel.tune       = T.tune;

    /* V/Oct on J5: jumps of more than 0.4 st land at once, smaller glide
     * (upstream's rule for its LEVEL 1 CV-as-pitch input). */
    const float note = 12.0f * hw.cv_jacks[kCvVoct].Volts();
    if (fabsf(note - s_note_cv) > 0.4f) s_note_cv = note;
    else s_note_cv += 0.1f * (note - s_note_cv);
    s_panel.voct = s_note_cv / 12.0f;

    mi_warps::Map(s_panel, modulator->mutable_parameters());

    const float gain = T.gain;
    for (size_t i = 0; i < size; i++)
    {
        s_in[i].l = mi::f2i(in[0][i] * gain);
        s_in[i].r = mi::f2i(in[1][i] * gain);
    }

    modulator->Process(s_in, s_out, size);

    const float k = 1.0f / 32768.0f;
    for (size_t i = 0; i < size; i++)
    {
        out[0][i] = (float)s_out[i].l * k;   /* main */
        out[1][i] = (float)s_out[i].r * k;   /* aux  */
    }

    cpu.OnBlockEnd();
}

/* ---- boot ------------------------------------------------------------------ */

int main(void)
{
    hw.Init(daisy::SaiHandle::Config::SampleRate::SAI_96KHZ, kBlockSize);
    cpu.Init(hw.SampleRate(), (int)hw.BlockSize());
    if (hw.BlockSize() != kBlockSize) mi::fault_forever(hw);

    memset(g_modmem, 0, sizeof g_modmem);
    modulator = new (g_modmem) warps::Modulator();
    modulator->Init(hw.SampleRate());

    sd.Init();
    picker::Install(settings, kSettingsFirmware, sd, hw);
    settings.UseBrightness();
    settings.UsePresets(presets);

    /* CV: J5 is V/Oct, read raw in the callback; J3/J4/J6/J7 add into their
     * knobs through the SDK's matrix (re-routable from the web programmer);
     * J8 is left for the user to route. */
    cv_matrix.Jack(0).To(algorithm);
    cv_matrix.Jack(1).To(timbre);
    cv_matrix.Jack(2).Off();
    cv_matrix.Jack(3).To(level1);
    cv_matrix.Jack(4).To(level2);
    cv_matrix.Jack(5).Off();

    host.Product("Warps")
        .BootSlot(home.slot)
        .Pages(play_page, setup_page)
        .Jacks(jk_carrier, jk_modin, jk_cv_alg, jk_cv_tim, jk_voct, jk_cv_l1,
               jk_cv_l2, jk_cv_free, jk_out, jk_aux)
        .Buttons(bt_carrier, bt_mode, bt_setup)
        .Attach(manual)
        .Extend(fs_ext);
#ifdef MI_BENCH_USB
    bench_cdc.Init(bench_usb, daisy::UsbHandle::FS_INTERNAL, "Warps (bench)");
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
        /* First boot after a flash: unity gain, no transpose. The PLAY page
         * adopts the pots, except MODE (META) and CARRIER (external), so a
         * fresh module is stock Warps on its jacks whatever the pots say. */
        pager.SetStored(kPageSetup, kPotTopLeft,  0.5f, phys);
        pager.SetStored(kPageSetup, kPotTopRight, 0.5f, phys);
        for (uint8_t p = 0; p < kNumPots; p++)
            pager.SetStored(kPagePlay, p, phys[p], phys);
        pager.SetStored(kPagePlay, kPotBottomLeft,  0.5f / (float)kNumModes, phys);
        pager.SetStored(kPagePlay, kPotBottomRight, 0.5f / 4.0f, phys);
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
