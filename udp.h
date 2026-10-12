#ifndef UDP_H
#define UDP_H

#include <stdint.h>
#include <stddef.h>
#include "models.h"

#define UDP_CHANNEL_PORT 46000
#define UDP_CONTROL_BASE 47000
#define UDP_DATA_BASE 48000
#define UDP_LOOPBACK "127.0.0.1"
#define UDP_MAX_FRAME_SIZE MAX_FRAME_SIZE

typedef enum {
    UDP_MSG_HELLO = 1,
    UDP_MSG_SLOT_STATUS,
    UDP_MSG_TX_REQUEST,
    UDP_MSG_SLOT_RESULT,
    UDP_MSG_FRAME_DATA,
    UDP_MSG_METRIC,
    UDP_MSG_SHUTDOWN
} udp_message_type_t;

typedef struct {
    uint32_t type;
    uint64_t slot;
    uint32_t channel_state;
} udp_slot_status_t;

typedef struct {
    uint32_t type;
    int32_t station_id;
} udp_hello_t;

typedef struct {
    uint32_t type;
    int32_t station_id;
    uint64_t slot;
    uint32_t frame_length;
    uint8_t frame[UDP_MAX_FRAME_SIZE];
} udp_tx_request_t;

typedef struct {
    uint32_t type;
    uint64_t slot;
    int32_t station_id;
    uint32_t collision;
    uint32_t success;
    uint32_t collision_detected;
    uint64_t detection_time_us;
} udp_slot_result_t;

typedef struct {
    uint32_t type;
    uint64_t slot;
    uint32_t frame_length;
    uint8_t frame[UDP_MAX_FRAME_SIZE];
} udp_frame_data_t;

typedef struct {
    uint32_t type;
    int32_t station_id;
    uint64_t slot;
    uint64_t delay_slots;
} udp_metric_t;

typedef struct {
    uint32_t type;
} udp_shutdown_t;

int udp_make_socket(void);
int udp_bind_port(int fd, int port);
int udp_make_address(const char *ip, int port, void *out_addr, size_t addr_size);

#endif
