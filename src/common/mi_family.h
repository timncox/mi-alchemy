/*
 * mi_family.h -- what every mi-alchemy firmware shares beyond the picker.
 *
 * Lifted from belt-alchemy (home-slot @ b11d765), where each piece was found
 * on hardware; the comments say which lesson each one carries. The four
 * firmwares here (Clouds, Elements, Marbles, Plaits) use them unchanged, so
 * a fix lands once instead of in every main file.
 *
 *   Gate        a CV jack read raw in the audio callback, thresholded in
 *               volts from the board's calibration record
 *   Toggle      the family's button gesture: momentary while held, a tap
 *               latches
 *   CpuExtras   last session's worst CPU load, in the preset blob, with a
 *               per-firmware schema tag
 *   HomeSlot    boot-time restore from this firmware's own preset slot and
 *               the hands-off autosave into it
 *   fault_forever, f2i, clampf, the boot CPU readout
 *
 * Home slots (0-based; `hostlink list` numbers from 1). The Smack / Mark /
 * Belt / Relay family owns 12-15 and documents 0-11 as free for presets
 * saved by hand. This repository takes 8-11 out of that range:
 *
 *   Clouds 8   Elements 9   Marbles 10   Plaits 11
 *
 * so 0-7 are what is left for hand-saved presets across the whole card.
 */
#pragma once

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "daisy_seed.h"
#include "util/CpuLoadMeter.h"
#include "alchemy/hw/alchemy_lab.h"
#include "alchemy/hw/v2_calibration.h"
#include "alchemy/surface/presets.h"
#include "alchemy/surface/serializable.h"

/* Cached D2 SRAM (224 KB, see src/common/alchemy.lds) and DTCM (128 KB, the
 * fastest RAM on the part and the nearest thing to the STM32F4's CCM that
 * Clouds and Elements were written for). Both NOLOAD: never zeroed at boot. */
#define MI_D2_BSS   __attribute__((section(".d2_bss"), aligned(32)))
#define MI_DTCM_BSS __attribute__((section(".dtcmram_bss"), aligned(32)))

namespace mi {

using alchemy::AlchemyLab;

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline int16_t f2i(float v)
{
    if (v > 0.999969f) v = 0.999969f;
    if (v < -1.0f)     v = -1.0f;
    return (int16_t)(v * 32767.0f);
}

/* ---- Gate ---------------------------------------------------------------
 *
 * A jack read as raw ADC in the audio callback, not through the SDK's
 * AnalogControl: that path is low-passed at ~20 Hz for pots, which rounds a
 * gate's edges into a swell (belt-alchemy's J3). The calibration record
 * gives the jack's 0 V code and the ADC reference, so the thresholds are
 * volts: assert above 1.5 V, release below 0.5 V. The deviation MAGNITUDE
 * is thresholded, so it reads whichever way the ADC counts -- the sign of
 * the front end is documented (higher code = more negative volts) but not
 * verified.
 *
 * One read per audio block: at 48 kHz / 128 frames a trigger under ~3 ms can
 * be missed (docs/alchemy-lab-status.md). The mi firmwares use 16-32 frame
 * blocks, so the window here is 0.5-1 ms.
 */
struct Gate
{
    uint8_t       jack  = 0;      /* CV index: 0 = J3 ... 5 = J8 */
    uint16_t      zero  = 32768u;
    int           hi    = 4915;
    int           lo    = 1638;
    volatile bool state = false;
    volatile bool rose  = false;  /* latched rising edge, cleared by Take() */

    void Init(AlchemyLab& hw, uint8_t cv_index)
    {
        jack = cv_index;
        const alchemy::V2Calibration& cal = hw.Calibration();
        float vdda = cal.vdda_at_cal;
        if (!(vdda > 2.5f && vdda < 3.6f)) vdda = alchemy::kV2VddaDesign;
        uint16_t z = cal.jack[cv_index].adc_zero_code;
        if (z < 20000u || z > 45000u) z = 32768u;   /* design mid-scale */
        const float cpv = alchemy::kV2CvInGainDesign * 65535.0f / vdda;
        zero = z;
        hi   = (int)(1.5f * cpv);
        lo   = (int)(0.5f * cpv);
    }

    /* Audio callback, once per block. Returns the new state. */
    inline bool Poll(AlchemyLab& hw)
    {
        const uint16_t raw = hw.seed.adc.Get(alchemy::kCvAdcOffset + jack);
        int            dev = (int)zero - (int)raw;
        if (dev < 0) dev = -dev;
        if (!state)
        {
            if (dev > hi) { state = true; rose = true; }
        }
        else if (dev < lo)
            state = false;
        return state;
    }

    /* True once per rising edge. */
    inline bool Take()
    {
        if (!rose) return false;
        rose = false;
        return true;
    }
};

/* ---- Toggle -------------------------------------------------------------
 *
 * The family's button gesture: momentary while held, and a tap (released
 * within kTapMs) toggles a latch instead. The effective state is latch XOR
 * held, so a hold from a latched state gives the opposite for its duration.
 * Edges come from Pressed(), not RisingEdge(): a 1 ms poll against a
 * debouncer's own edge flag is a coin flip (smack-versio's note).
 */
struct Toggle
{
    static constexpr uint32_t kTapMs = 300u;
    bool     down  = false;
    bool     latch = false;
    uint32_t t0    = 0;

    bool Poll(bool pressed, uint32_t now)
    {
        if (pressed && !down) { down = true; t0 = now; }
        if (!pressed && down)
        {
            down = false;
            if (now - t0 < kTapMs) latch = !latch;
        }
        return latch != down;
    }
    void Reset() { down = false; }
};

/* ---- CpuExtras ----------------------------------------------------------
 *
 * The worst audio-block load of the PREVIOUS session, replayed on the P1
 * ring for 2.5 s at boot, plus the worst smoothed load (HostLink getlive).
 * Each firmware passes its own tag, so a slot written by another firmware
 * fails the schema gate and lands in first-boot defaults instead of being
 * read as this one's settings. Tags: 'CLD' 'ELE' 'MRB' 'PLT'.
 */
struct CpuExtras : public alchemy::Serializable
{
    explicit CpuExtras(uint32_t tag3) : tag(tag3) {}

    uint32_t tag;
    float    cpu_peak = 0.0f;   /* 0..1; 0 == no data yet */
    float    cpu_avg  = 0.0f;

    size_t SerializedSize() const override { return 8u; }

    void Serialize(uint8_t* out) const override
    {
        memcpy(out, &cpu_peak, 4u);
        memcpy(out + 4u, &cpu_avg, 4u);
    }

    bool Deserialize(const uint8_t* in) override
    {
        float p;
        memcpy(&p, in, 4u);
        cpu_peak = (p >= 0.0f && p <= 1.0f) ? p : 0.0f;
        memcpy(&p, in + 4u, 4u);
        cpu_avg = (p >= 0.0f && p <= 1.0f) ? p : 0.0f;
        return true;
    }

    /* 'XYZ' << 8 | layout version. Bump the low byte on a layout change. */
    uint32_t SchemaHash() const override { return (tag << 8) | 0x01u; }

    /* Fold this block's meter into the session's worst. Returns true when
     * the peak rose enough (5 points) above what was last saved to be
     * worth a flash write. */
    bool Track(daisy::CpuLoadMeter& cpu, float saved_peak)
    {
        float mx = cpu.GetMaxCpuLoad();
        if (mx > 1.0f) mx = 1.0f;
        if (mx > cpu_peak) cpu_peak = mx;
        float av = cpu.GetAvgCpuLoad();
        if (av > 1.0f) av = 1.0f;
        if (av > cpu_avg) cpu_avg = av;
        return cpu_peak > saved_peak + 0.05f;
    }
};

/* ---- HomeSlot -----------------------------------------------------------
 *
 * Boot: BootLoad() still runs -- it fires HostLink's pre-boot hook and
 * restores slot 0 when slot 0 holds THIS firmware's settings (the schema
 * gate refuses any other) -- and the home slot, once it holds anything,
 * wins. Returns true when something was restored, false on a first boot
 * after a flash (the caller then adopts the pots and sets its defaults).
 *
 * Autosave: five seconds after the last change and only with hands off.
 * Save() erases the record's sector from this thread and the 1 ms button
 * poll pauses for it; the callers' epsilons are the wear limiter, this is a
 * ceiling on write frequency.
 */
struct HomeSlot
{
    uint8_t  slot;
    bool     dirty = false;
    uint32_t since = 0;

    explicit HomeSlot(uint8_t s) : slot(s) {}

    bool Boot(alchemy::Presets& presets)
    {
        const bool had_home = presets.HasValid(slot);
        const bool had_boot = had_home || presets.HasValid(0);
        presets.BootLoad();
        if (had_home) presets.Load(slot);
        return had_boot;
    }

    void Touch(uint32_t now)
    {
        dirty = true;
        since = now;
    }

    /* Every control frame. `busy` = Settings open or the picker writing.
     * Returns true when it saved. */
    bool Service(alchemy::Presets& presets, AlchemyLab& hw, uint32_t now, bool busy)
    {
        if (!dirty || busy || now - since < 5000u) return false;
        for (uint8_t b = 0; b < alchemy::kNumButtons; b++)
            if (hw.buttons[b].Pressed()) return false;
        presets.Save(slot);
        dirty = false;
        return true;
    }
};

/* A page's stored values: true when any moved more than 1 % since the last
 * call. The stored values move only when a caught pot moves (or a preset
 * loads), and the 1 % step keeps ADC noise from re-arming the autosave. The
 * first call only primes. */
template <uint8_t kPots>
struct StoredWatch
{
    float seen[kPots];
    bool  primed = false;

    template <typename PagerT>
    bool Changed(PagerT& pager, uint8_t page)
    {
        bool moved = false;
        for (uint8_t p = 0; p < kPots; p++)
        {
            const float s = pager.Stored(page, p);
            if (primed && fabsf(s - seen[p]) <= 0.01f) continue;
            if (primed) moved = true;
            seen[p] = s;
        }
        primed = true;
        return moved;
    }
};

/* Buttons still down when Settings closed are ignored until let go: the
 * SDK closes Settings on the B2 or B3 press itself, and the app's poll sees
 * that same press a moment later. */
struct Swallow
{
    uint8_t mask = 0;
    void    All() { mask = 0x07u; }
    bool    Live(AlchemyLab& hw, uint8_t b)
    {
        const bool    down = hw.buttons[b].Pressed();
        const uint8_t bit  = (uint8_t)(1u << b);
        if (mask & bit)
        {
            if (down) return false;
            mask = (uint8_t)(mask & ~bit);
        }
        return down;
    }
};

/* Refuse to run half-initialised: every button pair red, no audio. */
[[noreturn]] static inline void fault_forever(AlchemyLab& hw)
{
    for (;;)
    {
        hw.leds.Clear();
        for (uint8_t b = 0; b < alchemy::kNumButtons; b++)
            hw.leds.SetButtonPair(b, {0x80, 0x00, 0x00});
        hw.leds.Show();
        daisy::System::Delay(200);
    }
}

/* Paint `frac` of a ring from its start, in `c`. */
static inline void paint_fill(AlchemyLab& hw, uint8_t pot, float frac,
                              const alchemy::LedPanel::Rgb& c)
{
    const alchemy::ArcGeometry& geo = hw.Arc();
    frac = clampf(frac, 0.0f, 1.0f);
    const int n = (int)lroundf(frac * (float)geo.arc_leds);
    for (int i = 0; i < n; i++)
        hw.leds.SetRingByHour(pot, fmodf(geo.start_hour + geo.step_hours * (float)i, 12.0f),
                              hw.leds.ScaleGlobal(c));
}

/* The boot readout on P1: the previous session's CPU peak, for 2.5 s. */
static inline void paint_cpu_readout(AlchemyLab& hw, float peak)
{
    using Rgb = alchemy::LedPanel::Rgb;
    hw.leds.ClearRing(alchemy::kPotTopLeft);
    if (peak <= 0.0f)
    {
        /* No data: first boot after a flash, distinct from "measured, low". */
        hw.leds.SetRingByHour(alchemy::kPotTopLeft, hw.Arc().start_hour,
                              hw.leds.ScaleGlobal(Rgb{0x00, 0x00, 0x30}));
        return;
    }
    const Rgb c = peak >= 0.90f ? Rgb{0xFF, 0x00, 0x00}
                : peak >= 0.75f ? Rgb{0xFF, 0x80, 0x00}
                                : Rgb{0x00, 0xFF, 0x00};
    paint_fill(hw, alchemy::kPotTopLeft, peak, c);
}

} // namespace mi
