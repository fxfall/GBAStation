#ifndef ROMX_BINARY_INTERNAL_H
#define ROMX_BINARY_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

static inline uint16_t romx_read_le16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8U));
}

static inline uint32_t romx_read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) |
        ((uint32_t)bytes[2] << 16U) | ((uint32_t)bytes[3] << 24U);
}

static inline uint64_t romx_read_le64(const uint8_t *bytes)
{
    return (uint64_t)romx_read_le32(bytes) |
        ((uint64_t)romx_read_le32(bytes + 4U) << 32U);
}

static inline void romx_write_le16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
}

static inline void romx_write_le32(uint8_t *bytes, uint32_t value)
{
    unsigned int index;
    for (index = 0U; index < 4U; ++index)
        bytes[index] = (uint8_t)(value >> (index * 8U));
}

static inline void romx_write_le64(uint8_t *bytes, uint64_t value)
{
    romx_write_le32(bytes, (uint32_t)value);
    romx_write_le32(bytes + 4U, (uint32_t)(value >> 32U));
}

static inline int romx_bytes_zero(const uint8_t *bytes, size_t size)
{
    size_t index;
    for (index = 0U; index < size; ++index)
        if (bytes[index] != 0U) return 0;
    return 1;
}

static inline int romx_add_seek_offset(uint64_t base, int64_t offset,
    uint64_t *target)
{
    uint64_t magnitude;
    if (offset >= 0) {
        magnitude = (uint64_t)offset;
        if (base > UINT64_MAX - magnitude) return 0;
        *target = base + magnitude;
    } else {
        magnitude = (uint64_t)(-(offset + INT64_C(1))) + UINT64_C(1);
        if (magnitude > base) return 0;
        *target = base - magnitude;
    }
    return 1;
}

#endif
