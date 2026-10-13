/*
 * The on-card format of a firmware's autosaved panel (home_state.h): a
 * 24-byte header, then exactly the bytes Presets::SerializeLive() gives.
 * Pure functions, no FatFS, so test/test_state_file.cpp runs them natively.
 *
 *   u32 magic 'MIST'   u16 version (1)   u16 header length (24)
 *   u32 schema hash    (Presets::LiveSchemaHash(), as a QSPI slot stamps)
 *   u32 payload length u32 CRC-32 of the payload   u32 reserved (0)
 *
 * Little-endian, as the M7 writes it. A file whose schema hash differs is
 * treated as absent, the way Presets::Load treats a slot: the firmware
 * changed shape, so the panel starts from defaults instead of misreading.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mi {
namespace state_file {

constexpr uint32_t kMagic     = 0x5453494Du;   /* "MIST" */
constexpr uint16_t kVersion   = 1u;
constexpr size_t   kHeaderLen = 24u;

/* CRC-32 (IEEE, reflected), bitwise: a state file is a few hundred bytes. */
inline uint32_t Crc32(const uint8_t* p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
    {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

inline void Put32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
inline uint32_t Get32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Write the header in front of a payload already at buf + kHeaderLen.
 * Returns the file length. */
inline size_t Seal(uint8_t* buf, uint32_t schema, size_t payload_len)
{
    Put32(buf + 0, kMagic);
    buf[4] = (uint8_t)kVersion;
    buf[5] = (uint8_t)(kVersion >> 8);
    buf[6] = (uint8_t)kHeaderLen;
    buf[7] = 0u;
    Put32(buf + 8, schema);
    Put32(buf + 12, (uint32_t)payload_len);
    Put32(buf + 16, Crc32(buf + kHeaderLen, payload_len));
    Put32(buf + 20, 0u);
    return kHeaderLen + payload_len;
}

/* Check a file read into @p buf (@p n bytes). On success @p payload_len is
 * set and the payload starts at buf + kHeaderLen. */
inline bool Open(const uint8_t* buf, size_t n, uint32_t schema, size_t expect_len,
                 size_t& payload_len)
{
    if (n < kHeaderLen) return false;
    if (Get32(buf) != kMagic) return false;
    if ((uint16_t)(buf[4] | (buf[5] << 8)) != kVersion) return false;
    if (buf[6] != kHeaderLen) return false;
    if (Get32(buf + 8) != schema) return false;
    const uint32_t len = Get32(buf + 12);
    if (len != expect_len || kHeaderLen + len > n) return false;
    if (Get32(buf + 16) != Crc32(buf + kHeaderLen, len)) return false;
    payload_len = len;
    return true;
}

}  // namespace state_file
}  // namespace mi
