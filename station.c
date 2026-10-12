#define _POSIX_C_SOURCE 200809L
#include "station.h"
#include "framing.h"
#include "udp.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <poll.h>

static int recv_control(station_t *s, void *buf, size_t size)
{
    for (;;) {
        ssize_t n = recvfrom(s->control_fd, buf, size, 0, NULL, NULL);
        if (n >= 0)
            return (int)n;
        if (errno == EINTR)
            continue;
        return -1;
    }
}

static int send_hello(station_t *s)
{
    udp_hello_t msg = {0};
    msg.type = UDP_MSG_HELLO;
    msg.station_id = s->id;

    ssize_t n = sendto(s->control_fd, &msg, sizeof(msg), 0,
                       (struct sockaddr *)s->channel_addr_ptr,
                       sizeof(struct sockaddr_in));
    return n == (ssize_t)sizeof(msg) ? 0 : -1;
}

static int send_request(station_t *s, frame_t *frame, unsigned long slot)
{
    udp_tx_request_t msg;
    memset(&msg, 0, sizeof(msg));

    uint8_t *buffer = NULL;
    size_t length = SerializeFrame(frame, &buffer);
    if (length == 0 || length > UDP_MAX_FRAME_SIZE) {
        free(buffer);
        return -1;
    }

    msg.type = UDP_MSG_TX_REQUEST;
    msg.station_id = s->id;
    msg.slot = slot;
    msg.frame_length = (uint32_t)length;
    memcpy(msg.frame, buffer, length);
    free(buffer);

    size_t msg_size = offsetof(udp_tx_request_t, frame) + length;
    ssize_t n = sendto(s->control_fd, &msg, msg_size, 0,
                       (struct sockaddr *)s->channel_addr_ptr,
                       sizeof(struct sockaddr_in));
    return n == (ssize_t)msg_size ? 0 : -1;
}

static void *rx_thread(void *arg)
{
    station_t *s = (station_t *)arg;
    uint8_t buffer[sizeof(udp_frame_data_t)];

    while (s->running) {
        struct pollfd pfd = { .fd = s->data_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 100);
        if (pr == 0)
            continue;
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        ssize_t n = recvfrom(s->data_fd, buffer, sizeof(buffer), 0, NULL, NULL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (!s->running)
                break;
            continue;
        }

        if ((size_t)n < offsetof(udp_frame_data_t, frame))
            continue;

        udp_frame_data_t *msg = (udp_frame_data_t *)buffer;
        if (msg->type != UDP_MSG_FRAME_DATA)
            continue;

        size_t header_size = offsetof(udp_frame_data_t, frame);
        if (msg->frame_length > UDP_MAX_FRAME_SIZE ||
            header_size + msg->frame_length > (size_t)n)
            continue;

        frame_t frame;
        memset(&frame, 0, sizeof(frame));
        int result = DeserializeFrame(msg->frame, msg->frame_length, &frame);
        if (result == 1) {
            printf("Station %d received frame from station %d (seqNo=%u)\n",
                   s->id, frame.header.source_id, frame.header.seqNo);
            fflush(stdout);
            free(frame.payload.data);
        }
    }

    return NULL;
}

static void *tx_thread(void *arg)
{
    station_t *s = (station_t *)arg;
    unsigned long current_slot = 0;

    for (;;) {
        udp_slot_status_t status;
        int n = recv_control(s, &status, sizeof(status));
        if (n < 0)
            break;
        if ((size_t)n < sizeof(uint32_t))
            continue;

        uint32_t message_type;
        memcpy(&message_type, &status, sizeof(message_type));
        if (message_type == UDP_MSG_SHUTDOWN)
            break;

        if ((size_t)n != sizeof(status) || status.type != UDP_MSG_SLOT_STATUS)
            continue;

        current_slot = (unsigned long)status.slot;

        /*
         * In CDMA, stations share the channel using unique orthogonal Walsh codes.
         * Transmission decision is governed by traffic generation / probability p.
         */
        int transmit = cdma_should_transmit(s->p);

        if (!transmit)
            continue;

        if (!s->packet_pending) {
            s->pending_sequence_no = (uint8_t)s->sequence_no++;
            s->first_attempt_slot = current_slot;
            s->packet_pending = 1;
            s->retry_count = 0;
        }

        frame_t *frame = create_test_frame(
            s->id, s->destination, s->pending_sequence_no);
        if (!frame) {
            fprintf(stderr, "Station %d: frame creation failed\n", s->id);
            continue;
        }

        s->attempt_slot = current_slot;

        if (send_request(s, frame, current_slot) != 0) {
            free_frame(frame);
            continue;
        }
        free_frame(frame);

        udp_slot_result_t result;
        n = recv_control(s, &result, sizeof(result));
        if (n < 0)
            break;
        if ((size_t)n != sizeof(result) || result.type != UDP_MSG_SLOT_RESULT)
            continue;
        if ((unsigned long)result.slot != s->attempt_slot)
            continue;

        if (result.collision) {
            s->total_collisions++;
            s->retry_count++;
        } else if (result.success) {
            s->successful_frames++;
            unsigned long delay = current_slot - s->first_attempt_slot;
            udp_metric_t metric = {
                .type = UDP_MSG_METRIC,
                .station_id = s->id,
                .slot = current_slot,
                .delay_slots = delay
            };
            sendto(s->control_fd, &metric, sizeof(metric), 0,
                   (struct sockaddr *)s->channel_addr_ptr,
                   sizeof(struct sockaddr_in));
            s->packet_pending = 0;
            s->retry_count = 0;
        }
    }

    return NULL;
}

void station_run(station_t *s)
{
    pthread_t tx, rx;

    srand((unsigned int)(time(NULL) ^ getpid() ^ (s->id << 16)));
    s->running = 1;
    s->packet_pending = 0;
    s->pending_sequence_no = 0;
    s->retry_count = 0;
    s->total_collisions = 0;
    s->sequence_no = 0;
    s->attempt_slot = 0;
    s->first_attempt_slot = 0;
    s->successful_frames = 0;

    cdma_get_station_code(s->id, s->walsh_len, s->walsh_code);

    if (pthread_create(&rx, NULL, rx_thread, s) != 0) {
        perror("pthread_create(rx)");
        return;
    }
    if (pthread_create(&tx, NULL, tx_thread, s) != 0) {
        perror("pthread_create(tx)");
        s->running = 0;
        shutdown(s->data_fd, SHUT_RDWR);
        pthread_join(rx, NULL);
        return;
    }

    printf("Station %d started -> destination %d (Walsh code length: %d)\n",
           s->id, s->destination, s->walsh_len);
    fflush(stdout);

    send_hello(s);

    pthread_join(tx, NULL);
    s->running = 0;
    shutdown(s->data_fd, SHUT_RDWR);
    pthread_join(rx, NULL);

    close(s->control_fd);
    close(s->data_fd);

    printf("Station %d stopped: successful=%lu collisions=%lu\n",
           s->id, s->successful_frames, s->total_collisions);
    fflush(stdout);
}
