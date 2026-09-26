/*
 * SDRAM storage for Elements' two sample tables.
 *
 * The engine refers to elements::smp_sample_data and
 * elements::smp_noise_sample (resources.h: `extern const int16_t x[]`), and
 * the image's copy of resources.cc has both definitions cut out (fw.mk).
 * These arrays take the two symbols' mangled names, so every reference in
 * the vendored DSP -- exciter.cc and resources.cc's own sample_table --
 * links here, unchanged. They are writable here and const to every reader:
 * this translation unit deliberately does not include resources.h, so no
 * compiler ever sees both declarations at once. Filled at boot by
 * es::Loader from the SD card, zeroed when that fails.
 *
 * .sdram_bss is NOLOAD: the loader or the zeroing writes every byte before
 * the audio starts.
 */
#include <stdint.h>

#include "elements_samples.h"

int16_t g_smp_sample_data[es::kSampleCount]
    __asm__("_ZN8elements15smp_sample_dataE")
    __attribute__((section(".sdram_bss"), aligned(32)));

int16_t g_smp_noise_sample[es::kNoiseCount]
    __asm__("_ZN8elements16smp_noise_sampleE")
    __attribute__((section(".sdram_bss"), aligned(32)));

/* The shim's handles on the same storage (the names above are the engine's
 * symbols, which no C++ declaration here can spell). */
int16_t* es_sample_storage() { return g_smp_sample_data; }
int16_t* es_noise_storage()  { return g_smp_noise_sample; }
