#ifndef CDMA_H
#define CDMA_H

#include <stddef.h>
#include <stdint.h>

#define MAX_WALSH_LEN 64

/*
 * Returns the smallest power-of-two Walsh code length >= num_stations,
 * clamped between 2 and MAX_WALSH_LEN.
 */
int cdma_get_walsh_len(int num_stations);

/*
 * Populates code_out with the unique length-walsh_len Walsh code for station_id.
 * Each entry is +1 or -1.
 */
void cdma_get_station_code(int station_id, int walsh_len, int *code_out);

/*
 * Verifies that the Walsh codes for all stations [0..num_stations-1]
 * are mutually orthogonal (inner product == 0 for i != j, and == walsh_len for i == j).
 * Returns 1 if valid, 0 otherwise.
 */
int cdma_verify_orthogonality(int num_stations, int walsh_len);

/*
 * Decide whether station should transmit in current slot based on traffic load / probability p.
 * In CDMA, channel is shared via orthogonal codes, so no carrier sensing is needed.
 * Returns 1 to transmit, 0 to defer.
 */
int cdma_should_transmit(double p);

/*
 * Spread raw byte data into chips using a station's Walsh code.
 * input_bytes: buffer containing frame data
 * num_bytes: length of input_bytes
 * walsh_code: length-walsh_len array of +1 / -1
 * walsh_len: chip sequence length
 * chips_out: pre-allocated output array of size (num_bytes * 8 * walsh_len)
 */
void cdma_spread_data(const uint8_t *input_bytes, size_t num_bytes,
                      const int *walsh_code, int walsh_len,
                      int8_t *chips_out);

/*
 * Superimpose (add) one station's transmitted chips into the shared channel composite signal.
 * composite_chips: accumulator of received chips
 * station_chips: chips from one station
 * num_chips: total number of chips
 */
void cdma_channel_combine(int16_t *composite_chips, const int8_t *station_chips,
                          size_t num_chips);

/*
 * Despread the composite channel signal using a station's Walsh code to recover bytes.
 * composite_chips: sum of chips from all active stations
 * num_bytes: number of bytes to reconstruct
 * walsh_code: sender station's Walsh code of length walsh_len
 * output_bytes: pre-allocated output buffer for recovered data
 *
 * Returns 1 on success, 0 on failure.
 */
int cdma_despread_data(const int16_t *composite_chips, size_t num_bytes,
                       const int *walsh_code, int walsh_len,
                       uint8_t *output_bytes);

/*
 * Print Walsh code for a station in human-readable format.
 */
void cdma_print_code(int station_id, const int *code, int len);

#endif /* CDMA_H */
