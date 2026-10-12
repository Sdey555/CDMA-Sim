#ifndef STATION_H
#define STATION_H

#include "channel.h"
#include "cdma.h"
#include "models.h"

#include <stdint.h>
#include <netinet/in.h>

typedef struct {
    int dest_id;
    int show_walsh_code;
    uint32_t payload_length;
    char payload[MAX_PAYLOAD_LENGTH];
} station_cmd_t;

typedef struct {
    int id;
    int destination;
    double p;
    int walsh_len;
    int walsh_code[MAX_WALSH_LEN];
    int running;

    int control_fd;
    int data_fd;

    struct sockaddr_in *channel_addr_ptr;
    struct sockaddr_in *data_addr_ptr;

    int packet_pending;
    uint8_t pending_sequence_no;
    unsigned int retry_count;
    unsigned long total_collisions;
    unsigned long sequence_no;
    unsigned long attempt_slot;
    unsigned long first_attempt_slot;
    unsigned long successful_frames;

    /* Terminal input / command pipe */
    int cmd_pipe_fd;
    int show_walsh_code;
    int has_terminal_payload;
    uint32_t pending_payload_len;
    char pending_payload[MAX_PAYLOAD_LENGTH];

} station_t;

void station_run(station_t *station);
int station_run_standalone(int station_id, int num_stations, int destination);
void station_print_bitwise_walsh(int station_id, const int *walsh_code, int walsh_len,
                                 const uint8_t *payload, size_t payload_len);

#endif
