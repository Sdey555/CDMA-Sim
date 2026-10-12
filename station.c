#define _POSIX_C_SOURCE 200809L
#include "station.h"
#include "framing.h"
#include "udp.h"
#include "calc_crc.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <poll.h>

void station_print_bitwise_walsh(int station_id, const int *walsh_code, int walsh_len,
                                 const uint8_t *payload, size_t payload_len)
{
    printf("\n======================================================================\n");
    printf("BIT-WISE WALSH ENCODING (Station %d, Walsh Code Length M = %d)\n",
           station_id, walsh_len);
    printf("Station %d Walsh Code: [", station_id);
    for (int k = 0; k < walsh_len; k++) {
        printf("%s%+2d", (k > 0) ? " " : "", walsh_code[k]);
    }
    printf(" ]\n");
    printf("Payload: \"%.*s\" (%zu byte blocks)\n",
           (int)payload_len, (const char *)payload, payload_len);
    printf("======================================================================\n");

    for (size_t i = 0; i < payload_len; i++) {
        uint8_t byte = payload[i];
        char display_char = (byte >= 32 && byte <= 126) ? (char)byte : '.';
        printf("\n[Block %2zu] Character '%c' (Hex: 0x%02X, Binary: ", i, display_char, byte);
        for (int b = 7; b >= 0; b--) {
            printf("%d", (byte >> b) & 1);
        }
        printf(")\n");
        printf("  Bit Index | Data Bit | Bipolar (val) | Walsh Encoded Chips\n");
        printf("  ----------+----------+---------------+--------------------\n");

        for (int b = 7; b >= 0; b--) {
            int bit = (byte >> b) & 1;
            int val = (bit == 1) ? 1 : -1;
            printf("    Bit %d   |    %d     |      %+2d       | [", b, bit, val);
            for (int k = 0; k < walsh_len; k++) {
                int chip = val * walsh_code[k];
                printf("%s%+2d", (k > 0) ? " " : "", chip);
            }
            printf(" ]\n");
        }
    }
    printf("======================================================================\n\n");
    fflush(stdout);
}

static int recv_control(station_t *s, void *buf, size_t size)
{
    while (s->running) {
        struct pollfd pfd = { .fd = s->control_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 100);
        if (pr == 0)
            continue;
        if (pr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        ssize_t n = recvfrom(s->control_fd, buf, size, 0, NULL, NULL);
        if (n >= 0)
            return (int)n;
        if (errno == EINTR)
            continue;
        return -1;
    }
    return -1;
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
            uint16_t plen = ntohs(frame.header.payload_length);
            size_t crc_len = sizeof(header_t) + plen;
            uint8_t *crc_buf = malloc(crc_len);
            int crc_ok = 0;
            if (crc_buf) {
                memcpy(crc_buf, &frame.header, sizeof(header_t));
                memcpy(crc_buf + sizeof(header_t), frame.payload.data, plen);
                uint32_t computed_crc = crc32(crc_buf, crc_len);
                uint32_t received_crc = ntohl(frame.trailer.fcs);
                crc_ok = (computed_crc == received_crc);
                free(crc_buf);
            }

            const char *payload_text = (const char *)frame.payload.data;
            size_t text_len = strlen(payload_text);

            printf("\n\n======================================================================\n");
            printf(">>> PAYLOAD RECEIVED AT STATION %d <<<\n", s->id);
            printf("----------------------------------------------------------------------\n");
            printf("  From (Sender)       : Station %d\n", frame.header.source_id);
            printf("  To (Destination)    : Station %d\n", frame.header.dest_id);
            printf("  Sequence Number     : %u\n", frame.header.seqNo);
            printf("  Payload Message     : \"%s\"\n", payload_text);
            printf("  Message Length      : %zu bytes (Frame Payload: %u bytes)\n", text_len, plen);
            printf("  CRC-32 FCS          : 0x%08X [%s]\n",
                   ntohl(frame.trailer.fcs), crc_ok ? "VERIFIED OK" : "CHECKSUM FAILED");
            printf("======================================================================\n\n");
            if (s->cmd_pipe_fd >= 0) {
                printf("[Station %d -> Station %d] Enter payload (or '<dest> <msg>', 'dest <id>', 'exit') > ",
                       s->id, s->destination);
            }
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
    int got_first_status = 0;

    while (s->running) {
        udp_slot_status_t status;
        int n = recv_control(s, &status, sizeof(status));
        if (n <= 0) {
            if (!got_first_status && s->running) {
                /* Coordinator may still be starting up: resend hello */
                send_hello(s);
            }
            continue;
        }
        if ((size_t)n < sizeof(uint32_t))
            continue;

        uint32_t message_type;
        memcpy(&message_type, &status, sizeof(message_type));
        if (message_type == UDP_MSG_SHUTDOWN)
            break;

        if ((size_t)n != sizeof(status) || status.type != UDP_MSG_SLOT_STATUS)
            continue;

        got_first_status = 1;
        current_slot = (unsigned long)status.slot;

        if (s->cmd_pipe_fd >= 0) {
            /* Interactive mode: check if command was queued */
            if (!s->has_terminal_payload) {
                struct pollfd pfd = { .fd = s->cmd_pipe_fd, .events = POLLIN };
                int pr = poll(&pfd, 1, 0);
                if (pr > 0 && (pfd.revents & POLLIN)) {
                    station_cmd_t cmd;
                    ssize_t r = read(s->cmd_pipe_fd, &cmd, sizeof(cmd));
                    if (r == (ssize_t)sizeof(cmd)) {
                        s->destination = cmd.dest_id;
                        s->pending_payload_len = cmd.payload_length;
                        memcpy(s->pending_payload, cmd.payload, cmd.payload_length);
                        s->has_terminal_payload = 1;
                    }
                }
            }

            if (!s->has_terminal_payload) {
                /* No payload queued for this station in current slot */
                continue;
            }

            s->pending_sequence_no = (uint8_t)s->sequence_no++;
            s->first_attempt_slot = current_slot;
            s->attempt_slot = current_slot;

            frame_t *frame = create_payload_frame(
                s->id, s->destination, s->pending_sequence_no,
                (const uint8_t *)s->pending_payload, s->pending_payload_len);
            if (!frame) {
                fprintf(stderr, "Station %d: frame creation failed\n", s->id);
                s->has_terminal_payload = 0;
                continue;
            }

            /* Transmit immediately to stay well within the slot's 5ms request window */
            if (send_request(s, frame, current_slot) != 0) {
                free_frame(frame);
                continue;
            }
            free_frame(frame);
            s->has_terminal_payload = 0;

            udp_slot_result_t result;
            n = recv_control(s, &result, sizeof(result));
            if (n > 0 && (size_t)n == sizeof(result) && result.type == UDP_MSG_SLOT_RESULT) {
                if (result.collision) {
                    s->total_collisions++;
                    s->retry_count++;
                } else if (result.success) {
                    s->successful_frames++;
                    udp_metric_t metric = {
                        .type = UDP_MSG_METRIC,
                        .station_id = s->id,
                        .slot = current_slot,
                        .delay_slots = 0
                    };
                    sendto(s->control_fd, &metric, sizeof(metric), 0,
                           (struct sockaddr *)s->channel_addr_ptr,
                           sizeof(struct sockaddr_in));
                }
            }
            continue;
        }

        /*
         * Batch simulation mode (when cmd_pipe_fd < 0):
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

    signal(SIGINT, SIG_IGN);
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
    s->has_terminal_payload = 0;

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
    if (s->cmd_pipe_fd >= 0)
        close(s->cmd_pipe_fd);

    printf("Station %d stopped: successful=%lu collisions=%lu\n",
           s->id, s->successful_frames, s->total_collisions);
    fflush(stdout);
}

int station_run_standalone(int station_id, int num_stations, int destination)
{
    if (num_stations < 2 || num_stations > MAX_STATIONS) {
        fprintf(stderr, "Invalid number of stations (%d): must be 2..%d\n", num_stations, MAX_STATIONS);
        return 1;
    }
    if (station_id < 0 || station_id >= num_stations) {
        fprintf(stderr, "Invalid station ID (%d): must be 0..%d\n", station_id, num_stations - 1);
        return 1;
    }
    if (destination < 0 || destination >= num_stations || destination == station_id) {
        destination = (station_id + 1) % num_stations;
    }

    int control_fd = udp_make_socket();
    int data_fd = udp_make_socket();

    if (control_fd < 0 || data_fd < 0 ||
        udp_bind_port(control_fd, UDP_CONTROL_BASE + station_id) != 0 ||
        udp_bind_port(data_fd, UDP_DATA_BASE + station_id) != 0) {
        fprintf(stderr, "Station %d: Failed to bind UDP control/data ports (%d, %d).\n",
                station_id, UDP_CONTROL_BASE + station_id, UDP_DATA_BASE + station_id);
        if (control_fd >= 0) close(control_fd);
        if (data_fd >= 0) close(data_fd);
        return 1;
    }

    int cmd_pipe[2];
    if (pipe(cmd_pipe) != 0) {
        perror("pipe");
        close(control_fd);
        close(data_fd);
        return 1;
    }

    station_t s;
    memset(&s, 0, sizeof(s));
    s.id = station_id;
    s.destination = destination;
    s.p = 1.0;
    s.walsh_len = cdma_get_walsh_len(num_stations);
    s.control_fd = control_fd;
    s.data_fd = data_fd;
    s.cmd_pipe_fd = cmd_pipe[0];
    s.running = 1;

    struct sockaddr_in channel_addr;
    struct sockaddr_in destination_addr;
    udp_make_address(UDP_LOOPBACK, UDP_CHANNEL_PORT,
                     &channel_addr, sizeof(channel_addr));
    udp_make_address(UDP_LOOPBACK, UDP_DATA_BASE + destination,
                     &destination_addr, sizeof(destination_addr));
    s.channel_addr_ptr = &channel_addr;
    s.data_addr_ptr = &destination_addr;

    cdma_get_station_code(s.id, s.walsh_len, s.walsh_code);

    int show_walsh = 0;
    printf("Display bit-wise Walsh encoded code for each block? (1/y = Yes, 0/n = No) [default: 0]: ");
    fflush(stdout);
    char opt_buf[64];
    if (fgets(opt_buf, sizeof(opt_buf), stdin)) {
        if (opt_buf[0] == '1' || opt_buf[0] == 'y' || opt_buf[0] == 'Y')
            show_walsh = 1;
    }
    s.show_walsh_code = show_walsh;

    pthread_t rx, tx;
    if (pthread_create(&rx, NULL, rx_thread, &s) != 0) {
        perror("pthread_create(rx)");
        close(control_fd);
        close(data_fd);
        close(cmd_pipe[0]);
        close(cmd_pipe[1]);
        return 1;
    }
    if (pthread_create(&tx, NULL, tx_thread, &s) != 0) {
        perror("pthread_create(tx)");
        s.running = 0;
        shutdown(s.data_fd, SHUT_RDWR);
        pthread_join(rx, NULL);
        close(control_fd);
        close(data_fd);
        close(cmd_pipe[0]);
        close(cmd_pipe[1]);
        return 1;
    }

    send_hello(&s);

    printf("\n======================================================================\n");
    printf("                     STATION %d CONSOLE TERMINAL                      \n", station_id);
    printf("======================================================================\n");
    printf("Station ID          : %d (of %d total stations)\n", station_id, num_stations);
    printf("Default Destination : Station %d\n", destination);
    printf("Walsh Code Length   : M = %d\n", s.walsh_len);
    printf("Assigned Walsh Code : [");
    for (int k = 0; k < s.walsh_len; k++) {
        printf("%s%+2d", (k > 0) ? " " : "", s.walsh_code[k]);
    }
    printf(" ]\n");
    printf("UDP Control Port    : %d\n", UDP_CONTROL_BASE + station_id);
    printf("UDP Data Port       : %d\n", UDP_DATA_BASE + station_id);
    printf("Walsh Display       : %s\n", show_walsh ? "ENABLED" : "DISABLED");
    printf("----------------------------------------------------------------------\n");
    printf("Commands & Usage:\n");
    printf("  <message>           : Send payload to current Destination (Station %d)\n", destination);
    printf("  <dest_id> <message> : Send payload to Station <dest_id> (e.g., '1 Hello')\n");
    printf("  <dest_id>           : Choose Station <dest_id> and prompt for payload\n");
    printf("  dest <dest_id>      : Change default destination to Station <dest_id>\n");
    printf("  walsh / toggle      : Toggle bit-wise Walsh code display on/off\n");
    printf("  exit / quit / q     : Exit Station %d\n", station_id);
    printf("======================================================================\n\n");
    fflush(stdout);

    while (s.running) {
        printf("[Station %d -> Station %d] Enter payload (or '<dest> <msg>', 'dest <id>', 'exit') > ",
               station_id, s.destination);
        fflush(stdout);

        char line[2048];
        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n[Station %d: Terminal closed / EOF detected. Exiting...]\n", station_id);
            break;
        }

        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') continue;

        if (strcasecmp(line, "exit") == 0 ||
            strcasecmp(line, "quit") == 0 ||
            strcasecmp(line, "q") == 0 ||
            strcasecmp(line, "bye") == 0) {
            printf("[Station %d: Exit code received. Shutting down...]\n", station_id);
            break;
        }

        if (strcasecmp(line, "walsh") == 0 || strcasecmp(line, "toggle") == 0) {
            show_walsh = !show_walsh;
            s.show_walsh_code = show_walsh;
            printf("[Station %d] Bit-wise Walsh code display is now %s.\n\n",
                   station_id, show_walsh ? "ENABLED" : "DISABLED");
            continue;
        }

        if (strncasecmp(line, "dest ", 5) == 0 || strncasecmp(line, "to ", 3) == 0) {
            const char *p = (line[0] == 'd' || line[0] == 'D') ? (line + 5) : (line + 3);
            while (*p == ' ') p++;
            int new_dest = atoi(p);
            if (new_dest >= 0 && new_dest < num_stations && new_dest != station_id) {
                s.destination = new_dest;
                printf("[Station %d] Current destination changed to Station %d.\n\n", station_id, new_dest);
            } else {
                printf("[Station %d] Error: Destination must be in [0..%d] and != %d.\n\n",
                       station_id, num_stations - 1, station_id);
            }
            continue;
        }

        int target_dest = s.destination;
        const char *payload_text = line;
        int parsed_dest = -1;
        int chars_read = 0;
        if (sscanf(line, "%d %n", &parsed_dest, &chars_read) == 1 && chars_read > 0) {
            if (parsed_dest < 0 || parsed_dest >= num_stations || parsed_dest == station_id) {
                printf("[Station %d] Error: Destination %d is invalid (must be 0..%d and != %d).\n\n",
                       station_id, parsed_dest, num_stations - 1, station_id);
                continue;
            }
            target_dest = parsed_dest;
            payload_text = line + chars_read;
            while (*payload_text == ' ') payload_text++;
            if (*payload_text == '\0') {
                printf("[Station %d -> Station %d] Enter payload message: ", station_id, target_dest);
                fflush(stdout);
                char extra[2048];
                if (!fgets(extra, sizeof(extra), stdin)) break;
                extra[strcspn(extra, "\r\n")] = '\0';
                if (extra[0] == '\0') continue;
                payload_text = extra;
            }
        }

        /* If Walsh display is enabled, print bit-wise Walsh chips immediately */
        if (s.show_walsh_code) {
            station_print_bitwise_walsh(
                s.id, s.walsh_code, s.walsh_len,
                (const uint8_t *)payload_text, strlen(payload_text));
        }

        station_cmd_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.dest_id = target_dest;
        cmd.show_walsh_code = 0;
        cmd.payload_length = (uint32_t)strlen(payload_text);
        if (cmd.payload_length > MAX_PAYLOAD_LENGTH)
            cmd.payload_length = MAX_PAYLOAD_LENGTH;
        memcpy(cmd.payload, payload_text, cmd.payload_length);

        if (write(cmd_pipe[1], &cmd, sizeof(cmd)) != (ssize_t)sizeof(cmd)) {
            perror("write(cmd_pipe)");
            continue;
        }

        printf("[Station %d] Queued transmission to Station %d: \"%s\" (%u bytes)\n\n",
               station_id, target_dest, payload_text, cmd.payload_length);
        fflush(stdout);
        usleep(50000);
    }

    s.running = 0;
    close(cmd_pipe[1]);
    shutdown(control_fd, SHUT_RDWR);
    shutdown(data_fd, SHUT_RDWR);
    pthread_join(tx, NULL);
    pthread_join(rx, NULL);
    close(control_fd);
    close(data_fd);
    close(cmd_pipe[0]);

    printf("\nStation %d stopped: successful=%lu collisions=%lu\n",
           station_id, s.successful_frames, s.total_collisions);
    fflush(stdout);
    return 0;
}
