/* elements_samples -- see elements_samples.h. */
#include "elements_samples.h"

#include <string.h>

namespace es {

const char* StatusName(Status s)
{
    switch (s)
    {
        case Status::Ok:        return "ok";
        case Status::Missing:   return "missing";
        case Status::BadHeader: return "bad header";
        case Status::Short:     return "short";
        case Status::BadCrc:    return "bad crc";
    }
    return "?";
}

uint32_t Crc32(uint32_t crc, const void* p, size_t n)
{
    /* Bitwise: 338 KB once at boot costs a few ms on the M7, and a 1 KB
     * table would be image space for nothing. */
    const uint8_t* b = static_cast<const uint8_t*>(p);
    crc = ~crc;
    while (n--)
    {
        crc ^= *b++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void put32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
           | ((uint32_t)p[3] << 24);
}

void MakeHeader(uint8_t out[kHeaderBytes], uint32_t crc)
{
    put32(out + 0, kMagic);
    put32(out + 4, kVersion);
    put32(out + 8, kSampleCount);
    put32(out + 12, kNoiseCount);
    put32(out + 16, crc);
    put32(out + 20, 0u);
}

void Loader::Begin(int16_t* sample_data, int16_t* noise)
{
    data_    = sample_data;
    noise_   = noise;
    hdr_got_ = 0;
    hdr_bad_ = false;
    got_     = 0;
    extra_   = 0;
    crc_     = 0;
    want_    = 0;
}

bool Loader::Feed(const uint8_t* p, size_t n)
{
    if (hdr_bad_) return false;
    while (n && hdr_got_ < kHeaderBytes)
    {
        hdr_[hdr_got_++] = *p++;
        n--;
        if (hdr_got_ == kHeaderBytes)
        {
            hdr_bad_ = get32(hdr_ + 0) != kMagic || get32(hdr_ + 4) != kVersion
                       || get32(hdr_ + 8) != kSampleCount
                       || get32(hdr_ + 12) != kNoiseCount;
            want_ = get32(hdr_ + 16);
            if (hdr_bad_) return false;
        }
    }
    if (!n) return true;

    const size_t room = kPayloadBytes - got_;
    const size_t take = n < room ? n : room;
    extra_ += n - take;
    crc_ = Crc32(crc_, p, take);

    /* The payload is one byte stream across both arrays. */
    const size_t split = 2u * (size_t)kSampleCount;
    size_t       done  = 0;
    while (done < take)
    {
        const size_t at = got_ + done;
        size_t       len;
        if (at < split)
        {
            len = take - done < split - at ? take - done : split - at;
            memcpy(reinterpret_cast<uint8_t*>(data_) + at, p + done, len);
        }
        else
        {
            len = take - done;
            memcpy(reinterpret_cast<uint8_t*>(noise_) + (at - split), p + done, len);
        }
        done += len;
    }
    got_ += take;
    return true;
}

Status Loader::Finish() const
{
    if (hdr_got_ == 0) return Status::Missing;
    if (hdr_got_ < kHeaderBytes) return Status::Short;
    if (hdr_bad_) return Status::BadHeader;
    if (got_ < kPayloadBytes) return Status::Short;
    if (extra_) return Status::BadHeader;   /* a longer file is not this format */
    if (crc_ != want_) return Status::BadCrc;
    return Status::Ok;
}

} // namespace es
