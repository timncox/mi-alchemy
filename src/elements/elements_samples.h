/*
 * elements_samples -- Elements' exciter samples, on the SD card instead of
 * in the firmware image.
 *
 * Upstream's resources.cc carries two sample tables: smp_sample_data (the
 * nine strike / mallet recordings, 128,013 int16) and smp_noise_sample (the
 * blow exciter's noise loop, 40,963 int16) -- 338 KB of a 373 KB file, and
 * more than the 480 KB SRAM image has room for next to the SDK. So the
 * firmware links a copy of resources.cc with those two arrays cut out (see
 * fw.mk), gives their symbols SDRAM storage (elements_storage.cpp), and
 * fills that storage at boot from `0:/mi/elements.smp`.
 *
 * File format, little-endian, 24-byte header then the payload:
 *
 *   0   u32  magic    'M' 'I' 'E' 'S'
 *   4   u32  version  1
 *   8   u32  count of smp_sample_data   (int16 samples, 128013)
 *  12   u32  count of smp_noise_sample  (int16 samples, 40963)
 *  16   u32  CRC-32 (IEEE, zlib's) of the payload bytes
 *  20   u32  reserved, 0
 *  24        smp_sample_data then smp_noise_sample, int16 LE
 *
 * Written by tools/elements_samples.cpp, which #includes the vendored
 * resources.cc, so the file is those exact arrays. Read through the
 * streaming Loader below by both the firmware (4 KB chunks from FatFS) and
 * the native test (the same Loader over the tool's output).
 *
 * Plain C++, no SDK or libDaisy: it compiles on the host unchanged.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace es {

constexpr uint32_t kMagic       = 0x5345494Du;   /* "MIES" in file order */
constexpr uint32_t kVersion     = 1u;
constexpr size_t   kHeaderBytes = 24u;

/* The vendored tables' lengths (eurorack 08460a6); the tool and the test
 * static_assert these against the arrays themselves. */
constexpr uint32_t kSampleCount = 128013u;
constexpr uint32_t kNoiseCount  = 40963u;
constexpr size_t   kPayloadBytes = 2u * ((size_t)kSampleCount + kNoiseCount);
constexpr size_t   kFileBytes    = kHeaderBytes + kPayloadBytes;

/* Where the firmware looks. Not /alchemy: the picker lists that folder. */
constexpr const char* kPath = "0:/mi/elements.smp";

enum class Status : uint8_t
{
    Ok = 0,
    Missing,     /* no card, or no file */
    BadHeader,   /* magic / version / counts do not match this build */
    Short,       /* file ended early, or a read failed */
    BadCrc,      /* payload does not match the header's CRC */
};

const char* StatusName(Status s);

/* zlib-compatible CRC-32: crc = Crc32(crc, p, n), starting from 0. */
uint32_t Crc32(uint32_t crc, const void* p, size_t n);

/* Fill a 24-byte header for a payload with CRC `crc`. */
void MakeHeader(uint8_t out[kHeaderBytes], uint32_t crc);

/*
 * Streaming reader. Feed() the file in pieces of any size (the header may
 * straddle pieces); the payload is copied straight into the two
 * destinations. Finish() says whether what arrived is a whole, intact file.
 * On anything but Ok the caller should zero both destinations: silence is
 * the graceful failure (the strike and blow exciters lose their samples,
 * everything else in Elements still sounds).
 */
class Loader
{
  public:
    void Begin(int16_t* sample_data, int16_t* noise);
    /* False once the header has been seen and refused (stop reading). */
    bool Feed(const uint8_t* p, size_t n);
    Status Finish() const;

  private:
    int16_t* data_  = nullptr;
    int16_t* noise_ = nullptr;
    uint8_t  hdr_[kHeaderBytes];
    size_t   hdr_got_ = 0;
    bool     hdr_bad_ = false;
    size_t   got_     = 0;       /* payload bytes placed */
    size_t   extra_   = 0;       /* bytes beyond the payload */
    uint32_t crc_     = 0;
    uint32_t want_    = 0;
};

} // namespace es
