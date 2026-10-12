#define _POSIX_C_SOURCE 200809L

#include "channel.h"
#include "framing.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static long long current_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}

static int send_status(const channel_t *channel, int station_id,
                       unsigned long slot, channel_state_t state)
{
    udp_slot_status_t msg = {
        .type = UDP_MSG_SLOT_STATUS,
        .slot = slot,
        .channel_state = (uint32_t)state
    };

    ssize_t n = sendto(channel->fd, &msg, sizeof(msg), 0,
                       (const struct sockaddr *)&channel->control_addr[station_id],
                       sizeof(channel->control_addr[station_id]));
    return n == (ssize_t)sizeof(msg) ? 0 : -1;
}

static int send_result(const channel_t *channel, int station_id,
                       unsigned long slot, int collision, int success, int cd)
{
    udp_slot_result_t msg;
    memset(&msg, 0, sizeof(msg));

    msg.type = UDP_MSG_SLOT_RESULT;
    msg.slot = slot;
    msg.station_id = station_id;
    msg.collision = (uint32_t)collision;
    msg.success = (uint32_t)success;
    msg.collision_detected = (uint32_t)cd;
    msg.detection_time_us = 0;

    ssize_t n = sendto(channel->fd, &msg, sizeof(msg), 0,
                       (const struct sockaddr *)&channel->control_addr[station_id],
                       sizeof(channel->control_addr[station_id]));
    return n == (ssize_t)sizeof(msg) ? 0 : -1;
}

static int send_frame(const channel_t *channel, int destination,
                      unsigned long slot, const shared_frame_t *frame)
{
    udp_frame_data_t *msg = calloc(1, sizeof(*msg));
    if (!msg)
        return -1;

    msg->type = UDP_MSG_FRAME_DATA;
    msg->slot = slot;
    msg->frame_length = (uint32_t)frame->length;
    memcpy(msg->frame, frame->data, frame->length);

    size_t msg_size = offsetof(udp_frame_data_t, frame) + frame->length;
    ssize_t n = sendto(channel->fd, msg, msg_size, 0,
                       (const struct sockaddr *)&channel->data_addr[destination],
                       sizeof(channel->data_addr[destination]));

    free(msg);
    return n == (ssize_t)msg_size ? 0 : -1;
}

static void send_shutdown(const channel_t *channel, int station_id)
{
    udp_shutdown_t msg = {.type = UDP_MSG_SHUTDOWN};
    sendto(channel->fd, &msg, sizeof(msg), 0,
           (const struct sockaddr *)&channel->control_addr[station_id],
           sizeof(channel->control_addr[station_id]));
}

int channel_init(channel_t *channel, int num_stations)
{
    if (!channel || num_stations < 2 || num_stations > MAX_STATIONS)
        return -1;

    memset(channel, 0, sizeof(*channel));
    channel->num_stations = num_stations;
    channel->walsh_len = cdma_get_walsh_len(num_stations);
    channel->stats.num_stations = num_stations;
    channel->stats.walsh_len = channel->walsh_len;
    channel->stats.state = CHANNEL_IDLE;

    /* Generate unique orthogonal Walsh codes for each station */
    for (int i = 0; i < num_stations; i++) {
        cdma_get_station_code(i, channel->walsh_len, channel->walsh_codes[i]);
    }

    if (!cdma_verify_orthogonality(num_stations, channel->walsh_len)) {
        fprintf(stderr, "Channel error: Walsh code orthogonality verification failed.\n");
        return -1;
    }

    channel->fd = udp_make_socket();
    if (channel->fd < 0)
        return -1;

    if (udp_bind_port(channel->fd, UDP_CHANNEL_PORT) != 0) {
        close(channel->fd);
        channel->fd = -1;
        return -1;
    }

    for (int i = 0; i < num_stations; i++) {
        if (udp_make_address(UDP_LOOPBACK, UDP_CONTROL_BASE + i,
                             &channel->control_addr[i],
                             sizeof(channel->control_addr[i])) != 0 ||
            udp_make_address(UDP_LOOPBACK, UDP_DATA_BASE + i,
                             &channel->data_addr[i],
                             sizeof(channel->data_addr[i])) != 0) {
            channel_destroy(channel);
            return -1;
        }
    }

    return 0;
}

void channel_destroy(channel_t *channel)
{
    if (!channel)
        return;

    if (channel->fd >= 0) {
        close(channel->fd);
        channel->fd = -1;
    }
}

int channel_wait_for_hellos(channel_t *channel)
{
    int registered[MAX_STATIONS] = {0};
    int count = 0;

    while (count < channel->num_stations) {
        udp_hello_t msg;
        struct sockaddr_in src;
        socklen_t src_len = sizeof(src);

        ssize_t n = recvfrom(channel->fd, &msg, sizeof(msg), 0,
                             (struct sockaddr *)&src, &src_len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("recvfrom(hello)");
            return -1;
        }

        if ((size_t)n != sizeof(msg) || msg.type != UDP_MSG_HELLO)
            continue;

        if (msg.station_id < 0 || msg.station_id >= channel->num_stations)
            continue;

        if (!registered[msg.station_id]) {
            registered[msg.station_id] = 1;
            count++;
            printf("Channel: station %d registered\n", msg.station_id);
            fflush(stdout);
        }
    }

    return 0;
}

void channel_start_slot(channel_t *channel, unsigned long slot)
{
    memset(channel->requests, 0, sizeof(channel->requests));
    channel->stats.slot = slot;
    channel->stats.state = CHANNEL_IDLE;

    for (int i = 0; i < channel->num_stations; i++)
        send_status(channel, i, slot, CHANNEL_IDLE);
}

int channel_collect_requests(channel_t *channel)
{
    long long deadline = current_time_us() + REQUEST_WINDOW_US;

    while (current_time_us() < deadline) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(channel->fd, &set);

        long long remaining = deadline - current_time_us();
        if (remaining <= 0)
            break;

        struct timeval tv;
        tv.tv_sec = (time_t)(remaining / 1000000LL);
        tv.tv_usec = (suseconds_t)(remaining % 1000000LL);

        int rc = select(channel->fd + 1, &set, NULL, NULL, &tv);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (rc == 0)
            break;

        uint8_t buffer[sizeof(udp_tx_request_t)];
        struct sockaddr_in src;
        socklen_t src_len = sizeof(src);
        ssize_t n = recvfrom(channel->fd, buffer, sizeof(buffer), 0,
                             (struct sockaddr *)&src, &src_len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if ((size_t)n >= sizeof(uint32_t)) {
            uint32_t type;
            memcpy(&type, buffer, sizeof(type));

            if (type == UDP_MSG_METRIC && (size_t)n == sizeof(udp_metric_t)) {
                udp_metric_t *metric = (udp_metric_t *)buffer;
                if (metric->station_id >= 0 &&
                    metric->station_id < channel->num_stations) {
                    channel->stats.total_delay_slots += metric->delay_slots;
                    channel->stats.total_delay_events++;
                }
                continue;
            }
        }

        if (n < (ssize_t)offsetof(udp_tx_request_t, frame))
            continue;

        udp_tx_request_t *msg = (udp_tx_request_t *)buffer;
        if (msg->type != UDP_MSG_TX_REQUEST ||
            msg->station_id < 0 ||
            msg->station_id >= channel->num_stations ||
            msg->slot != channel->stats.slot ||
            msg->frame_length > UDP_MAX_FRAME_SIZE)
            continue;

        size_t expected = offsetof(udp_tx_request_t, frame) + msg->frame_length;
        if ((size_t)n < expected || channel->requests[msg->station_id].valid)
            continue;

        transmission_type_t *request = &channel->requests[msg->station_id];
        request->valid = 1;
        request->station_id = msg->station_id;
        request->slot = channel->stats.slot;
        request->frame.length = msg->frame_length;
        memcpy(request->frame.data, msg->frame, msg->frame_length);
    }

    return 0;
}

channel_state_t channel_evaluate(channel_t *channel, int *num_active)
{
    int count = 0;

    for (int i = 0; i < channel->num_stations; i++) {
        if (channel->requests[i].valid) {
            count++;
        }
    }

    if (num_active)
        *num_active = count;

    if (count == 0) {
        channel->stats.state = CHANNEL_IDLE;
        channel->stats.idle_slots++;
    } else {
        channel->stats.state = CHANNEL_ACTIVE;
        channel->stats.active_slots++;
        channel->stats.successful_transmissions += (unsigned long)count;
    }

    return channel->stats.state;
}

int channel_send_results(channel_t *channel, unsigned long slot)
{
    for (int i = 0; i < channel->num_stations; i++) {
        if (!channel->requests[i].valid)
            continue;

        /* In CDMA with orthogonal Walsh codes, all concurrent transmissions succeed */
        int collision = 0;
        int success = 1;
        int cd = 0;

        if (send_result(channel, i, slot, collision, success, cd) != 0)
            return -1;
    }

    return 0;
}

int channel_forward_frames(channel_t *channel, unsigned long slot)
{
    int active_count = 0;
    size_t max_len = 0;

    for (int i = 0; i < channel->num_stations; i++) {
        if (channel->requests[i].valid) {
            active_count++;
            if (channel->requests[i].frame.length > max_len)
                max_len = channel->requests[i].frame.length;
        }
    }

    if (active_count == 0 || max_len == 0)
        return 0;

    size_t total_chips = max_len * 8 * (size_t)channel->walsh_len;
    int16_t *composite_chips = calloc(total_chips, sizeof(int16_t));
    int8_t *station_chips = malloc(total_chips * sizeof(int8_t));

    if (!composite_chips || !station_chips) {
        free(composite_chips);
        free(station_chips);
        return -1;
    }

    /*
     * CDMA Physical Channel Simulation:
     * 1. Spread each transmitting station's frame using its unique Walsh code.
     * 2. Superimpose all signals into the shared composite channel signal.
     */
    for (int i = 0; i < channel->num_stations; i++) {
        if (!channel->requests[i].valid)
            continue;

        size_t len = channel->requests[i].frame.length;
        memset(station_chips, 0, total_chips * sizeof(int8_t));
        cdma_spread_data(channel->requests[i].frame.data, len,
                         channel->walsh_codes[i], channel->walsh_len,
                         station_chips);
        cdma_channel_combine(composite_chips, station_chips, total_chips);
    }
    free(station_chips);

    /*
     * CDMA Receiver Separation:
     * Despread the composite signal using each active station's Walsh code,
     * reconstruct the frame, and forward it to the destination's data UDP socket.
     */
    uint8_t *recovered_buf = malloc(max_len);
    if (!recovered_buf) {
        free(composite_chips);
        return -1;
    }

    for (int i = 0; i < channel->num_stations; i++) {
        if (!channel->requests[i].valid)
            continue;

        size_t len = channel->requests[i].frame.length;
        cdma_despread_data(composite_chips, len,
                           channel->walsh_codes[i], channel->walsh_len,
                           recovered_buf);

        frame_t decoded;
        memset(&decoded, 0, sizeof(decoded));
        if (DeserializeFrame(recovered_buf, len, &decoded) == 1) {
            int destination = decoded.header.dest_id;
            if (destination >= 0 && destination < channel->num_stations) {
                shared_frame_t f;
                f.length = len;
                memcpy(f.data, recovered_buf, len);
                send_frame(channel, destination, slot, &f);
            }
            free(decoded.payload.data);
        }
    }

    free(recovered_buf);
    free(composite_chips);
    return 0;
}

int channel_run_slot(channel_t *channel, unsigned long slot)
{
    long long slot_start = current_time_us();
    int num_active = 0;

    channel_start_slot(channel, slot);

    if (channel_collect_requests(channel) != 0)
        return -1;

    channel_evaluate(channel, &num_active);

    if (channel_send_results(channel, slot) != 0)
        return -1;

    if (num_active > 0) {
        if (channel_forward_frames(channel, slot) != 0)
            return -1;
    }

    long long elapsed = current_time_us() - slot_start;
    if (elapsed < SLOT_TIME_US)
        usleep((useconds_t)(SLOT_TIME_US - elapsed));

    return 0;
}

int channel_drain_metrics(channel_t *channel)
{
    for (;;) {
        uint8_t buffer[sizeof(udp_tx_request_t)];
        struct sockaddr_in src;
        socklen_t src_len = sizeof(src);

        ssize_t n = recvfrom(channel->fd, buffer, sizeof(buffer), MSG_DONTWAIT,
                             (struct sockaddr *)&src, &src_len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                return 0;
            return -1;
        }

        if ((size_t)n != sizeof(udp_metric_t))
            continue;

        udp_metric_t *metric = (udp_metric_t *)buffer;
        if (metric->type == UDP_MSG_METRIC &&
            metric->station_id >= 0 &&
            metric->station_id < channel->num_stations) {
            channel->stats.total_delay_slots += metric->delay_slots;
            channel->stats.total_delay_events++;
        }
    }
}

void channel_shutdown_stations(channel_t *channel)
{
    for (int i = 0; i < channel->num_stations; i++)
        send_shutdown(channel, i);
}

const channel_stats_t *channel_get_stats(const channel_t *channel)
{
    return &channel->stats;
}
