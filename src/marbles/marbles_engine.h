/*
 * marbles_engine.h -- marbles.cc's Process(), without the hardware.
 *
 * Everything between the jacks and the vendored generators lives here, so
 * the native test (test/test_marbles.cpp) runs the exact code the firmware
 * runs: the clock-input edge detection, the parameter scaling of
 * cv_reader.cc's channel table, the deja vu deadband, the loop-length
 * quantizer, the X clock source choice, TGenerator::Process and
 * XYGenerator::Process with the GroupSettings marbles.cc builds, and the T
 * gate delay. The firmware file only moves values between this and the
 * Alchemy Lab's jacks, pots and buttons.
 *
 * Upstream line references are to vendor/eurorack/marbles/marbles.cc
 * @ 08460a6.
 */
#pragma once

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "marbles/note_filter.h"
#include "marbles/ramp/ramp_divider.h"
#include "marbles/random/random_generator.h"
#include "marbles/random/random_stream.h"
#include "marbles/random/t_generator.h"
#include "marbles/random/x_y_generator.h"
#include "stmlib/dsp/hysteresis_quantizer.h"
#include "stmlib/utils/gate_flags.h"

#include "preset_scales.h"

namespace marbles {
/* settings.h is not compiled here (it drags in the F4 flash driver); its
 * one enum is all that is needed. Values as upstream. */
#ifndef MARBLES_SETTINGS_H_
enum DejaVuState { DEJA_VU_OFF, DEJA_VU_ON, DEJA_VU_LOCKED };
#endif
}

namespace marbles_port {

using namespace marbles;
using stmlib::GateFlags;

static constexpr size_t kMaxBlock = 16;

/* T gates leave this many samples late (2 ms at 32 kHz). Upstream delayed
 * them 2 samples to line up with its SPI DAC. Here X reaches the jacks
 * through an I2C DAC flushed from a 1 ms poll -- up to ~2 ms after the
 * callback computed it -- so the gates wait for X: a sample-and-hold or an
 * envelope fired by T1 sees the new X1, as on the original. */
static constexpr size_t kGateDelay = 64;

/* Everything the panel sets, written by the control thread. */
struct Controls
{
    /* PLAY / SETUP knobs, 0..1 (selectors: zone index). */
    volatile float   rate = 0.5f, t_bias = 0.5f, t_jitter = 0.f, deja_vu = 0.f;
    volatile float   x_spread = 0.5f, x_bias = 0.5f, x_steps = 0.5f, length = 0.75f;
    volatile uint8_t t_model = 0, x_mode = 0, x_range = 2, x_scale = 0;
    /* Settings. */
    volatile uint8_t t_range = 1;       /* TGeneratorRange */
    volatile bool    j1_ignore = false;
    volatile uint8_t x_clock = 0;       /* 0 auto, 1 T1T2T3, 2 T1, 3 T2, 4 T3, 5 J2 */
    volatile uint8_t cv_dest = 0;       /* 0 rate ... 7 length */
    volatile bool    ext_x = false;
    volatile float   pw_mean = 0.5f, pw_std = 0.f;
    volatile float   y_spread = 0.5f, y_bias = 0.5f, y_steps = 0.f;
    volatile uint8_t y_divider = 6, y_range = 2;
    volatile uint8_t t_deja_vu = DEJA_VU_OFF, x_deja_vu = DEJA_VU_OFF;
};

enum CvDest : uint8_t {
    CV_RATE, CV_T_BIAS, CV_T_JITTER, CV_DEJA_VU, CV_X_SPREAD, CV_X_BIAS,
    CV_X_STEPS, CV_LENGTH
};

/* One block's outputs. */
struct Block
{
    bool  t[3][kMaxBlock];   /* T1 T2 T3 */
    float v[4];              /* X1 X2 X3 Y at the block's last sample, volts */
};

/* A codec input as a clock: +1.0 V asserts, +0.5 V releases (the jacks are
 * AC-coupled, so a held gate sags; clocks and triggers are what they are
 * for). +/-5 V at the jack is +/-1.0 in the buffer. */
struct ClockIn
{
    GateFlags flags     = stmlib::GATE_FLAG_LOW;
    bool      high      = false;
    bool      ever      = false;
    uint32_t  last_edge = 0;

    void Process(const float* in, size_t n, uint32_t now_ms, GateFlags* out)
    {
        for (size_t i = 0; i < n; i++)
        {
            const float s = in[i];
            if (!high && s > 0.2f)      high = true;
            else if (high && s < 0.1f)  high = false;
            flags  = stmlib::ExtractGateFlags(flags, high);
            out[i] = flags;
            if (flags & stmlib::GATE_FLAG_RISING)
            {
                ever      = true;
                last_edge = now_ms;
            }
        }
    }

    /* Marbles sensed a patch cable through its jack's normalling switch;
     * the Lab cannot, so "patched" = an edge within the last 3 s. */
    bool Active(uint32_t now_ms) const { return ever && now_ms - last_edge < 3000u; }
};

/* marbles.cc's tables (lines 186-219). */
static const Ratio kYDividerRatios[12] = {
    {1, 64}, {1, 48}, {1, 32}, {1, 24}, {1, 16}, {1, 12},
    {1, 8},  {1, 6},  {1, 4},  {1, 3},  {1, 2},  {1, 1},
};

static const int kLoopLength[] = {
    1,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    5, 5, 5, 5,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    7, 7,
    8, 8, 8, 8, 8, 8, 8, 8, 8,
    10, 10, 10,
    12, 12, 12, 12, 12, 12, 12,
    14, 14,
    16
};

static inline float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

class Engine
{
  public:
    RandomGenerator random_generator;
    RandomStream    random_stream;
    TGenerator      t_generator;
    XYGenerator     xy_generator;

    void Init(float sample_rate)
    {
        random_generator.Init(1);
        random_stream.Init(&random_generator);
        t_generator.Init(&random_stream, sample_rate);
        xy_generator.Init(&random_stream, sample_rate);
        for (int i = 0; i < 6; ++i) xy_generator.LoadScale(i, kPresetScales[i]);
        note_filter_.Init();
        length_quantizer_.Init(sizeof(kLoopLength) / sizeof(int), 0.25f, false);
        memset(delay_, 0, sizeof delay_);
        delay_pos_ = 0;
        smoothed_  = false;
    }

    bool deja_vu_at_noon() const { return dv_lock_; }
    int  loop_length() const { return last_length_; }
    bool t_external() const { return last_t_external_; }
    ClockSource xy_source() const { return last_xy_source_; }

    /*
     * One block. `t_in` / `x_in` are the J1 / J2 codec samples, `cv_volts`
     * J8, `now_ms` the system clock (patch sensing only).
     */
    void Process(const Controls& c, const float* t_in, const float* x_in,
                 float cv_volts, uint32_t now_ms, size_t size, Block* out)
    {
        GateFlags t_clock[kMaxBlock], xy_clock[kMaxBlock];
        t_clk_.Process(t_in, size, now_ms, t_clock);
        x_clk_.Process(x_in, size, now_ms, xy_clock);

        /* ---- parameters, cv_reader.cc's channel table ------------------ */
        const uint8_t dest = c.ext_x ? 0xFFu : c.cv_dest;
        const float   cvn  = cv_volts / 10.0f;   /* full knob travel over 10 V */
        auto knob = [&](float k, CvDest d) { return clamp01(k + (dest == d ? cvn : 0.f)); };

        /* The SDK's pots arrive at the frame rate; a one-pole at the block
         * rate (2 kHz) stands in for cv_reader's pot_lp so stepped frame
         * updates do not click the rate or bias. */
        const float in_rate = c.rate, in_tb = c.t_bias, in_tj = c.t_jitter;
        const float in_dv = c.deja_vu, in_xs = c.x_spread, in_xb = c.x_bias;
        const float in_st = c.x_steps, in_len = c.length;
        if (!smoothed_)
        {
            s_rate_ = in_rate; s_tb_ = in_tb; s_tj_ = in_tj; s_dv_ = in_dv;
            s_xs_ = in_xs; s_xb_ = in_xb; s_st_ = in_st; s_len_ = in_len;
            smoothed_ = true;
        }
        const float k = 0.05f;
        s_rate_ += k * (in_rate - s_rate_);  s_tb_ += k * (in_tb - s_tb_);
        s_tj_   += k * (in_tj - s_tj_);      s_dv_ += k * (in_dv - s_dv_);
        s_xs_   += k * (in_xs - s_xs_);      s_xb_ += k * (in_xb - s_xb_);
        s_st_   += k * (in_st - s_st_);      s_len_ += k * (in_len - s_len_);

        /* T_RATE: pot_scale 120, offset -60, clamped +/-120; CV 1 V/oct. */
        float rate = s_rate_ * 120.0f - 60.0f;
        if (dest == CV_RATE) rate += 12.0f * cv_volts;
        if (rate < -120.f) rate = -120.f;
        if (rate > 120.f)  rate = 120.f;
        const float t_bias   = clamp01(knob(s_tb_, CV_T_BIAS) * 1.05f - 0.025f);
        const float t_jitter = knob(s_tj_, CV_T_JITTER);
        /* hysteresis 0.01 / 0.02 widen the pot's scale (CvReaderChannel::Init) */
        const float x_spread = clamp01(knob(s_xs_, CV_X_SPREAD) * 1.02f - 0.01f);
        const float x_bias   = clamp01(knob(s_xb_, CV_X_BIAS) * 1.02f - 0.01f);
        const float x_steps  = clamp01(knob(s_st_, CV_X_STEPS) * 1.04f - 0.02f);
        float       deja_vu  = knob(s_dv_, CV_DEJA_VU);

        /* Deadband near 12 o'clock for deja vu (marbles.cc:236-249). */
        const float d = fabsf(deja_vu - 0.5f);
        if (d > 0.03f)      dv_lock_ = false;
        else if (d < 0.02f) dv_lock_ = true;
        if (deja_vu < 0.47f)      deja_vu *= 1.06382978723f;
        else if (deja_vu > 0.53f) deja_vu = 0.5f + (deja_vu - 0.53f) * 1.06382978723f;
        else                      deja_vu = 0.5f;

        const int length = length_quantizer_.Lookup(kLoopLength, knob(s_len_, CV_LENGTH));
        last_length_ = length;

        /* ---- clock sources ---------------------------------------------- */
        const bool t_external = !c.j1_ignore && t_clk_.Active(now_ms);
        last_t_external_ = t_external;

        ClockSource xy_source;
        switch (c.x_clock)
        {
            case 1:  xy_source = CLOCK_SOURCE_INTERNAL_T1_T2_T3; break;
            case 2:  xy_source = CLOCK_SOURCE_INTERNAL_T1; break;
            case 3:  xy_source = CLOCK_SOURCE_INTERNAL_T2; break;
            case 4:  xy_source = CLOCK_SOURCE_INTERNAL_T3; break;
            case 5:  xy_source = CLOCK_SOURCE_EXTERNAL; break;
            default: xy_source = x_clk_.Active(now_ms) ? CLOCK_SOURCE_EXTERNAL
                                                       : CLOCK_SOURCE_INTERNAL_T1_T2_T3;
        }
        last_xy_source_ = xy_source;

        /* ---- T section (marbles.cc:267-296) ----------------------------- */
        Ramps ramps;
        ramps.master   = &ramp_[0][0];
        ramps.external = &ramp_[1][0];
        ramps.slave[0] = &ramp_[2][0];
        ramps.slave[1] = &ramp_[3][0];

        const float t_dv = c.t_deja_vu == DEJA_VU_LOCKED ? 0.5f
                         : (c.t_deja_vu == DEJA_VU_ON ? deja_vu : 0.0f);
        bool t_reset = false;
        t_generator.set_model(TGeneratorModel(c.t_model < 6 ? c.t_model : 0));
        t_generator.set_range(TGeneratorRange(c.t_range < 3 ? c.t_range : 1));
        t_generator.set_rate(rate);
        t_generator.set_bias(t_bias);
        t_generator.set_jitter(t_jitter);
        t_generator.set_deja_vu(t_dv);
        t_generator.set_length(length);
        t_generator.set_pulse_width_mean(clamp01(c.pw_mean));
        t_generator.set_pulse_width_std(clamp01(c.pw_std));
        t_generator.Process(t_external, &t_reset, t_clock, ramps, gates_, size);

        /* ---- X / Y section (marbles.cc:298-386) ------------------------- */
        const float note_cv = cv_volts / 5.0f;              /* scaled_raw_cv */
        const float u       = note_filter_.Process(0.5f * (note_cv + 1.0f));

        GroupSettings x, y;
        x.control_mode   = ControlMode(c.x_mode < 3 ? c.x_mode : 0);
        x.voltage_range  = VoltageRange(c.x_range % 3);
        x.register_mode  = c.ext_x;
        x.register_value = u;
        x.spread         = x_spread;
        x.bias           = x_bias;
        x.steps          = x_steps;
        x.deja_vu        = c.x_deja_vu == DEJA_VU_LOCKED ? 0.5f
                         : (c.x_deja_vu == DEJA_VU_ON ? deja_vu : 0.0f);
        x.length         = length;
        x.ratio.p        = 1;
        x.ratio.q        = 1;

        y.control_mode   = CONTROL_MODE_IDENTICAL;
        y.voltage_range  = VoltageRange(c.y_range % 3);
        y.register_mode  = false;
        y.register_value = 0.0f;
        y.spread         = clamp01(c.y_spread);
        y.bias           = clamp01(c.y_bias);
        y.steps          = clamp01(c.y_steps);
        y.deja_vu        = 0.0f;
        y.length         = 1;
        y.ratio          = kYDividerRatios[c.y_divider < 12 ? c.y_divider : 6];

        y.scale_index = x.scale_index = c.x_scale < 6 ? c.x_scale : 0;

        bool x_reset = false;
        if (xy_source != CLOCK_SOURCE_EXTERNAL) x_reset |= t_reset;
        xy_generator.Process(xy_source, x, y, &x_reset, xy_clock, ramps, voltages_, size);

        /* ---- outputs ---------------------------------------------------- */
        /* voltages_ is interleaved X1 X2 X3 Y per sample; gates_ T1 T3.
         * T2 is the master ramp's first half (marbles.cc:392-397). */
        for (int j = 0; j < 4; j++) out->v[j] = voltages_[4 * (size - 1) + j];
        for (size_t i = 0; i < size; i++)
        {
            uint8_t now = (uint8_t)((gates_[2 * i] ? 1u : 0u)
                                  | (ramps.master[i] < 0.5f ? 2u : 0u)
                                  | (gates_[2 * i + 1] ? 4u : 0u));
            const uint8_t late = delay_[delay_pos_];
            delay_[delay_pos_] = now;
            delay_pos_         = (delay_pos_ + 1) % kGateDelay;
            out->t[0][i] = late & 1u;
            out->t[1][i] = late & 2u;
            out->t[2][i] = late & 4u;
        }
    }

    /* Test access: the whole block's voltages (X1 X2 X3 Y interleaved). */
    const float* voltages() const { return voltages_; }

  private:
    ClockIn     t_clk_, x_clk_;
    NoteFilter  note_filter_;
    stmlib::HysteresisQuantizer2 length_quantizer_;
    float       ramp_[4][kMaxBlock];
    bool        gates_[2 * kMaxBlock];
    float       voltages_[4 * kMaxBlock];
    uint8_t     delay_[kGateDelay];
    size_t      delay_pos_ = 0;
    bool        smoothed_  = false;
    float       s_rate_ = 0, s_tb_ = 0, s_tj_ = 0, s_dv_ = 0;
    float       s_xs_ = 0, s_xb_ = 0, s_st_ = 0, s_len_ = 0;
    bool        dv_lock_ = false;
    int         last_length_ = 1;
    bool        last_t_external_ = false;
    ClockSource last_xy_source_ = CLOCK_SOURCE_INTERNAL_T1_T2_T3;
};

}  // namespace marbles_port
