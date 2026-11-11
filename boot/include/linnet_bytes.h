#ifndef LINNET_BYTES_H
#define LINNET_BYTES_H
#include <stdint.h>

static inline uint32_t ln_read_be16(const uint8_t *p)
{
    return ((uint32_t)p[0] << 8) | p[1];
}

static inline uint32_t ln_read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
        ((uint32_t)p[2] << 8) | p[3];
}

static inline uint64_t ln_read_be64(const uint8_t *p)
{
    return ((uint64_t)ln_read_be32(p) << 32) | ln_read_be32(p + 4);
}

static inline uint16_t ln_read_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t ln_read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t ln_read_le64(const uint8_t *p)
{
    return ln_read_le32(p) | ((uint64_t)ln_read_le32(p + 4) << 32);
}

static inline void ln_write_le16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static inline void ln_write_le32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8u * i));
}

static inline void ln_write_le64(uint8_t *p, uint64_t value)
{
    ln_write_le32(p, (uint32_t)value);
    ln_write_le32(p + 4, (uint32_t)(value >> 32));
}

static inline int ln_bytes_equal(const uint8_t *a, const uint8_t *b, uint32_t length)
{
    for (uint32_t i = 0; i < length; ++i)
        if (a[i] != b[i]) return 0;
    return 1;
}

static inline int ln_bytes_zero(const uint8_t *p, uint32_t length)
{
    for (uint32_t i = 0; i < length; ++i)
        if (p[i]) return 0;
    return 1;
}
#endif
