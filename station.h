#ifndef STATION_H
#define STATION_H

#include "channel.h"
#include "cdma.h"

#include <stdint.h>
#include <netinet/in.h>

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

} station_t;

void station_run(station_t *station);

#endif
