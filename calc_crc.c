#include "calc_crc.h"

uint32_t crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0;

    while (len--) {
        crc ^= (uint32_t)*data++;

        for (int i = 0; i < 8; i++) {
            if (crc & 0x80000000)
                crc = (crc << 1) ^ 0x04C11DB7;
            else
                crc <<= 1;
        }
    }

    return crc;
}
