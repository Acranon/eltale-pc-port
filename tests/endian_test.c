#include "port/endian.h"

#include <assert.h>
#include <stdint.h>

int main(void) {
    uint8_t b[4] = { 0x12, 0x34, 0x56, 0x78 };
    assert(Port_ReadBE16(b) == 0x1234);
    assert(Port_ReadBE32(b) == 0x12345678);
    Port_WriteBE16(b, 0xABCD);
    assert(b[0] == 0xAB && b[1] == 0xCD);
    Port_WriteBE32(b, 0x89ABCDEF);
    assert(Port_ReadBE32(b) == 0x89ABCDEF);
    const uint8_t one[4] = { 0x3F, 0x80, 0x00, 0x00 };
    assert(Port_ReadBEF32(one) == 1.0f);
    return 0;
}
