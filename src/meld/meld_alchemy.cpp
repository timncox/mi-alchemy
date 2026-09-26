/*
 * Meld on the Hermetic Modular Alchemy Lab V2 -- Warps Parasites, plus stock
 * Warps as a tenth mode, plus Meld's extra outputs: a coincidence clock and
 * two envelope / pitch CVs. Ported from ~/tim-os/meld (the Daisy Patch
 * version), minus what the Lab cannot do: Meld's second modulator pair (the
 * Lab has two audio ins and two outs) and its Lissajous scope (no screen).
 *
 * The DSP: warps::Modulator from mqtthiqs/parasites @ 32fa66f (shared with
 * src/warps) and warps_stock::Modulator from pichenettes/eurorack warps @
 * 3c23d03, renamed by tools/vendor_stock_warps.py (vendor/VENDOR.md). One
 * runs at a time, at 96 kHz with Warps' 60-frame block.
 *
 *   PLAY   ALGORITHM . TIMBRE . LEVEL 1 . LEVEL 2 . MODE (10) . CARRIER
 *   SETUP  IN GAIN . TUNE . CV 2 SOURCE                     (hold B3)
 *   B1     next CARRIER state     B2  next MODE
 *   J1 carrier in . J2 modulator in . J3 CV algorithm . J4 CV timbre
 *   J5 V/Oct (internal carrier) . J6 CV 2 out . J7 gate out . J8 CV 1 out
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
#include "warps_stock/dsp/modulator.h"
#include "meld_params.h"

#ifndef MI_VERSION
#define MI_VERSION "0.0.0"
#endif
#ifndef MI_GIT_HASH
#define MI_GIT_HASH "dev"
#endif

using namespace alchemy;
using daisy::System;
using mi_meld::kNumModes;
using mi_meld::kModeStock;

/* ---- geometry ----------------------------------------------------------- */

static constexpr uint32_t kBlockSize = 60u;
static_assert(kBlockSize <= warps::kMaxBlockSize, "block larger than Warps expects");
static_assert(kBlockSize <= warps_stock::kMaxBlockSize, "block larger than stock Warps expects");

enum : uint8_t { kPagePlay = 0, kPageSetup = 1, kNumAppPages = 2 };
enum : uint8_t { kSettingsMain = 0, kSettingsFirmware = 1 };

/* CV indices: 0 = J3 ... 5 = J8. J6 is on the MCP4728 (I2C, flushed from
 * the 1 ms poll); J7 and J8 are the STM32's own DAC (written per block). */
static constexpr uint8_t kCvVoct = 2u;   /* J5 */
static constexpr uint8_t kJackCv2 = 3u;  /* J6 */
static constexpr uint8_t kJackGate = 4u; /* J7 */
static constexpr uint8_t kJackCv1 = 5u;  /* J8 */

/* Meld's 5 ms coincidence gate in 60-frame blocks at 96 kHz (0.625 ms each). */
static constexpr uint32_t kGateBlocks =
    (mi_meld::Extras::kGateMs * 96000u / 1000u + kBlockSize - 1u) / kBlockSize;

/* ---- hardware + engines ------------------------------------------------- */

static AlchemyLab          hw;
static daisy::CpuLoadMeter cpu;

/* Both modulators in cached D2 SRAM (123 KB + 73 KB of its 224 KB). Only
 * the one the MODE selects runs. Placement-new into zeroed memory: Init()
 * leaves some previous_parameters_ fields to the zeroed statics Warps had
 * (meld's lesson). One attribute list, so the alignment is not dropped. */
static uint8_t MI_D2_BSS g_modmem[sizeof(warps::Modulator)];
static uint8_t MI_D2_BSS g_stockmem[sizeof(warps_stock::Modulator)];
static warps::Modulator*       modulator = nullptr;
static warps_stock::Modulator* stock     = nullptr;

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
static constexpr LedPanel::Rgb kColCv2    = {0x80, 0xFF, 0xFF};
static constexpr LedPanel::Rgb kColNotch  = {0xFF, 0xFF, 0xFF};
static constexpr LedPanel::Rgb kColDim    = {0x18, 0x18, 0x18};
static constexpr LedPanel::Rgb kColSetup  = {0x10, 0x28, 0x30};

static constexpr LedPanel::Rgb kWhite = {0xFF, 0xFF, 0xFF};
static constexpr LedPanel::Rgb kRed   = {0xFF, 0x00, 0x00};

/* Warps' button LED: off = external, then green / yellow / red. */
static constexpr LedPanel::Rgb kCarrierLed[4] = {
    {0x00, 0x00, 0x30}, {0x00, 0xFF, 0x00}, {0xFF, 0xC0, 0x00}, {0xFF, 0x00, 0x00}};

/* One colour per mode for B2, in the MODE selector's order. */
static constexpr LedPanel::Rgb kModeLed[kNumModes] = {
    {0xFF, 0xFF, 0xFF},   /* Meta */
    {0x00, 0xC0, 0xFF},   /* Doppler */
    {0xFF, 0x80, 0x00},   /* Fold */
    {0xFF, 0x40, 0x40},   /* Chebyshev */
    {0x40, 0xFF, 0x40},   /* Frequency shifter */
    {0xFF, 0xFF, 0x00},   /* Bitcrusher */
    {0xFF, 0x00, 0xFF},   /* Comparator */
    {0x80, 0x40, 0xFF},   /* Vocoder */
    {0x00, 0xFF, 0xC0},   /* Delay */
    {0xFF, 0xB0, 0x80}};  /* Warps (stock) */

static const char* const kModeLabels[kNumModes] = {
    "Meta", "Doppler", "Fold", "Chebyshev", "Freq shift", "Bitcrush", "Comparator",
    "Vocoder", "Delay", "Warps"};
static const char* const kCarrierLabels[4] = {"Ext / 1", "Sine / 2", "Tri / 3", "Saw / 4"};
static const char* const kCv2Labels[3] = {"Auto", "Envelope", "Note"};

/* ---- pages --------------------------------------------------------------- */

/* PLAY. Panel positions, front view:
 *      [P1] B1 [P2]
 *      [P3] B2 [P4]
 *      [P5] B3 [P6]                                                        */

static VirtualKnob algorithm = VirtualKnob(kPotTopLeft, "Algorithm")
    .Linear(0.f, 1.f).Ident("algorithm")
    .Help("Warps' big knob. In META it sweeps the stock algorithms "
          "(crossfade, fold, analog and digital ring mod, XOR, comparator); "
          "in WARPS it goes on past the comparator into the vocoder and its "
          "release time, and the last hair freezes the vocoder's spectrum. "
          "In the Parasites modes it is that mode's main control. J3 adds CV.")
    .Ring(Level(kColAlgo));

static VirtualKnob timbre = VirtualKnob(kPotTopRight, "Timbre")
    .Linear(0.f, 1.f).Ident("timbre")
    .Help("Warps' small knob: the algorithm's parameter. J4 adds CV.")
    .Ring(Level(kColTimbre));

static VirtualKnob level1 = VirtualKnob(kPotMiddleLeft, "Level 1")
    .Linear(0.f, 1.f).Ident("level1")
    .Help("Carrier level (drive = level squared, as on Warps). With the "
          "internal carrier on, also its pitch: 60 semitones of travel, "
          "plus V/Oct on J5.")
    .Ring(Level(kColLvl1));

static VirtualKnob level2 = VirtualKnob(kPotMiddleRight, "Level 2")
    .Linear(0.f, 1.f).Ident("level2")
    .Help("Modulator level (drive = level squared).")
    .Ring(Level(kColLvl2));

static VirtualKnob mode = VirtualKnob(kPotBottomLeft, "Mode")
    .Selector(kNumModes).Labels(kModeLabels).Ident("mode")
    .Help("META is Parasites' copy of the stock algorithms; the next eight "
          "are Parasites' modes; WARPS, the last, is stock Mutable Warps "
          "itself. B2 steps through them too.")
    .Ring(SelectorRing(kColMode, kColDim, kNumModes));

static VirtualKnob carrier = VirtualKnob(kPotBottomRight, "Carrier")
    .Selector(4).Labels(kCarrierLabels).Ident("carrier")
    .Help("Warps' button. Usually the internal carrier: external (J1), "
          "sine, triangle, saw. In Doppler it is the room size, in Delay "
          "the loop topology. B1 steps it.")
    .Ring(SelectorRing(kColCarr, kColDim, 4));

/* SETUP: hold B3. */

static VirtualKnob in_gain = VirtualKnob(kPotTopLeft, "In gain")
    .Linear(0.f, 1.f).Ident("in_gain")
    .Help("Input gain for J1 and J2, -12 dB to +12 dB, 0 dB at noon. It "
          "also scales what the envelopes and the coincidence clock see.")
    .Ring(Bipolar(kColGain, kColGain, kColNotch));

static VirtualKnob tune = VirtualKnob(kPotTopRight, "Tune")
    .Linear(-1.f, 1.f).Ident("tune")
    .Help("Internal carrier transpose, -24 to +24 semitones in semitone "
          "steps, 0 at noon.")
    .Ring(Bipolar(kColTuneU, kColTuneD, kColNotch));

static VirtualKnob cv2_src = VirtualKnob(kPotMiddleLeft, "CV 2")
    .Selector(3).Labels(kCv2Labels).Ident("cv2_source")
    .Help("What J6 puts out. AUTO (Meld's rule): the internal carrier's "
          "note, 1 V/oct from C2, while CARRIER is internal, else the "
          "carrier's envelope. ENVELOPE: always the envelope, 0-5 V. "
          "NOTE: always the note.")
    .Ring(SelectorRing(kColCv2, kColDim, 3));

static Page play_page = Page(kPagePlay).Name("Play").Color("#ffa030")
    .Knobs(algorithm, timbre, level1, level2, mode, carrier);

static Page setup_page = Page(kPageSetup).Name("Setup").Color("#102830")
    .Knobs(in_gain, tune, cv2_src);

/* ---- descriptor metadata -------------------------------------------------- */

static Jack jk_carrier ("J1",  "Carrier in",   JackSig::AudioIn);
static Jack jk_modin   ("J2",  "Modulator in", JackSig::AudioIn);
static Jack jk_cv_alg  ("J3",  "CV Algorithm", JackSig::CvBi);
static Jack jk_cv_tim  ("J4",  "CV Timbre",    JackSig::CvBi);
static Jack jk_voct    ("J5",  "V/Oct",        JackSig::CvBi);
static Jack jk_cv2     ("J6",  "CV 2 out",     JackSig::CvUni);
static Jack jk_gate    ("J7",  "Gate out",     JackSig::Gate);
static Jack jk_cv1     ("J8",  "CV 1 out",     JackSig::CvUni);
static Jack jk_out     ("J9",  "Out",          JackSig::AudioOut);
static Jack jk_aux     ("J10", "Aux",          JackSig::AudioOut);

static VirtualButton bt_carrier = VirtualButton("b1", "Carrier")
    .Action("Tap", "Next CARRIER state (Warps' button)");

static VirtualButton bt_mode = VirtualButton("b2", "Mode")
    .Action("Tap", "Next MODE");

static VirtualButton bt_setup = VirtualButton("b3", "Setup")
    .Action("Hold", "Show the Setup page");

static Manual manual = Manual()
    .Tagline("Warps Parasites and stock Warps, with a coincidence clock and envelope CVs")
    .Preamble(
        "**Meld** combines a carrier on J1 with a modulator on J2, like "
        "Warps, and adds three outputs. J7 is a gate that fires when the two "
        "inputs cross zero together, a clock that follows how their "
        "frequencies relate. J8 follows the modulator's envelope, and J6 "
        "follows the carrier's envelope or plays the internal carrier's "
        "note at 1 V/oct (SETUP P3). MODE (or B2) picks META, the eight "
        "Parasites modes, or WARPS, stock Mutable Warps itself, whose big "
        "knob runs on past the comparator into the vocoder. CARRIER (or B1) "
        "switches the internal oscillator, which LEVEL 1 tunes with V/Oct on "
        "J5. Out on J9, aux on J10. The DSP is Mutable Instruments' Warps "
        "(Emilie Gillet) and Matthias Puech's Parasites, MIT.");

/* ---- surfaces ------------------------------------------------------------ */

static ControlLoop loop(hw);
static Pager       pager = Pager(kNumAppPages, kNumPots)
                               .Shift(hw.buttons[kButtonB3], kPageSetup);

/* Home slot 6: see mi_family.h for the whole card's map. */
static mi::HomeSlot home(6u);
static Presets      presets(hw.seed.qspi);
static Settings     settings(hw, &pager);
static CvMatrix     cv_matrix(kNumCvInputs);
static SdCard       sd;
static hostlink::FsExtension fs_ext(sd);
static hostlink::Host host(presets, "meld_alchemy", "Meld",
                           MI_VERSION, MI_GIT_HASH);
static mi::CpuExtras extras(0x4D4C44u);   /* 'MLD' */

#ifdef MI_BENCH_USB
static daisy::UsbHandle          bench_usb;
static hostlink::CdcUsbTransport bench_cdc;
#endif

/* ---- control frame -> audio block ---------------------------------------- */

struct Targets
{
    volatile float algorithm = 0.f, timbre = 0.5f, level1 = 0.5f, level2 = 0.5f;
    volatile float gain = 1.f, tune = 0.f;
    volatile int   carrier = 0, cv2_source = mi_meld::CV2_AUTO;
};
static Targets T;

static volatile int   G_MODE_IDX = 0;    /* the MODE selector index */
static volatile float G_CV2      = 0.f;  /* volts for J6, set per block */
static volatile uint32_t G_GATE_T = 0;   /* last coincidence, ms */

/* ---- buttons + the I2C output (1 ms poll) --------------------------------- */

static mi::Swallow swallow;
static bool        g_b1_down = false, g_b2_down = false;
static volatile bool g_carrier_tap = false, g_mode_tap = false;
static float       g_cv2_staged = 99.f;

static void OnPoll(uint32_t now)
{
    (void)now;

    /* J6 is on the MCP4728: stage and latch only when it moved a DAC step,
     * from here, never from the audio callback (src/marbles' rule). */
    const float v = G_CV2;
    if (fabsf(v - g_cv2_staged) >= 0.0012f)
    {
        g_cv2_staged = v;
        hw.cv_jacks[kJackCv2].StageVolts(v);
        hw.FlushCvOutputs();
    }

    if (settings.IsActive())
    {
        g_b1_down = g_b2_down = true;
        swallow.All();
        return;
    }
    const bool b1 = swallow.Live(hw, kButtonB1);
    if (b1 && !g_b1_down) g_carrier_tap = true;
    g_b1_down = b1;

    /* B2 stands down while B3 is held: B2+B3 is the Settings chord. */
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

    T.algorithm = mi::clampf(algorithm.Value(), 0.f, 1.f);
    T.timbre    = mi::clampf(timbre.Value(), 0.f, 1.f);
    T.level1    = mi::clampf(level1.Value(), 0.f, 1.f);
    T.level2    = mi::clampf(level2.Value(), 0.f, 1.f);
    T.gain      = powf(4.0f, 2.0f * mi::clampf(in_gain.Value(), 0.f, 1.f) - 1.0f);
    T.tune      = roundf(24.0f * mi::clampf(tune.Value(), -1.f, 1.f));
    const int s = (int)cv2_src.Value();
    if (s >= 0 && s < 3) T.cv2_source = s;

    const int c = (int)carrier.Value();
    if (c >= 0 && c < 4) T.carrier = c;

    /* The Parasites modulator's mode from the control thread, as Parasites'
     * own UI does; the stock mode needs no call (it has no modes). */
    const int m = (int)mode.Value();
    if (m != applied_mode && m >= 0 && m < kNumModes)
    {
        applied_mode = m;
        G_MODE_IDX   = m;
        if (m != kModeStock) modulator->set_feature_mode(mi_meld::kModeOrder[m]);
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
 *   B1 pair   the CARRIER state in Warps' button colours
 *   B2 pair   the MODE's colour; red when the callback averages over 80 %
 *   B3 pair   white for 30 ms on each coincidence (the J7 gate), else the
 *             dim Setup tint (Meld's gate LED)
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
        L.SetButtonPair(kButtonB3, L.ScaleGlobal(t_ms - G_GATE_T < 30u ? kWhite : kColSetup));
}

/* ---- audio ----------------------------------------------------------------- */

static warps::ShortFrame s_in[kBlockSize], s_out[kBlockSize];
static float             s_x[kBlockSize], s_y[kBlockSize];

static mi_warps::Panel     s_panel;
static warps::Parameters   s_params;
static mi_meld::Extras     s_extras;
static float               s_note_cv   = 0.f;
static uint32_t            s_gate_left = 0;   /* blocks the gate stays high */

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

    /* Parasites' cv_scaler smoothing, once per block (src/warps). */
    s_panel.level[0]  += 0.165f  * (T.level1    - s_panel.level[0]);
    s_panel.level[1]  += 0.165f  * (T.level2    - s_panel.level[1]);
    s_panel.algorithm += 0.0264f * (T.algorithm - s_panel.algorithm);
    s_panel.timbre    += 0.0264f * (T.timbre    - s_panel.timbre);
    s_panel.carrier    = T.carrier;
    s_panel.tune       = T.tune;

    const float note = 12.0f * hw.cv_jacks[kCvVoct].Volts();
    if (fabsf(note - s_note_cv) > 0.4f) s_note_cv = note;
    else s_note_cv += 0.1f * (note - s_note_cv);
    s_panel.voct = s_note_cv / 12.0f;

    mi_warps::Map(s_panel, &s_params);

    const float gain = T.gain;
    for (size_t i = 0; i < size; i++)
    {
        s_x[i]    = mi::clampf(in[0][i] * gain, -1.f, 1.f);
        s_y[i]    = mi::clampf(in[1][i] * gain, -1.f, 1.f);
        s_in[i].l = mi::f2i(s_x[i]);
        s_in[i].r = mi::f2i(s_y[i]);
    }

    if (G_MODE_IDX == kModeStock)
    {
        mi_meld::MapStock(s_params, stock->mutable_parameters());
        /* The two ShortFrame types share one layout (meld does the same). */
        stock->Process(reinterpret_cast<warps_stock::ShortFrame*>(s_in),
                       reinterpret_cast<warps_stock::ShortFrame*>(s_out), size);
    }
    else
    {
        *modulator->mutable_parameters() = s_params;
        modulator->Process(s_in, s_out, size);
    }

    const float k = 1.0f / 32768.0f;
    for (size_t i = 0; i < size; i++)
    {
        out[0][i] = (float)s_out[i].l * k;
        out[1][i] = (float)s_out[i].r * k;
    }

    /* Meld's extras, on the inputs as the engine hears them. The gate and
     * CV 1 are the STM32 DAC (J7, J8): direct writes, once per block
     * (0.6 ms), as src/marbles writes T3. CV 2 goes to the poll (I2C). */
    if (s_extras.Process(s_x, s_y, size))
    {
        s_gate_left = kGateBlocks;
        G_GATE_T    = System::GetNow();
    }
    hw.cv_jacks[kJackGate].SetVolts(s_gate_left ? 5.0f : 0.0f);
    if (s_gate_left) s_gate_left--;
    hw.cv_jacks[kJackCv1].SetVolts(mi::clampf(5.0f * s_extras.env[1], 0.f, 5.f));
    G_CV2 = mi_meld::Cv2Volts(T.cv2_source, s_params.carrier_shape, s_extras.env[0], s_params.note);

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
    memset(g_stockmem, 0, sizeof g_stockmem);
    stock = new (g_stockmem) warps_stock::Modulator();
    stock->Init(hw.SampleRate());
    s_extras.Init(hw.SampleRate());

    /* J6 (MCP4728), J7 and J8 (STM32 DAC) become outputs, from 0 V. */
    hw.cv_jacks[kJackCv2].StageVolts(0.0f);
    hw.cv_jacks[kJackCv2].EnableCvOutput();
    hw.FlushCvOutputs();
    g_cv2_staged = 0.0f;
    for (uint8_t j : {kJackGate, kJackCv1})
    {
        hw.cv_jacks[j].SetVolts(0.0f);
        hw.cv_jacks[j].EnableCvOutput();
    }

    sd.Init();
    picker::Install(settings, kSettingsFirmware, sd, hw);
    settings.UseBrightness();
    settings.UsePresets(presets);

    /* CV: J3/J4 add into ALGORITHM/TIMBRE through the SDK's matrix
     * (re-routable in the web programmer); J5 is V/Oct, read raw; J6-J8 are
     * outputs. */
    cv_matrix.Jack(0).To(algorithm);
    cv_matrix.Jack(1).To(timbre);
    cv_matrix.Jack(2).Off();
    cv_matrix.Jack(3).Off();
    cv_matrix.Jack(4).Off();
    cv_matrix.Jack(5).Off();

    host.Product("Meld")
        .BootSlot(home.slot)
        .Pages(play_page, setup_page)
        .Jacks(jk_carrier, jk_modin, jk_cv_alg, jk_cv_tim, jk_voct, jk_cv2,
               jk_gate, jk_cv1, jk_out, jk_aux)
        .Buttons(bt_carrier, bt_mode, bt_setup)
        .Attach(manual)
        .Extend(fs_ext);
#ifdef MI_BENCH_USB
    bench_cdc.Init(bench_usb, daisy::UsbHandle::FS_INTERNAL, "Meld (bench)");
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
        /* First boot after a flash: unity gain, no transpose, CV 2 on AUTO.
         * The PLAY page adopts the pots, except MODE (META) and CARRIER
         * (external). */
        pager.SetStored(kPageSetup, kPotTopLeft,    0.5f, phys);
        pager.SetStored(kPageSetup, kPotTopRight,   0.5f, phys);
        pager.SetStored(kPageSetup, kPotMiddleLeft, 0.5f / 3.0f, phys);
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
