/*
 * Marbles on the Hermetic Modular Alchemy Lab V2 -- Mutable Instruments'
 * random sampler (T: three random gates; X: three random voltages, Y: one
 * slow one) as Alchemy SDK firmware.
 *
 * The DSP is marbles::TGenerator / XYGenerator, vendored byte-identical from
 * pichenettes/eurorack (vendor/VENDOR.md), run at its native 32 kHz. This
 * file does what marbles.cc's Process(), cv_reader.cc and ui.cc did, on six
 * pots, three buttons and ten jacks. See src/marbles/DESIGN.md.
 *
 *   PLAY   RATE . t BIAS . t JITTER . DEJA VU . X SPREAD . X BIAS
 *   SETUP  X STEPS . LENGTH . t MODE . X MODE . X RANGE . SCALE  (hold B3)
 *   B1     t DEJA VU: tap on/off, hold 2 s = locked (Marbles' gesture)
 *   B2     X DEJA VU: the same
 *   J1 t clock in . J2 X clock in . J8 CV in (default RATE, 1 V/oct)
 *   J3 X1 . J4 X2 . J5 X3 . J6 Y      (MCP4728, flushed from the 1 ms poll)
 *   J9 T1 . J10 T2 . J7 T3            (codec: per sample; STM32 DAC: per block)
 *   Settings (B2+B3 2 s): page 0 brightness, presets, t RANGE, J1 CLOCK,
 *   X CLOCK; page 1 the SD picker, J8 DEST, EXT X, GATE LENGTH;
 *   page 2 the Y section (spread, bias, steps, divider, range), GATE JITTER.
 */
#include <math.h>
#include <string.h>

#include "daisy_seed.h"
#include "per/rng.h"
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
#include "alchemy/surface/serializable.h"
#include "alchemy/surface/settings.h"
#include "alchemy/surface/virtual_button.h"
#include "alchemy/surface/virtual_knob.h"

#include "mi_family.h"
#include "picker.h"

#include "marbles_engine.h"

#ifndef MI_VERSION
#define MI_VERSION "0.0.0"
#endif
#ifndef MI_GIT_HASH
#define MI_GIT_HASH "dev"
#endif

using namespace alchemy;
using daisy::System;

/* ---- geometry ----------------------------------------------------------- */

/* 16 frames at 32 kHz = 0.5 ms. Upstream processed 5-frame blocks; the
 * generators have no block-size limit, and 16 keeps the callback overhead
 * low while holding T3 (the one gate on the STM32 DAC, updated once per
 * block) to 0.5 ms of jitter. */
static constexpr uint32_t kBlockSize = marbles_port::kMaxBlock;

enum : uint8_t { kPagePlay = 0, kPageSetup = 1, kNumAppPages = 2 };
enum : uint8_t { kSettingsMain = 0, kSettingsFirmware = 1, kSettingsY = 2 };

/* cv_jacks[] indices. */
static constexpr uint8_t kJackX1 = 0u, kJackX2 = 1u, kJackX3 = 2u, kJackY = 3u;
static constexpr uint8_t kJackT3 = 4u;   /* J7, STM32 DAC */
static constexpr uint8_t kJackCv = 5u;   /* J8, the one CV input */

/* ---- hardware + engine -------------------------------------------------- */

static AlchemyLab          hw;
static daisy::CpuLoadMeter cpu;
static marbles_port::Engine engine;

/* ---- colours ------------------------------------------------------------ */

static constexpr LedPanel::Rgb kColRate  = {0x40, 0xFF, 0x40};
static constexpr LedPanel::Rgb kColTBias = {0x40, 0xC0, 0xFF};
static constexpr LedPanel::Rgb kColJit   = {0xFF, 0x60, 0xA0};
static constexpr LedPanel::Rgb kColDvU   = {0x00, 0xFF, 0x60};
static constexpr LedPanel::Rgb kColDvD   = {0xFF, 0x80, 0x00};
static constexpr LedPanel::Rgb kColSpr   = {0xFF, 0xC0, 0x60};
static constexpr LedPanel::Rgb kColXBias = {0xC0, 0x60, 0xFF};
static constexpr LedPanel::Rgb kColSteps = {0x80, 0xFF, 0xFF};
static constexpr LedPanel::Rgb kColLen   = {0xFF, 0xFF, 0x80};
static constexpr LedPanel::Rgb kColSel   = {0xFF, 0x90, 0x20};
static constexpr LedPanel::Rgb kColNotch = {0xFF, 0xFF, 0xFF};
static constexpr LedPanel::Rgb kColDim   = {0x18, 0x18, 0x18};
static constexpr LedPanel::Rgb kColSetup = {0x10, 0x30, 0x40};

static constexpr LedPanel::Rgb kIdle  = {0x00, 0x00, 0x30};
static constexpr LedPanel::Rgb kGreen = {0x00, 0xFF, 0x00};
static constexpr LedPanel::Rgb kAmber = {0xFF, 0xA0, 0x00};
static constexpr LedPanel::Rgb kRed   = {0xFF, 0x00, 0x00};
static constexpr LedPanel::Rgb kWhite = {0xFF, 0xFF, 0xFF};

static const char* const kTModeLabels[6] = {
    "Coin toss", "Clusters", "Drums", "Independent", "Divider", "Three states"};
static const char* const kXModeLabels[3]  = {"Identical", "Bump", "Tilt"};
static const char* const kRangeLabels[3]  = {"+2 V", "+5 V", "+/-5 V"};
static const char* const kScaleLabels[6]  = {
    "Major", "Minor", "Pentatonic", "Pelog", "Raag Bhairav", "Raag Shri"};
static const char* const kTRangeLabels[3] = {"x1/4", "x1", "x4"};
static const char* const kJ1Labels[2]     = {"Auto", "Ignore"};
static const char* const kXClkLabels[6]   = {"Auto", "T1+T2+T3", "T1", "T2", "T3", "J2"};
static const char* const kCvDestLabels[8] = {
    "Rate", "t Bias", "t Jitter", "Deja vu", "X Spread", "X Bias", "X Steps", "Length"};
static const char* const kOnOffLabels[2]  = {"Off", "On"};
static const char* const kYDivLabels[12]  = {
    "1/64", "1/48", "1/32", "1/24", "1/16", "1/12", "1/8", "1/6", "1/4", "1/3", "1/2", "1/1"};

/* ---- pages --------------------------------------------------------------- */

/* PLAY. Panel positions, front view:
 *      [P1] B1 [P2]
 *      [P3] B2 [P4]
 *      [P5] B3 [P6]                                                        */

static VirtualKnob rate = VirtualKnob(kPotTopLeft, "Rate")
    .Linear(0.f, 1.f).Ident("t_rate")
    .Help("Clock rate, 10 octaves around 2 Hz at noon (x1 range). With a clock "
          "on J1 it multiplies / divides that clock instead. J8 adds 1 V/oct.")
    .Ring(Level(kColRate));

static VirtualKnob t_bias = VirtualKnob(kPotTopRight, "t Bias")
    .Linear(0.f, 1.f).Ident("t_bias")
    .Help("Which gate wins more often (T1 left, T3 right); in Drums and "
          "Divider it picks the pattern.")
    .Ring(Bipolar(kColTBias, kColTBias, kColNotch));

static VirtualKnob t_jitter = VirtualKnob(kPotMiddleLeft, "t Jitter")
    .Linear(0.f, 1.f).Ident("t_jitter")
    .Help("From a steady clock to a random, Poisson-like one.")
    .Ring(Level(kColJit));

static VirtualKnob deja_vu = VirtualKnob(kPotMiddleRight, "Deja vu")
    .Linear(0.f, 1.f).Ident("deja_vu")
    .Help("With a deja vu button on: fully left is new random values, noon "
          "loops the last LENGTH steps exactly, right permutes them.")
    .Ring(Bipolar(kColDvU, kColDvD, kColNotch));

static VirtualKnob x_spread = VirtualKnob(kPotBottomLeft, "X Spread")
    .Linear(0.f, 1.f).Ident("x_spread")
    .Help("Width of the X distribution: from one value to a wide spread.")
    .Ring(Level(kColSpr));

static VirtualKnob x_bias = VirtualKnob(kPotBottomRight, "X Bias")
    .Linear(0.f, 1.f).Ident("x_bias")
    .Help("Centre of the X distribution: low to high.")
    .Ring(Bipolar(kColXBias, kColXBias, kColNotch));

/* SETUP: hold B3. */

static VirtualKnob x_steps = VirtualKnob(kPotTopLeft, "X Steps")
    .Linear(0.f, 1.f).Ident("x_steps")
    .Help("Left of noon: smooth, then slewed random. Right: quantized to the "
          "SCALE, fewer notes as it turns.")
    .Ring(Bipolar(kColSteps, kColSteps, kColNotch));

static VirtualKnob length = VirtualKnob(kPotTopRight, "Length")
    .Linear(0.f, 1.f).Ident("dv_length")
    .Help("Deja vu loop length, 1 to 16 steps.")
    .Ring(Level(kColLen));

static VirtualKnob t_mode = VirtualKnob(kPotMiddleLeft, "t Mode")
    .Selector(6).Labels(kTModeLabels).Ident("t_mode")
    .Ring(SelectorRing(kColSel, kColDim, 6));

static VirtualKnob x_mode = VirtualKnob(kPotMiddleRight, "X Mode")
    .Selector(3).Labels(kXModeLabels).Ident("x_mode")
    .Help("How SPREAD / BIAS / STEPS reach X1-X3: identical, bump (X2 gets "
          "the most), tilt (X1 to X3).")
    .Ring(SelectorRing(kColSel, kColDim, 3));

static VirtualKnob x_range = VirtualKnob(kPotBottomLeft, "X Range")
    .Selector(3).Labels(kRangeLabels).Ident("x_range")
    .Ring(SelectorRing(kColSel, kColDim, 3));

static VirtualKnob scale = VirtualKnob(kPotBottomRight, "Scale")
    .Selector(6).Labels(kScaleLabels).Ident("x_scale")
    .Ring(SelectorRing(kColSel, kColDim, 6));

static Page play_page = Page(kPagePlay).Name("Play").Color("#40ff40")
    .Knobs(rate, t_bias, t_jitter, deja_vu, x_spread, x_bias);

static Page setup_page = Page(kPageSetup).Name("Setup").Color("#ff9020")
    .Knobs(x_steps, length, t_mode, x_mode, x_range, scale);

/* ---- descriptor metadata -------------------------------------------------- */

static Jack jk_tclk ("J1",  "t Clock", JackSig::Trig);
static Jack jk_xclk ("J2",  "X Clock", JackSig::Trig);
static Jack jk_x1   ("J3",  "X1",      JackSig::CvBi);
static Jack jk_x2   ("J4",  "X2",      JackSig::CvBi);
static Jack jk_x3   ("J5",  "X3",      JackSig::CvBi);
static Jack jk_y    ("J6",  "Y",       JackSig::CvBi);
static Jack jk_t3   ("J7",  "T3",      JackSig::Trig);
static Jack jk_cv   ("J8",  "CV",      JackSig::CvBi);
static Jack jk_t1   ("J9",  "T1",      JackSig::Trig);
static Jack jk_t2   ("J10", "T2",      JackSig::Trig);

static VirtualButton bt_tdv = VirtualButton("b1", "t Deja vu")
    .Action("Tap", "Deja vu on / off for the T gates")
    .Action("Hold 2 s", "Lock the T loop (deja vu fixed at noon)");

static VirtualButton bt_xdv = VirtualButton("b2", "X Deja vu")
    .Action("Tap", "Deja vu on / off for the X voltages")
    .Action("Hold 2 s", "Lock the X loop");

static VirtualButton bt_setup = VirtualButton("b3", "Setup")
    .Action("Hold", "Show the Setup page");

static Manual manual = Manual()
    .Tagline("Random sampler: three random gates, four random voltages")
    .Preamble(
        "**Marbles** generates random gates on T1-T3 (J9, J10, J7) and "
        "random voltages on X1-X3 (J3-J5) and Y (J6). RATE sets the clock "
        "(or multiplies / divides a clock on J1), t BIAS and t JITTER shape "
        "the gates, X SPREAD / BIAS / STEPS the voltages (STEPS right of "
        "noon quantizes to the SCALE). B1 and B2 switch DEJA VU on for T "
        "and X: the DEJA VU knob then replays the last LENGTH steps (noon "
        "loops them exactly; hold the button 2 s to lock). Hold B3 for "
        "the Setup page. J2 clocks X on its own; J8 is one CV input, to "
        "RATE by default (Settings chooses). The DSP is Mutable "
        "Instruments' Marbles (Emilie Gillet, MIT), unmodified.");

/* ---- surfaces ------------------------------------------------------------ */

static ControlLoop loop(hw);
static Pager       pager = Pager(kNumAppPages, kNumPots)
                               .Shift(hw.buttons[kButtonB3], kPageSetup);

/* Home slot 10: see mi_family.h for the whole card's map. */
static mi::HomeSlot home(10u);
static Presets      presets(hw.seed.qspi);
static Settings     settings(hw, &pager);
static CvMatrix     cv_matrix(kNumCvInputs);
static SdCard       sd;
static hostlink::FsExtension fs_ext(sd);
static hostlink::Host host(presets, "marbles_alchemy", "Marbles",
                           MI_VERSION, MI_GIT_HASH);
static mi::CpuExtras extras(0x4D5242u);   /* 'MRB' */
static SelectorHandle s_t_range, s_j1, s_xclk, s_cv_dest, s_ext_x, s_ydiv, s_yrange;
static KnobHandle     s_pw_mean, s_pw_std, s_yspread, s_ybias, s_ysteps;

#ifdef MI_BENCH_USB
static daisy::UsbHandle          bench_usb;
static hostlink::CdcUsbTransport bench_cdc;
#endif

/*
 * The two deja vu buttons' states (off / on / locked): Marbles kept them in
 * its settings, and so does the preset blob here. Separate from CpuExtras
 * so its layout stays the family's.
 */
struct DejaVuStates : public Serializable
{
    volatile uint8_t t = marbles::DEJA_VU_OFF;
    volatile uint8_t x = marbles::DEJA_VU_OFF;

    size_t SerializedSize() const override { return 2u; }
    void   Serialize(uint8_t* out) const override { out[0] = t; out[1] = x; }
    bool   Deserialize(const uint8_t* in) override
    {
        t = in[0] <= marbles::DEJA_VU_LOCKED ? in[0] : marbles::DEJA_VU_OFF;
        x = in[1] <= marbles::DEJA_VU_LOCKED ? in[1] : marbles::DEJA_VU_OFF;
        return true;
    }
    uint32_t SchemaHash() const override { return 0x4D524244u; }   /* 'MRBD' */
};
static DejaVuStates dv;

/* ---- control frame -> audio block ---------------------------------------- */

/* Written by the control thread at the frame rate, read by the callback.
 * Single words: no torn reads on the M7. */
static marbles_port::Controls C;

/* Read back by the LEDs and the 1 ms DAC flush. */
static volatile float    G_V[4];              /* X1 X2 X3 Y, volts */
static volatile bool     G_T[3];              /* T1 T2 T3 */
static volatile uint32_t G_T_EDGE_MS[3];

/* ---- buttons + the MCP4728 (1 ms poll) ------------------------------------ */

/*
 * Marbles' deja vu gesture, decided on release (ui.cc OnSwitchReleased):
 * from OFF a tap turns it ON, a 2 s hold LOCKED; from LOCKED any press goes
 * to ON; from ON a tap turns it OFF, a hold LOCKED.
 */
struct DvButton
{
    bool     down = false;
    uint32_t t0   = 0;

    void Poll(bool pressed, uint32_t now, volatile uint8_t& state, bool* changed)
    {
        if (pressed && !down) { down = true; t0 = now; }
        if (!pressed && down)
        {
            down            = false;
            const bool held = now - t0 >= 2000u;
            if (state == marbles::DEJA_VU_OFF)
                state = held ? marbles::DEJA_VU_LOCKED : marbles::DEJA_VU_ON;
            else if (state == marbles::DEJA_VU_LOCKED)
                state = marbles::DEJA_VU_ON;
            else
                state = held ? marbles::DEJA_VU_LOCKED : marbles::DEJA_VU_OFF;
            *changed = true;
        }
    }
    void Reset() { down = false; }
};

static DvButton    b_tdv, b_xdv;
static mi::Swallow swallow;
static bool        g_dv_changed = false;

/*
 * X1-X3 and Y are on the MCP4728, an I2C DAC: never written from the audio
 * callback. The callback leaves its last voltages in G_V; this 1 ms hook
 * stages the ones that moved and latches all four with one fast-write and
 * one LDAC pulse. At 400 kHz that is ~9 bytes for the write plus two
 * expander writes for LDAC, roughly 0.35 ms of blocking I2C, only in the
 * milliseconds where a value changed (every step when X is quantized, every
 * millisecond while it slews).
 */
static float    g_staged[4] = {99.f, 99.f, 99.f, 99.f};
static uint32_t g_flushes   = 0;

static void flush_xy(void)
{
    bool any = false;
    for (uint8_t i = 0; i < 4; i++)
    {
        const float v = G_V[i];
        if (fabsf(v - g_staged[i]) < 0.0012f) continue;   /* < 1 DAC LSB */
        g_staged[i] = v;
        hw.cv_jacks[kJackX1 + i].StageVolts(v);
        any = true;
    }
    if (any && hw.FlushCvOutputs()) g_flushes++;
}

/* The STM32's hardware RNG feeds Marbles' random stream, as upstream's did
 * (random_stream falls back to its own generator when the buffer is dry). */
static void feed_rng(void)
{
    for (int i = 0; i < 4 && daisy::Random::IsReady(); i++)
        engine.random_stream.Write(daisy::Random::GetValue());
}

static void OnPoll(uint32_t now)
{
    flush_xy();
    feed_rng();

    if (settings.IsActive())
    {
        b_tdv.Reset();
        b_xdv.Reset();
        swallow.All();
        return;
    }
    const bool b3 = swallow.Live(hw, kButtonB3);
    b_tdv.Poll(swallow.Live(hw, kButtonB1), now, dv.t, &g_dv_changed);
    /* B2 stands down while B3 is held: B2+B3 is the Settings chord. */
    if (b3) b_xdv.Reset();
    else b_xdv.Poll(swallow.Live(hw, kButtonB2), now, dv.x, &g_dv_changed);
}

/* ---- control frame (~60 Hz) ------------------------------------------------ */

static mi::StoredWatch<kNumPots> play_watch, setup_watch;
static float g_saved_peak  = 0.0f;
static bool  prev_settings = false;

static void OnFrame(void)
{
    const uint32_t now = System::GetNow();

    if (play_watch.Changed(pager, kPagePlay)) home.Touch(now);
    if (setup_watch.Changed(pager, kPageSetup)) home.Touch(now);
    if (g_dv_changed) { g_dv_changed = false; home.Touch(now); }

    /* Values are catch + locks, already mixed by the SDK (no CV reaches
     * them: J8 is added in the callback, see marbles_engine.h). */
    C.rate     = rate.Value();
    C.t_bias   = t_bias.Value();
    C.t_jitter = t_jitter.Value();
    C.deja_vu  = deja_vu.Value();
    C.x_spread = x_spread.Value();
    C.x_bias   = x_bias.Value();
    C.x_steps  = x_steps.Value();
    C.length   = length.Value();
    C.t_model  = (uint8_t)t_mode.Value();
    C.x_mode   = (uint8_t)x_mode.Value();
    C.x_range  = (uint8_t)x_range.Value();
    C.x_scale  = (uint8_t)scale.Value();

    C.t_range   = s_t_range.Value();
    C.j1_ignore = s_j1.Value() == 1;
    C.x_clock   = s_xclk.Value();
    C.cv_dest   = s_cv_dest.Value();
    C.ext_x     = s_ext_x.Value() == 1;
    C.pw_mean   = s_pw_mean.Value();
    C.pw_std    = s_pw_std.Value();
    C.y_spread  = s_yspread.Value();
    C.y_bias    = s_ybias.Value();
    C.y_steps   = s_ysteps.Value();
    C.y_divider = s_ydiv.Value();
    C.y_range   = s_yrange.Value();
    C.t_deja_vu = dv.t;
    C.x_deja_vu = dv.x;

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
 *   B1 / B2   t / X deja vu: dark off, green on, amber locked; while ON and
 *             the DEJA VU knob sits at its noon lock, green breathes
 *             (Marbles' slow triangle)
 *   B3        white for 30 ms on each T2 tick, else the Setup tint
 *   RATE ring T1 / T2 / T3 as three pips while each gate is high
 *   P1 ring   the previous session's worst CPU load, 2.5 s at boot
 *   CPU alarm B2 red while the callback averages over 80 %
 */
static uint32_t g_readout_until = 0;
static float    g_boot_peak     = 0.0f;

static LedPanel::Rgb dv_colour(uint8_t state, uint32_t t_ms)
{
    if (state == marbles::DEJA_VU_OFF) return kIdle;
    if (state == marbles::DEJA_VU_LOCKED) return kAmber;
    if (engine.deja_vu_at_noon())
    {
        int tri = (int)((t_ms & 1023u) >> 5);
        tri     = tri >= 16 ? 31 - tri : tri;
        const uint8_t g = (uint8_t)(0x20 + tri * 0x0D);
        return {0x00, g, 0x00};
    }
    return kGreen;
}

static void RateOverdraw(LedPanel& panel, uint8_t pot, const ArcGeometry& geo,
                         float norm, uint32_t t_ms, void* ctx)
{
    (void)norm; (void)t_ms; (void)ctx;
    static const LedPanel::Rgb kGate[3] = {{0xFF, 0x40, 0x40}, {0xFF, 0xFF, 0xFF}, {0x40, 0x40, 0xFF}};
    static const float         kHour[3] = {9.0f, 12.0f, 3.0f};
    (void)geo;
    for (int i = 0; i < 3; i++)
        if (G_T[i]) panel.SetRingByHour(pot, kHour[i], panel.ScaleGlobal(kGate[i]));
}

static void OnRender(uint32_t t_ms)
{
    LedPanel& L = hw.leds;
    if (t_ms < g_readout_until) mi::paint_cpu_readout(hw, g_boot_peak);
    if (settings.IsActive()) return;

    L.SetButtonPair(kButtonB1, L.ScaleGlobal(dv_colour(dv.t, t_ms)));
    L.SetButtonPair(kButtonB2, L.ScaleGlobal(cpu.GetAvgCpuLoad() > 0.80f ? kRed
                                                                         : dv_colour(dv.x, t_ms)));
    if (pager.Page() != kPageSetup)
    {
        const bool tick = System::GetNow() - G_T_EDGE_MS[1] < 30u;
        L.SetButtonPair(kButtonB3, L.ScaleGlobal(tick ? kWhite : kColSetup));
    }
}

/* ---- audio ----------------------------------------------------------------- */

/* The codec's DC-coupled outputs: +/-5 V at the jack is +/-1.0 in the
 * buffer (kCodecJackFullScaleVolts). Gates are 0 / +5 V; 0.999 keeps the
 * top code off the rail. */
static constexpr float kGateHigh = 0.999f;

static void AudioCallback(daisy::AudioHandle::InputBuffer  in,
                          daisy::AudioHandle::OutputBuffer out,
                          size_t                           size)
{
    cpu.OnBlockStart();
    if (size > kBlockSize)
    {
        for (size_t i = 0; i < size; i++) out[0][i] = out[1][i] = 0.f;
        cpu.OnBlockEnd();
        return;
    }

    marbles_port::Block b;
    engine.Process(C, in[0], in[1], hw.cv_jacks[kJackCv].Volts(),
                   System::GetNow(), size, &b);

    /* T1 -> J9 and T2 -> J10, per sample; T3 -> J7, per block (the level
     * at the block's end). */
    for (size_t i = 0; i < size; i++)
    {
        out[0][i] = b.t[0][i] ? kGateHigh : 0.f;
        out[1][i] = b.t[1][i] ? kGateHigh : 0.f;
    }
    hw.cv_jacks[kJackT3].SetVolts(b.t[2][size - 1] ? 5.0f : 0.0f);

    const uint32_t now = System::GetNow();
    for (int k = 0; k < 3; k++)
    {
        const bool g = b.t[k][size - 1];
        if (g && !G_T[k]) G_T_EDGE_MS[k] = now;
        G_T[k] = g;
    }
    for (int k = 0; k < 4; k++) G_V[k] = b.v[k];

    cpu.OnBlockEnd();
}

/* ---- boot ------------------------------------------------------------------ */

int main(void)
{
    hw.Init(daisy::SaiHandle::Config::SampleRate::SAI_32KHZ, kBlockSize);
    cpu.Init(hw.SampleRate(), (int)hw.BlockSize());
    if (hw.BlockSize() != kBlockSize) mi::fault_forever(hw);
    daisy::Random::Init();

    engine.Init((float)hw.SampleRate());

    /* J3-J6 (MCP4728) and J7 (STM32 DAC) become outputs. J9/J10 are NOT
     * claimed with EnableCvOutput: the SDK would then overwrite the whole
     * block with one staged level, and T1/T2 want per-sample edges, so the
     * callback writes out[] itself. */
    for (uint8_t j = kJackX1; j <= kJackY; j++)
    {
        hw.cv_jacks[j].StageVolts(0.0f);
        hw.cv_jacks[j].EnableCvOutput();
    }
    hw.FlushCvOutputs();
    hw.cv_jacks[kJackT3].SetVolts(0.0f);
    hw.cv_jacks[kJackT3].EnableCvOutput();

    sd.Init();
    picker::Install(settings, kSettingsFirmware, sd, hw);
    settings.UseBrightness();
    settings.UsePresets(presets);

    s_t_range = settings.Page(kSettingsMain).Pot(1).Selector(kTRangeLabels).Default(1)
        .Ident("t_range").Name("t Range")
        .Help("RATE's range: a quarter, normal, or four times as fast.");
    s_j1 = settings.Page(kSettingsMain).Pot(4).Selector(kJ1Labels).Default(0)
        .Ident("j1_mode").Name("J1 clock")
        .Help("**Auto**: J1 clocks T while edges arrive (within 3 s), else the "
              "internal clock. **Ignore**: always the internal clock. The Lab "
              "cannot sense a patched jack the way Marbles' normalling did.");
    s_xclk = settings.Page(kSettingsMain).Pot(5).Selector(kXClkLabels).Default(0)
        .Ident("x_clock").Name("X clock")
        .Help("What steps X: **Auto** = J2 while edges arrive, else T1+T2+T3 "
              "(Marbles' default); or pin it to one T output or to J2.");

    s_cv_dest = settings.Page(kSettingsFirmware).Pot(3).Selector(kCvDestLabels).Default(0)
        .Ident("cv_dest").Name("J8 CV to")
        .Help("Where J8 goes. RATE takes 1 V/oct; the others move their "
              "knob's full travel over 10 V.");
    s_ext_x = settings.Page(kSettingsFirmware).Pot(4).Selector(kOnOffLabels).Default(0)
        .Ident("ext_x").Name("External X")
        .Help("Marbles' external processing: X samples and quantizes the "
              "voltage on J8 instead of generating one (deja vu still "
              "loops it). J8 then has no other destination.");
    s_pw_mean = settings.Page(kSettingsFirmware).Pot(5).Knob().Default(0.5f)
        .Ident("t_pw").Name("Gate length")
        .Help("T gate length, as a fraction of the clock period.");

    s_yspread = settings.Page(kSettingsY).Pot(0).Knob().Default(0.5f)
        .Ident("y_spread").Name("Y Spread");
    s_ybias = settings.Page(kSettingsY).Pot(1).Knob().Default(0.5f)
        .Ident("y_bias").Name("Y Bias");
    s_ysteps = settings.Page(kSettingsY).Pot(2).Knob().Default(0.0f)
        .Ident("y_steps").Name("Y Steps")
        .Help("Y's smoothness (Marbles' hidden Y settings).");
    s_ydiv = settings.Page(kSettingsY).Pot(3).Selector(kYDivLabels).Default(6)
        .Ident("y_divider").Name("Y Divider")
        .Help("Y takes a new value every n X steps.");
    s_yrange = settings.Page(kSettingsY).Pot(4).Selector(kRangeLabels).Default(2)
        .Ident("y_range").Name("Y Range");
    s_pw_std = settings.Page(kSettingsY).Pot(5).Knob().Default(0.0f)
        .Ident("t_pw_std").Name("Gate jitter")
        .Help("Random variation of the T gate length.");
    settings.Page(kSettingsY).Name("Y");

    /* No jack goes through the SDK's matrix: J3-J7 are outputs and J8 is
     * read in the callback, where RATE needs its exact 1 V/oct. */
    for (uint8_t j = 0; j < 6; j++) cv_matrix.Jack(j).Off();

    rate.Overdraw(RateOverdraw);

    host.Product("Marbles")
        .BootSlot(home.slot)
        .Pages(play_page, setup_page)
        .Jacks(jk_tclk, jk_xclk, jk_x1, jk_x2, jk_x3, jk_y, jk_t3, jk_cv,
               jk_t1, jk_t2)
        .Buttons(bt_tdv, bt_xdv, bt_setup)
        .Attach(manual)
        .Extend(fs_ext);
#ifdef MI_BENCH_USB
    bench_cdc.Init(bench_usb, daisy::UsbHandle::FS_INTERNAL, "Marbles (bench)");
    host.Transport(bench_cdc);
#endif

    presets.Manage(pager);
    presets.Manage(settings);
    presets.Manage(extras);
    presets.Manage(dv);
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
        /* First boot after a flash: Marbles' factory state (settings.cc):
         * coin toss, identical, +/-5 V, major; STEPS at noon (unquantized
         * steps), LENGTH 8. Zone i of n sits at (i + 0.5) / n. */
        pager.SetStored(kPageSetup, kPotTopLeft,     0.5f,        phys);
        pager.SetStored(kPageSetup, kPotTopRight,    0.75f,       phys);
        pager.SetStored(kPageSetup, kPotMiddleLeft,  0.5f / 6.0f, phys);
        pager.SetStored(kPageSetup, kPotMiddleRight, 0.5f / 3.0f, phys);
        pager.SetStored(kPageSetup, kPotBottomLeft,  2.5f / 3.0f, phys);
        pager.SetStored(kPageSetup, kPotBottomRight, 0.5f / 6.0f, phys);
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

    for (;;) loop.Tick();
}
