/*
 * 48 kHz <-> 32 kHz stereo resampling for Beads' 32 kHz quality.
 *
 * Beads restarts its codec at the rate each quality runs at; for the
 * "Clouds" quality (Processor::MEDIA_VINTAGE_DIGITAL) that rate is 32 kHz,
 * and the codec's own anti-alias filter is part of the sound. The Lab keeps
 * its SAI at 48 kHz (the SDK's pots, CV jacks and CPU meter are set up for
 * the rate given at boot), so this pair stands in for the codec: 12 frames
 * at 48 kHz in -> 8 frames at 32 kHz for the engine -> 12 frames out.
 *
 * Both directions are a 96-tap windowed sinc at the common 96 kHz rate
 * (up 2 / down 3 and up 3 / down 2), evaluated polyphase: only the taps
 * that land on a real sample are summed. Cutoff 13.5 kHz, Blackman window:
 * flat to ~12 kHz, about -74 dB by 16 kHz.
 */
#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>

namespace mi_beads {

struct StereoF { float l, r; };

class Resample32k
{
  public:
    static constexpr int kTaps = 96;

    void Init()
    {
        const double fc = 13500.0 / 96000.0;   /* cycles per 96 kHz sample */
        const double c  = 0.5 * (kTaps - 1);
        double       sum = 0.0;
        for (int k = 0; k < kTaps; k++)
        {
            const double x = k - c;
            const double s = x == 0.0 ? 2.0 * fc : sin(2.0 * M_PI * fc * x) / (M_PI * x);
            const double w = 0.42 - 0.5 * cos(2.0 * M_PI * k / (kTaps - 1))
                             + 0.08 * cos(4.0 * M_PI * k / (kTaps - 1));
            h_[k] = (float)(s * w);
            sum += s * w;
        }
        for (int k = 0; k < kTaps; k++) h_[k] = (float)(h_[k] / sum);
        Reset();
    }

    void Reset()
    {
        for (uint64_t i = 0; i < kHist; i++) hin_[i] = hout_[i] = {0.f, 0.f};
        n_in_ = n_mid_ = n_up_in_ = n_up_out_ = 0;
    }

    /* 48 kHz -> 32 kHz: @p n48 frames in (a multiple of 3), 2/3 as many out. */
    size_t Down(const StereoF* in, StereoF* out, size_t n48)
    {
        size_t m_out = 0;
        for (size_t i = 0; i < n48; i++)
        {
            hin_[n_in_ & kMask] = in[i];
            ++n_in_;
            /* Output m reads up-sampled index 3m, i.e. input frames up to
             * floor(3m / 2): emit every m whose newest input has arrived. */
            while ((3u * n_mid_) / 2u + 1u <= n_in_)
            {
                const uint64_t j0 = 3u * n_mid_;   /* in the 96 kHz grid */
                StereoF        acc = {0.f, 0.f};
                /* taps k with (j0 - k) even: input frame (j0 - k) / 2 */
                for (int k = (int)(j0 & 1u); k < kTaps && (uint64_t)k <= j0; k += 2)
                {
                    const StereoF& x = hin_[((j0 - (uint64_t)k) / 2u) & kMask];
                    acc.l += h_[k] * x.l;
                    acc.r += h_[k] * x.r;
                }
                out[m_out].l = 2.0f * acc.l;   /* zero-stuffing gain */
                out[m_out].r = 2.0f * acc.r;
                ++m_out;
                ++n_mid_;
            }
        }
        return m_out;
    }

    /* 32 kHz -> 48 kHz: @p n32 frames in (even), 3/2 as many out. */
    size_t Up(const StereoF* in, StereoF* out, size_t n32)
    {
        size_t n_out = 0;
        for (size_t i = 0; i < n32; i++)
        {
            hout_[n_up_in_ & kMask] = in[i];
            ++n_up_in_;
            /* Output n reads up-sampled index 2n, i.e. engine frames up to
             * floor(2n / 3). */
            while ((2u * n_up_out_) / 3u + 1u <= n_up_in_)
            {
                const uint64_t j0  = 2u * n_up_out_;
                StereoF        acc = {0.f, 0.f};
                for (int k = (int)(j0 % 3u); k < kTaps && (uint64_t)k <= j0; k += 3)
                {
                    const StereoF& x = hout_[((j0 - (uint64_t)k) / 3u) & kMask];
                    acc.l += h_[k] * x.l;
                    acc.r += h_[k] * x.r;
                }
                out[n_out].l = 3.0f * acc.l;
                out[n_out].r = 3.0f * acc.r;
                ++n_out;
                ++n_up_out_;
            }
        }
        return n_out;
    }

  private:
    static constexpr uint64_t kHist = 64u;   /* > kTaps / 2 */
    static constexpr uint64_t kMask = kHist - 1u;

    float    h_[kTaps];
    StereoF  hin_[kHist], hout_[kHist];
    /* Frame counters: 64-bit, so they never wrap (a 32-bit 96 kHz grid
     * index would after ~12 h). */
    uint64_t n_in_ = 0, n_mid_ = 0;
    uint64_t n_up_in_ = 0, n_up_out_ = 0;
};

}  // namespace mi_beads
