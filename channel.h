#ifndef CHANNEL_H
#define CHANNEL_H

#include "models.h"
#include "cdma.h"
#include "udp.h"

#include <netinet/in.h>

#define MAX_STATIONS 50

#define SLOT_TIME_US 10000
#define REQUEST_WINDOW_US 5000
#define PROPAGATION_DELAY_US 500

typedef enum {
    CHANNEL_IDLE,
    CHANNEL_ACTIVE,
    CHANNEL_COLLISION
} channel_state_t;

typedef struct {
    int valid;
    int station_id;
    unsigned long slot;
    shared_frame_t frame;
} transmission_type_t;

typedef struct {
    int valid;
    unsigned long slot;
    int collision;
    int success;
    int collision_detected;
    unsigned long detection_time_us;
} station_result_t;

typedef struct {
    channel_state_t state;
    int num_stations;
    int walsh_len;
    unsigned long slot;
    unsigned long successful_transmissions;
    unsigned long active_slots;
    unsigned long idle_slots;
    unsigned long collisions;
    unsigned long long total_delay_slots;
    unsigned long total_delay_events;
} channel_stats_t;

typedef struct {
    int fd;
    int num_stations;
    int walsh_len;
    struct sockaddr_in control_addr[MAX_STATIONS];
    struct sockaddr_in data_addr[MAX_STATIONS];
    transmission_type_t requests[MAX_STATIONS];
    int walsh_codes[MAX_STATIONS][MAX_WALSH_LEN];
    channel_stats_t stats;
} channel_t;

int channel_init(channel_t *channel, int num_stations);
void channel_destroy(channel_t *channel);

int channel_wait_for_hellos(channel_t *channel);

void channel_start_slot(channel_t *channel, unsigned long slot);
int channel_collect_requests(channel_t *channel);
channel_state_t channel_evaluate(channel_t *channel, int *num_active);
int channel_send_results(channel_t *channel, unsigned long slot);
int channel_forward_frames(channel_t *channel, unsigned long slot);

int channel_run_slot(channel_t *channel, unsigned long slot);
int channel_drain_metrics(channel_t *channel);
void channel_shutdown_stations(channel_t *channel);

const channel_stats_t *channel_get_stats(const channel_t *channel);

#endif
