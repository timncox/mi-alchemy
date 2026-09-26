/*
 * elements_samples -- write Elements' exciter samples as the SD-card file
 * the firmware loads (format: src/elements/elements_samples.h).
 *
 *   c++ -std=gnu++14 -DTEST -Ivendor/eurorack -Isrc/elements \
 *       tools/elements_samples.cpp src/elements/elements_samples.cpp -o t
 *   ./t build-elements/elements.smp
 *
 * src/elements/fw.mk builds and runs it. It #includes the vendored
 * resources.cc, so the file holds exactly the arrays upstream compiled in,
 * and sizeof() checks the counts the firmware was built with.
 */
#include <stdio.h>
#include <string.h>

#include "elements/resources.cc"
#include "elements_samples.h"

static_assert(sizeof(elements::smp_sample_data) / 2 == es::kSampleCount,
              "smp_sample_data length changed: update es::kSampleCount");
static_assert(sizeof(elements::smp_noise_sample) / 2 == es::kNoiseCount,
              "smp_noise_sample length changed: update es::kNoiseCount");

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "usage: %s out.smp\n", argv[0]);
        return 2;
    }
    /* The payload is the arrays' bytes; the M7 and every build host are
     * little-endian, which is what the format says. */
    const uint16_t probe = 1;
    if (*reinterpret_cast<const uint8_t*>(&probe) != 1)
    {
        fprintf(stderr, "big-endian host: not supported\n");
        return 1;
    }
    uint32_t crc = es::Crc32(0, elements::smp_sample_data, sizeof elements::smp_sample_data);
    crc = es::Crc32(crc, elements::smp_noise_sample, sizeof elements::smp_noise_sample);

    uint8_t hdr[es::kHeaderBytes];
    es::MakeHeader(hdr, crc);

    FILE* f = fopen(argv[1], "wb");
    if (!f) { perror(argv[1]); return 1; }
    const bool ok = fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr
                    && fwrite(elements::smp_sample_data, 1, sizeof elements::smp_sample_data, f)
                           == sizeof elements::smp_sample_data
                    && fwrite(elements::smp_noise_sample, 1, sizeof elements::smp_noise_sample, f)
                           == sizeof elements::smp_noise_sample;
    if (fclose(f) != 0 || !ok) { perror(argv[1]); return 1; }
    printf("%s: %zu bytes, crc32 %08x\n", argv[1], es::kFileBytes, (unsigned)crc);
    return 0;
}
