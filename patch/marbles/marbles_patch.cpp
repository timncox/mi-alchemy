/*
 * Marbles on the Electrosmith Daisy Patch.
 *
 * The engine is mi-alchemy's src/marbles/marbles_engine.h -- marbles.cc's
 * Process() over the vendored TGenerator / XYGenerator at Marbles' native
 * 32 kHz, 16-frame blocks -- byte for byte what the Alchemy Lab target and
 * the native test run. This file is only the Patch panel:
 *
 *   knobs    four knob+CV channels, paged (T / X / Y) with pickup: a knob
 *            takes over a param only once it crosses the stored value or
 *            comes within PICKUP_SLACK of it. CV patched into a knob's jack
 *            sums with that knob, so it modulates whatever the current page
 *            puts on it.
 *   encoder  click to select a row ('*'), turn to change, click to let go.
 *            Click on "Mods" = module picker. (Push-and-turn is dead on
 *            Tim's Patch: the switch opens while the knob turns.)
 *   gates    GATE IN 1 = T clock, GATE IN 2 = X clock.
 *   outputs  kRoutes[] below -- the ONE table to edit to remap jacks.
 *
 * Settings (knob values and menu) persist in QSPI at 0x00790000, written a
 * few seconds after the last change.
 *
 * NOTHING HERE HAS RUN ON HARDWARE. The output mapping is a guess Tim has
 * not chosen; the audio-out triggers go through AC-coupled jacks.
 */
#include "daisy_patch.h"
#include "per/rng.h"
#include "util/PersistentStorage.h"
#include "module_picker.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "marbles_engine.h"

using namespace daisy;
using namespace marbles_port;

#ifndef MI_GIT_HASH
#define MI_GIT_HASH "dev"
#endif

static constexpr size_t kBlock = kMaxBlock; /* 16 frames, 0.5 ms at 32 kHz */

static DaisyPatch hw;

/* ------------------------------------------------------------------------ */
/* OUTPUT MAP. One row per Patch output jack; change a row to remap.          */
/*                                                                            */
/* Only CV OUT 1/2 (12-bit DAC, DC-coupled, 0..~5 V) can carry X or Y. The    */
/* gate out is a DC logic output. Audio outs 1-4 are AC-coupled (4.7 uF on    */
/* the Seed for 1/2, 10 uF on the carrier for 3/4): slow CV dies there, a     */
/* short pulse mostly gets through -- UNPROVEN on hardware.                   */

enum Src : uint8_t { SRC_X1, SRC_X2, SRC_X3, SRC_Y, SRC_T1, SRC_T2, SRC_T3, SRC_NONE };
enum Kind : uint8_t {
    K_CV,   /* DAC volts, X/Y only */
    K_GATE, /* follows the T gate (high for its whole pulse width) */
    K_TRIG, /* kTrigSamples pulse at kTrigLevel on each T rising edge */
    K_OFF,
};
enum Jack : uint8_t { J_CV1, J_CV2, J_GATE, J_OUT1, J_OUT2, J_OUT3, J_OUT4, J_COUNT };

struct Route
{
    Src  src;
    Kind kind;
};

static const Route kRoutes[J_COUNT] = {
    /* J_CV1  */ {SRC_X1, K_CV},   /* the "CV1" menu row re-picks X1/X2/X3 */
    /* J_CV2  */ {SRC_Y, K_CV},
    /* J_GATE */ {SRC_T2, K_GATE}, /* T2 = the steady master clock */
    /* J_OUT1 */ {SRC_T1, K_TRIG}, /* trigger through an AC-coupled jack: unproven */
    /* J_OUT2 */ {SRC_T3, K_TRIG}, /* same */
    /* J_OUT3 */ {SRC_NONE, K_OFF},
    /* J_OUT4 */ {SRC_NONE, K_OFF},
};

static constexpr int   kTrigSamples = 64;   /* 2 ms at 32 kHz */
static constexpr float kTrigLevel   = 0.8f; /* of the codec's full scale */

/* DAC: 0..3.3 V through the carrier's gain of 1.512 = 0..4.99 V at code
 * 4095 (Rev8 schematic sheet 2; the same figure Relay uses as its default
 * calibration). Unmetered. */
static constexpr float kDacFullScaleV = 4.99f;

/* ------------------------------------------------------------------------ */
/* Params and settings                                                        */

enum Param {
    P_RATE, P_T_BIAS, P_T_JITTER, P_DEJA_VU,     /* page T */
    P_X_SPREAD, P_X_BIAS, P_X_STEPS, P_LENGTH,   /* page X */
    P_Y_SPREAD, P_Y_BIAS, P_Y_STEPS, P_GATE_LEN, /* page Y */
    P_COUNT
};
static const char *const PARAM_LABEL[P_COUNT] = {
    "RATE", "BIAS", "JITR", "DEJA", "SPRD", "BIAS", "STEP", "LEN",
    "SPRD", "BIAS", "STEP", "GLEN",
};
static constexpr int kPages = 3;
static const char *const PAGE_NAME[kPages] = {"T", "X", "Y"};

enum MenuItem {
    MI_PAGE, MI_T_DV, MI_X_DV, MI_T_MODE, MI_T_RANGE, MI_T_CLOCK, MI_X_MODE, MI_X_RANGE,
    MI_SCALE, MI_X_CLOCK, MI_CV1, MI_Y_RANGE, MI_Y_DIV, MI_GATE_JIT, MI_MODS, MI_COUNT
};

struct Settings
{
    uint32_t magic;
    float    param[P_COUNT];
    uint8_t  menu[MI_COUNT]; /* MI_PAGE and MI_MODS unused */

    bool operator==(const Settings &o) const { return memcmp(this, &o, sizeof o) == 0; }
    bool operator!=(const Settings &o) const { return !(*this == o); }
};
static constexpr uint32_t kMagic          = 0x4D524231; /* 'MRB1' */
static constexpr uint32_t kSettingsOffset = 0x00790000; /* 64 KB reserved for Marbles */

/* Menu value counts and names. */
static const char *const DV_NAME[]     = {"Off", "On", "Lock"};
static const char *const TMODE_NAME[]  = {"Coin", "Clustr", "Drums", "Indep", "Divide", "3state"};
static const char *const TRANGE_NAME[] = {"x1/4", "x1", "x4"};
static const char *const TCLK_NAME[]   = {"Gate1", "Ignore"};
static const char *const XMODE_NAME[]  = {"Ident", "Bump", "Tilt"};
/* "+-5>0-5": Marbles' +/-5 V squeezed into the DAC's 0..5 V (half scale). */
static const char *const RANGE_NAME[]  = {"+2V", "+5V", "+-5>0-5"};
static const char *const SCALE_NAME[]  = {"Major", "Minor", "Penta", "Pelog", "Bhairv", "Shri"};
static const char *const XCLK_NAME[]   = {"Auto", "T1T2T3", "T1", "T2", "T3", "Gate2"};
static const char *const CV1_NAME[]    = {"X1", "X2", "X3"};
static const char *const YDIV_NAME[]   = {"1/64", "1/48", "1/32", "1/24", "1/16", "1/12",
                                          "1/8",  "1/6",  "1/4",  "1/3",  "1/2",  "1/1"};

struct MenuDef
{
    const char        *label;
    int                count; /* values 0..count-1; 0 = not an enum */
    const char *const *names;
};
static const MenuDef MENU[MI_COUNT] = {
    {"Page", kPages, PAGE_NAME},  {"tDejaV", 3, DV_NAME},     {"xDejaV", 3, DV_NAME},
    {"tMode", 6, TMODE_NAME},     {"tRange", 3, TRANGE_NAME}, {"tClock", 2, TCLK_NAME},
    {"xMode", 3, XMODE_NAME},     {"xRange", 3, RANGE_NAME},  {"Scale", 6, SCALE_NAME},
    {"xClock", 6, XCLK_NAME},     {"CV1", 3, CV1_NAME},       {"yRange", 3, RANGE_NAME},
    {"yDiv", 12, YDIV_NAME},      {"GateJt", 21, nullptr},    {"Mods", 0, nullptr},
};

static Settings defaults()
{
    Settings s;
    memset(&s, 0, sizeof s);
    s.magic = kMagic;
    /* Marbles' factory state, except the ranges: +5 V, because the DAC has
     * no negative half (Marbles boots +/-5 V). */
    const float p[P_COUNT] = {0.5f, 0.5f, 0.0f, 0.0f, 0.5f, 0.5f, 0.5f, 0.75f,
                              0.5f, 0.5f, 0.0f, 0.5f};
    for(int i = 0; i < P_COUNT; i++) s.param[i] = p[i];
    s.menu[MI_T_RANGE] = 1;
    s.menu[MI_X_RANGE] = 1;
    s.menu[MI_Y_RANGE] = 1;
    s.menu[MI_Y_DIV]   = 6; /* 1/8 */
    return s;
}

static PersistentStorage<Settings> storage(hw.seed.qspi);
static Settings                   *S;
static bool                        g_dirty;
static uint32_t                    g_dirty_since;
static void                        touch() { g_dirty = true; g_dirty_since = System::GetNow(); }

/* ------------------------------------------------------------------------ */
/* Engine + audio                                                             */

static Engine   engine;
static Controls C;

static CpuLoadMeter      g_cpu;
static volatile float    g_volts[J_COUNT]; /* what each DAC jack outputs, V */
static volatile uint8_t  g_tstate;         /* T1..T3 at the last sample, bits 0..2 */
static int               g_trig_left[J_COUNT];
static bool              g_t_prev[3];

static float src_cv_volts(Src s, const Block &b)
{
    const int   j     = s == SRC_Y ? 3 : (int)s;
    const int   range = s == SRC_Y ? C.y_range : C.x_range;
    float       v     = b.v[j];
    if(range == VOLTAGE_RANGE_FULL) v = 0.5f * (v + 5.0f); /* +/-5 -> 0..5 */
    return v;
}

static uint16_t volts_to_code(float v)
{
    float n = v / kDacFullScaleV;
    if(n < 0.f) n = 0.f;
    if(n > 1.f) n = 1.f;
    return (uint16_t)(n * 4095.0f + 0.5f);
}

static void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)
{
    g_cpu.OnBlockStart();
    if(size > kBlock) size = kBlock;

    /* Gate ins as clock "samples": one read per block (0.5 ms resolution). */
    float t_in[kBlock], x_in[kBlock];
    const float t_lvl = hw.gate_input[DaisyPatch::GATE_IN_1].State() ? 1.f : 0.f;
    const float x_lvl = hw.gate_input[DaisyPatch::GATE_IN_2].State() ? 1.f : 0.f;
    for(size_t i = 0; i < size; i++)
    {
        t_in[i] = t_lvl;
        x_in[i] = x_lvl;
    }

    Block b;
    engine.Process(C, t_in, x_in, 0.f, System::GetNow(), size, &b);

    for(int j = 0; j < J_COUNT; j++)
    {
        const Route &r = kRoutes[j];
        Src          src = r.src;
        if(j == J_CV1 && src <= SRC_X3) src = (Src)(SRC_X1 + S->menu[MI_CV1] % 3);
        const bool is_t = src >= SRC_T1 && src <= SRC_T3;
        const int  t    = src - SRC_T1;
        float     *o    = j >= J_OUT1 ? out[j - J_OUT1] : nullptr;

        if(r.kind == K_CV && !is_t && src != SRC_NONE)
        {
            const float v = src_cv_volts(src, b);
            g_volts[j]    = v;
            hw.seed.dac.WriteValue(j == J_CV1 ? DacHandle::Channel::ONE : DacHandle::Channel::TWO,
                                   volts_to_code(v));
        }
        else if(r.kind == K_GATE && is_t)
        {
            const bool on = b.t[t][size - 1];
            if(j == J_GATE) hw.gate_output.Write(on);
            if(o)
                for(size_t i = 0; i < size; i++) o[i] = b.t[t][i] ? kTrigLevel : 0.f;
        }
        else if(r.kind == K_TRIG && is_t && o)
        {
            bool prev = g_t_prev[t];
            for(size_t i = 0; i < size; i++)
            {
                if(b.t[t][i] && !prev) g_trig_left[j] = kTrigSamples;
                prev = b.t[t][i];
                o[i] = g_trig_left[j] > 0 ? kTrigLevel : 0.f;
                if(g_trig_left[j] > 0) g_trig_left[j]--;
            }
        }
        else if(o)
        {
            for(size_t i = 0; i < size; i++) o[i] = 0.f;
        }
    }
    for(int t = 0; t < 3; t++) g_t_prev[t] = b.t[t][size - 1];
    g_tstate = (uint8_t)((b.t[0][size - 1] ? 1 : 0) | (b.t[1][size - 1] ? 2 : 0)
                         | (b.t[2][size - 1] ? 4 : 0));
    (void)in;
    g_cpu.OnBlockEnd();
}

/* Settings -> the engine's Controls (written here, read by the callback). */
static void apply(void)
{
    const float *p = S->param;
    C.rate      = p[P_RATE];
    C.t_bias    = p[P_T_BIAS];
    C.t_jitter  = p[P_T_JITTER];
    C.deja_vu   = p[P_DEJA_VU];
    C.x_spread  = p[P_X_SPREAD];
    C.x_bias    = p[P_X_BIAS];
    C.x_steps   = p[P_X_STEPS];
    C.length    = p[P_LENGTH];
    C.y_spread  = p[P_Y_SPREAD];
    C.y_bias    = p[P_Y_BIAS];
    C.y_steps   = p[P_Y_STEPS];
    C.pw_mean   = p[P_GATE_LEN];
    const uint8_t *m = S->menu;
    C.t_deja_vu = m[MI_T_DV];
    C.x_deja_vu = m[MI_X_DV];
    C.t_model   = m[MI_T_MODE];
    C.t_range   = m[MI_T_RANGE];
    C.j1_ignore = m[MI_T_CLOCK] != 0;
    C.x_mode    = m[MI_X_MODE];
    C.x_range   = m[MI_X_RANGE];
    C.x_scale   = m[MI_SCALE];
    C.x_clock   = m[MI_X_CLOCK];
    C.y_range   = m[MI_Y_RANGE];
    C.y_divider = m[MI_Y_DIV];
    C.pw_std    = m[MI_GATE_JIT] * 0.05f;
    C.cv_dest   = 0xFF; /* no CV input of its own on the Patch: knob CV sums instead */
    C.ext_x     = false;
}

/* ------------------------------------------------------------------------ */
/* Knobs with pickup                                                          */

/* Real Patch knobs stop short of full scale (Belt, 2026-10-21): map 2..98 %
 * of travel onto the whole param, and allow pickup within PICKUP_SLACK of
 * 127 steps, or a param at 100 % could never be picked up. */
#define PICKUP_SLACK 3
static int  g_page;
static bool g_live[4];
static int  g_knob_at[4];

static float knob_norm(int k)
{
    float v = (hw.GetKnobValue((DaisyPatch::Ctrl)k) - 0.02f) / 0.96f;
    return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
}
static int to127(float v) { return (int)(v * 127.0f + 0.5f); }

static void page_reset(void)
{
    for(int k = 0; k < 4; k++)
    {
        g_live[k]    = false;
        g_knob_at[k] = to127(knob_norm(k));
    }
}

static void knobs(void)
{
    for(int k = 0; k < 4; k++)
    {
        const int   idx = g_page * 4 + k;
        const float v   = knob_norm(k);
        const int   raw = to127(v);
        if(!g_live[k])
        {
            const int cur = to127(S->param[idx]);
            if((g_knob_at[k] <= cur && raw >= cur) || (g_knob_at[k] >= cur && raw <= cur)
               || abs(raw - cur) <= PICKUP_SLACK)
                g_live[k] = true;
            g_knob_at[k] = raw;
            if(!g_live[k]) continue;
        }
        g_knob_at[k] = raw;
        if(fabsf(v - S->param[idx]) > 0.002f || (raw != to127(S->param[idx])))
        {
            S->param[idx] = v;
            touch();
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Encoder: click to select, turn to change, click to let go                  */

static int  g_sel;
static bool g_editing, enc_down, enc_turned;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void menu_edit(int inc)
{
    if(g_sel == MI_PAGE)
    {
        g_page = clampi(g_page + inc, 0, kPages - 1);
        page_reset();
        return;
    }
    const MenuDef &d = MENU[g_sel];
    if(d.count <= 0) return;
    S->menu[g_sel] = (uint8_t)clampi(S->menu[g_sel] + inc, 0, d.count - 1);
    touch();
}

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
        if(enc_down) enc_turned = true; /* a turn during a press is not a click */
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
        {
            picker::run(hw); /* modal; returns only if the user backs out */
            enc_down = false;
            page_reset();
        }
        else
            g_editing = !g_editing;
    }
}

/* ------------------------------------------------------------------------ */
/* Display: 21 columns of Font_6x8                                            */

static int g_cpu_pct;

static void fmt_volts(char *buf, size_t n, float v)
{
    int mv = (int)(v * 100.0f + 0.5f);
    if(mv < 0) mv = 0;
    snprintf(buf, n, "%d.%02d", mv / 100, mv % 100);
}

static void draw(void)
{
    char line[32];
    hw.display.Fill(false);

    /* Header: page, T1-T3 lamps, clock source, CPU. */
    const uint8_t ts = g_tstate;
    snprintf(line, sizeof line, "MARBLES %s %c%c%c %s", PAGE_NAME[g_page], ts & 1 ? '1' : '.',
             ts & 2 ? '2' : '.', ts & 4 ? '3' : '.', engine.t_external() ? "ext" : "int");
    hw.display.SetCursor(0, 0);
    hw.display.WriteString(line, Font_6x8, true);

    /* The page's four knobs; '*' = not picked up yet. */
    for(int row = 0; row < 2; row++)
    {
        const int a = g_page * 4 + row * 2, b = a + 1;
        snprintf(line, sizeof line, "%-4s%4d%c %-4s%4d%c", PARAM_LABEL[a], (int)(S->param[a] * 100.f + 0.5f),
                 g_live[row * 2] ? ' ' : '*', PARAM_LABEL[b], (int)(S->param[b] * 100.f + 0.5f),
                 g_live[row * 2 + 1] ? ' ' : '*');
        hw.display.SetCursor(0, 10 + row * 9);
        hw.display.WriteString(line, Font_6x8, true);
    }

    /* What the two DAC jacks put out right now. */
    char v1[12], v2[12];
    fmt_volts(v1, sizeof v1, g_volts[J_CV1]);
    fmt_volts(v2, sizeof v2, g_volts[J_CV2]);
    snprintf(line, sizeof line, "%s %sV  Y %sV", CV1_NAME[S->menu[MI_CV1] % 3], v1, v2);
    hw.display.SetCursor(0, 28);
    hw.display.WriteString(line, Font_6x8, true);

    /* Menu: three rows, scrolled to keep the cursor in view. */
    static int top = 0;
    if(g_sel < top) top = g_sel;
    if(g_sel > top + 2) top = g_sel - 2;
    for(int r = 0; r < 3; r++)
    {
        const int i = top + r;
        if(i >= MI_COUNT) break;
        char val[16];
        if(i == MI_PAGE)
            snprintf(val, sizeof val, "%s", PAGE_NAME[g_page]);
        else if(i == MI_MODS)
            snprintf(val, sizeof val, "%dk c%d%%", (int)(hw.AudioSampleRate() / 1000.f), g_cpu_pct);
        else if(i == MI_GATE_JIT)
            snprintf(val, sizeof val, "%d%%", S->menu[i] * 5);
        else
            snprintf(val, sizeof val, "%s", MENU[i].names[S->menu[i] % MENU[i].count]);
        snprintf(line, sizeof line, "%c%-6s %s", i == g_sel ? (g_editing ? '*' : '>') : ' ', MENU[i].label, val);
        hw.display.SetCursor(0, 37 + r * 9);
        hw.display.WriteString(line, Font_6x8, true);
    }
    hw.display.Update();
}

/* ------------------------------------------------------------------------ */

int main(void)
{
    hw.Init();
    hw.SetAudioBlockSize(kBlock);
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_32KHZ);
    Random::Init();

    storage.Init(defaults(), kSettingsOffset);
    S = &storage.GetSettings();
    if(S->magic != kMagic) *S = defaults();
    for(int i = 0; i < P_COUNT; i++)
        if(!(S->param[i] >= 0.f && S->param[i] <= 1.f)) S->param[i] = defaults().param[i];
    for(int i = 0; i < MI_COUNT; i++)
        if(MENU[i].count > 0 && S->menu[i] >= MENU[i].count) S->menu[i] = 0;

    engine.Init(hw.AudioSampleRate());
    apply();

    g_cpu.Init(hw.AudioSampleRate(), hw.AudioBlockSize());
    hw.StartAdc();
    hw.StartAudio(AudioCallback);
    page_reset();

    uint32_t last_draw = 0, last_cpu = 0;
    for(;;)
    {
        hw.ProcessAllControls();
        encoder();
        knobs();
        apply();

        /* The STM32 RNG feeds Marbles' random stream, as upstream's driver
         * did; the stream falls back to its own generator when dry. */
        for(int i = 0; i < 4 && Random::IsReady(); i++) engine.random_stream.Write(Random::GetValue());

        const uint32_t now = System::GetNow();
        if(g_dirty && now - g_dirty_since > 3000u && !enc_down)
        {
            storage.Save(); /* blocks on QSPI; audio keeps running */
            g_dirty = false;
        }
        if(now - last_cpu >= 500u)
        {
            g_cpu_pct = (int)(g_cpu.GetMaxCpuLoad() * 100.f + 0.5f);
            g_cpu.Reset();
            last_cpu = now;
        }
        if(now - last_draw >= 33u)
        {
            last_draw = now;
            draw();
        }
        System::Delay(1);
    }
}
