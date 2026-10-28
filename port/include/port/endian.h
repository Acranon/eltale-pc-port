#ifndef ELTALE_PORT_ENDIAN_H
#define ELTALE_PORT_ENDIAN_H

#include <stdint.h>
#include <string.h>

/* DMA copies bytes. Interpret multi-byte ROM/save fields at the read site. */
static inline uint16_t Port_ReadBE16(const void *src) {
    const uint8_t *p = src;
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static inline uint32_t Port_ReadBE32(const void *src) {
    const uint8_t *p = src;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8 | p[3];
}

static inline float Port_ReadBEF32(const void *src) {
    uint32_t bits = Port_ReadBE32(src);
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static inline void Port_WriteBE16(void *dst, uint16_t value) {
    uint8_t *p = dst;
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static inline void Port_WriteBE32(void *dst, uint32_t value) {
    uint8_t *p = dst;
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

#endif
