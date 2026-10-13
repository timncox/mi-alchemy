/*
 * Native test for src/common/state_file.h, the on-card format of a
 * firmware's autosaved panel (home_state.h):
 *   1. a sealed payload opens and round-trips byte for byte
 *   2. CRC-32 matches the IEEE check value ("123456789" -> CBF43926)
 *   3. a flipped payload byte, a changed schema hash, a payload of the
 *      wrong length, a truncated file and a bad magic are all refused
 */
#include <cstdio>
#include <cstring>

#include "../src/common/state_file.h"

using namespace mi::state_file;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

int main()
{
    uint8_t buf[kHeaderLen + 256];
    const size_t  plen   = 200;
    const uint32_t schema = 0xC0FFEE11u;
    for (size_t i = 0; i < plen; i++) buf[kHeaderLen + i] = (uint8_t)(i * 37 + 5);
    uint8_t ref[200];
    memcpy(ref, buf + kHeaderLen, plen);
    const size_t total = Seal(buf, schema, plen);

    printf("1. round trip\n");
    size_t len = 0;
    CHECK(total == kHeaderLen + plen, "length %zu", total);
    CHECK(Open(buf, total, schema, plen, len) && len == plen, "sealed file does not open");
    CHECK(memcmp(buf + kHeaderLen, ref, plen) == 0, "payload changed");

    printf("2. CRC-32 check value\n");
    CHECK(Crc32((const uint8_t*)"123456789", 9) == 0xCBF43926u, "crc32 %08x",
          Crc32((const uint8_t*)"123456789", 9));

    printf("3. refusals\n");
    buf[kHeaderLen + 17] ^= 0x40u;
    CHECK(!Open(buf, total, schema, plen, len), "flipped byte accepted");
    buf[kHeaderLen + 17] ^= 0x40u;
    CHECK(!Open(buf, total, schema ^ 1u, plen, len), "other schema accepted");
    CHECK(!Open(buf, total, schema, plen + 4, len), "wrong live size accepted");
    CHECK(!Open(buf, total - 1, schema, plen, len), "truncated file accepted");
    CHECK(!Open(buf, kHeaderLen - 1, schema, plen, len), "short header accepted");
    buf[0] ^= 1u;
    CHECK(!Open(buf, total, schema, plen, len), "bad magic accepted");
    buf[0] ^= 1u;
    CHECK(Open(buf, total, schema, plen, len), "restored file no longer opens");

    if (failures) { printf("state_file: %d FAILURE(S)\n", failures); return 1; }
    printf("state_file: all checks passed\n");
    return 0;
}
