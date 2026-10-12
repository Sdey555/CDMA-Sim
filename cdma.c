#include "cdma.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int bit_parity(unsigned int v)
{
    int p = 0;
    while (v) {
        p ^= (v & 1U);
        v >>= 1;
    }
    return p;
}

int cdma_get_walsh_len(int num_stations)
{
    int len = 2;
    while (len < num_stations && len < MAX_WALSH_LEN)
        len <<= 1;
    if (len > MAX_WALSH_LEN)
        len = MAX_WALSH_LEN;
    return len;
}

void cdma_get_station_code(int station_id, int walsh_len, int *code_out)
{
    if (!code_out || walsh_len <= 0)
        return;

    for (int col = 0; col < walsh_len; col++) {
        unsigned int prod = (unsigned int)(station_id & col);
        code_out[col] = (bit_parity(prod) == 0) ? 1 : -1;
    }
}

int cdma_verify_orthogonality(int num_stations, int walsh_len)
{
    int c1[MAX_WALSH_LEN];
    int c2[MAX_WALSH_LEN];

    for (int i = 0; i < num_stations; i++) {
        cdma_get_station_code(i, walsh_len, c1);
        for (int j = 0; j < num_stations; j++) {
            cdma_get_station_code(j, walsh_len, c2);

            int dot = 0;
            for (int k = 0; k < walsh_len; k++)
                dot += c1[k] * c2[k];

            if (i == j && dot != walsh_len)
                return 0;
            if (i != j && dot != 0)
                return 0;
        }
    }
    return 1;
}

int cdma_should_transmit(double p)
{
    if (p <= 0.0)
        return 0;
    if (p >= 1.0)
        return 1;

    double r = (double)rand() / ((double)RAND_MAX + 1.0);
    return (r < p) ? 1 : 0;
}

void cdma_spread_data(const uint8_t *input_bytes, size_t num_bytes,
                      const int *walsh_code, int walsh_len,
                      int8_t *chips_out)
{
    if (!input_bytes || !walsh_code || !chips_out)
        return;

    size_t bit_idx = 0;
    for (size_t i = 0; i < num_bytes; i++) {
        uint8_t byte = input_bytes[i];
        for (int b = 7; b >= 0; b--) {
            int bit = (byte >> b) & 1;
            int val = (bit == 1) ? 1 : -1;
            for (int k = 0; k < walsh_len; k++) {
                chips_out[bit_idx * walsh_len + k] = (int8_t)(val * walsh_code[k]);
            }
            bit_idx++;
        }
    }
}

void cdma_channel_combine(int16_t *composite_chips, const int8_t *station_chips,
                          size_t num_chips)
{
    if (!composite_chips || !station_chips)
        return;

    for (size_t i = 0; i < num_chips; i++) {
        composite_chips[i] = (int16_t)(composite_chips[i] + station_chips[i]);
    }
}

int cdma_despread_data(const int16_t *composite_chips, size_t num_bytes,
                       const int *walsh_code, int walsh_len,
                       uint8_t *output_bytes)
{
    if (!composite_chips || !walsh_code || !output_bytes)
        return 0;

    memset(output_bytes, 0, num_bytes);
    size_t bit_idx = 0;

    for (size_t i = 0; i < num_bytes; i++) {
        uint8_t byte_val = 0;
        for (int b = 7; b >= 0; b--) {
            int32_t accumulator = 0;
            for (int k = 0; k < walsh_len; k++) {
                accumulator += composite_chips[bit_idx * walsh_len + k] * walsh_code[k];
            }
            int bit = (accumulator > 0) ? 1 : 0;
            byte_val |= (uint8_t)(bit << b);
            bit_idx++;
        }
        output_bytes[i] = byte_val;
    }

    return 1;
}

void cdma_print_code(int station_id, const int *code, int len)
{
    printf("Station %2d Walsh code: [", station_id);
    for (int i = 0; i < len; i++) {
        printf("%s%d", (i > 0) ? " " : "", code[i]);
    }
    printf("]\n");
}
