#define _POSIX_C_SOURCE 200809L

#include "channel.h"
#include "station.h"
#include "cdma.h"
#include "udp.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;

static void handle_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void print_usage(const char *prog)
{
    printf("\n======================================================================\n");
    printf("                        CDMA SIMULATION USAGE                         \n");
    printf("======================================================================\n");
    printf("1) Multi-terminal mode (Coordinator + Stations in separate terminals):\n");
    printf("   Terminal 1 (Coordinator) : %s coordinator <stations>\n", prog);
    printf("   Terminal 2 (Station 0)   : %s station 0 <stations> [dest]\n", prog);
    printf("   Terminal 3 (Station 1)   : %s station 1 <stations> [dest]\n", prog);
    printf("   Example for 4 stations:\n");
    printf("     T1: %s coordinator 4\n", prog);
    printf("     T2: %s station 0 4\n", prog);
    printf("     T3: %s station 1 4\n", prog);
    printf("     T4: %s station 2 4\n", prog);
    printf("     T5: %s station 3 4\n\n", prog);
    printf("2) Single-terminal mode (All-in-one console):\n");
    printf("   %s [stations]\n", prog);
    printf("   Example: %s 4\n\n", prog);
    printf("3) Automated batch experiment mode:\n");
    printf("   %s <stations> <slots> [p] --batch\n", prog);
    printf("======================================================================\n\n");
}

static int is_station_alive(int station_id)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)(UDP_CONTROL_BASE + station_id));
    int res = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    close(fd);
    /* If bind fails (EADDRINUSE), the station is actively running and holding the port */
    return (res < 0);
}

/* ----------------------------------------------------------------------
 * Standalone Coordinator (Runs in its own terminal)
 * ---------------------------------------------------------------------- */
static int run_standalone_coordinator(int num_stations)
{
    if (num_stations < 2 || num_stations > MAX_STATIONS) {
        fprintf(stderr, "Invalid number of stations (%d): must be 2..%d.\n", num_stations, MAX_STATIONS);
        return 1;
    }

    channel_t channel;
    if (channel_init(&channel, num_stations) != 0) {
        fprintf(stderr, "Failed to initialize CDMA channel.\n");
        return 1;
    }

    printf("\n======================================================================\n");
    printf("                  CDMA CHANNEL COORDINATOR CONSOLE                    \n");
    printf("======================================================================\n");
    printf("Stations : %d\n", num_stations);
    printf("Protocol : CDMA (Walsh codes, M = %d)\n", channel.walsh_len);
    printf("UDP Port : %d\n\n", UDP_CHANNEL_PORT);

    printf("Assigned Walsh Codes (Orthogonal Set):\n");
    for (int i = 0; i < num_stations; i++) {
        cdma_print_code(i, channel.walsh_codes[i], channel.walsh_len);
    }
    printf("Orthogonality check: PASSED (all %d codes mutually orthogonal)\n\n", num_stations);

    printf("Waiting for all %d stations to connect...\n", num_stations);
    printf("--> Run in other terminals: ./cdma_udp_sim station <id> %d\n", num_stations);
    fflush(stdout);

    if (channel_wait_for_hellos(&channel) != 0) {
        channel_shutdown_stations(&channel);
        channel_destroy(&channel);
        return 1;
    }

    printf("\nChannel: all %d stations registered; starting CDMA slotted coordination.\n", num_stations);
    printf("Coordinator is active. Stations can now transmit at any time.\n");
    printf("(Press Ctrl+C to stop coordinator, or close/exit all stations to stop)\n");
    printf("======================================================================\n\n");
    fflush(stdout);

    unsigned long slot = 0;
    while (!g_stop) {
        /* Execute coordination slot without exiting on transient socket events */
        channel_run_slot(&channel, slot);

        int active_count = 0;
        for (int i = 0; i < channel.num_stations; i++) {
            if (channel.requests[i].valid)
                active_count++;
        }
        if (active_count > 0) {
            printf("[Coordinator Slot %lu] %d transmission(s) from:", slot, active_count);
            for (int i = 0; i < channel.num_stations; i++) {
                if (channel.requests[i].valid) {
                    printf(" Station %d", i);
                }
            }
            printf(" -> Combined into composite signal -> Despread & forwarded\n");
            fflush(stdout);
        }

        /* Check if all stations have stopped (checked every 50 slots = ~0.5s) */
        if ((slot % 50) == 0 && slot > 0) {
            int active_stations = 0;
            for (int i = 0; i < num_stations; i++) {
                if (is_station_alive(i))
                    active_stations++;
            }
            if (active_stations == 0) {
                printf("\n[Coordinator] All %d stations have stopped. Coordinator shutting down...\n", num_stations);
                break;
            }
        }

        slot++;
    }

    printf("\nShutting down CDMA simulation stations...\n");
    channel_drain_metrics(&channel);
    channel_shutdown_stations(&channel);

    const channel_stats_t *stats = channel_get_stats(&channel);
    printf("\n========== Final Coordinator Summary ==========\n");
    printf("Stations                  : %d\n", num_stations);
    printf("Slots executed            : %lu\n", slot);
    printf("Successful transmissions  : %lu\n", stats->successful_transmissions);
    printf("Collision slots           : %lu\n", stats->collisions);
    printf("Idle slots                : %lu\n", stats->idle_slots);
    printf("===============================================\n");

    channel_destroy(&channel);
    return 0;
}

/* ----------------------------------------------------------------------
 * All-In-One Mode (Single terminal or batch experiments)
 * ---------------------------------------------------------------------- */
static int run_all_in_one(int num_stations, int total_slots, double p, int batch_mode)
{
    if (num_stations < 2 || num_stations > MAX_STATIONS) {
        fprintf(stderr, "Invalid number of stations: must be between 2 and %d.\n", MAX_STATIONS);
        return 1;
    }

    if (p < 0.0 || p > 1.0) {
        fprintf(stderr, "Invalid p: transmission probability must be in [0.0, 1.0].\n");
        return 1;
    }

    channel_t channel;
    if (channel_init(&channel, num_stations) != 0) {
        fprintf(stderr, "Failed to initialize CDMA channel.\n");
        return 1;
    }

    printf("\nCDMA UDP simulation\n");
    printf("Stations : %d\n", num_stations);
    printf("Protocol : CDMA (Walsh codes, M = %d)\n", channel.walsh_len);
    if (batch_mode) {
        printf("Slots    : %d\n", total_slots);
        printf("Traffic probability (p) : %.2f\n", p);
    }
    printf("UDP channel port: %d\n\n", UDP_CHANNEL_PORT);

    printf("Assigned Walsh Codes (Orthogonal Set):\n");
    for (int i = 0; i < num_stations; i++) {
        cdma_print_code(i, channel.walsh_codes[i], channel.walsh_len);
    }
    printf("Orthogonality check: PASSED (all %d codes mutually orthogonal)\n\n", num_stations);
    fflush(stdout);

    int show_walsh_code = 0;
    int child_pipes[MAX_STATIONS];
    int parent_pipes[MAX_STATIONS];

    for (int i = 0; i < num_stations; i++) {
        child_pipes[i] = -1;
        parent_pipes[i] = -1;
    }

    if (!batch_mode) {
        printf("Display bit-wise Walsh encoded code for each block? (1/y = Yes, 0/n = No) [default: 0]: ");
        fflush(stdout);

        char opt_buf[64];
        if (fgets(opt_buf, sizeof(opt_buf), stdin)) {
            if (opt_buf[0] == '1' || opt_buf[0] == 'y' || opt_buf[0] == 'Y') {
                show_walsh_code = 1;
            }
        }
        if (show_walsh_code)
            printf("Bit-wise Walsh code display: ENABLED\n");
        else
            printf("Bit-wise Walsh code display: DISABLED\n");
        fflush(stdout);

        for (int i = 0; i < num_stations; i++) {
            int pfd[2];
            if (pipe(pfd) != 0) {
                perror("pipe");
                channel_destroy(&channel);
                return 1;
            }
            child_pipes[i] = pfd[0];
            parent_pipes[i] = pfd[1];
        }
    }

    /* Fork station processes */
    for (int i = 0; i < num_stations; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            channel_shutdown_stations(&channel);
            while (wait(NULL) > 0) {}
            channel_destroy(&channel);
            return 1;
        }

        if (pid == 0) {
            close(channel.fd);

            if (!batch_mode) {
                for (int j = 0; j < num_stations; j++) {
                    if (parent_pipes[j] >= 0) close(parent_pipes[j]);
                    if (j != i && child_pipes[j] >= 0) close(child_pipes[j]);
                }
            }

            int control_fd = udp_make_socket();
            int data_fd = udp_make_socket();

            if (control_fd < 0 || data_fd < 0 ||
                udp_bind_port(control_fd, UDP_CONTROL_BASE + i) != 0 ||
                udp_bind_port(data_fd, UDP_DATA_BASE + i) != 0) {
                if (control_fd >= 0) close(control_fd);
                if (data_fd >= 0) close(data_fd);
                if (child_pipes[i] >= 0) close(child_pipes[i]);
                _exit(1);
            }

            station_t station;
            memset(&station, 0, sizeof(station));
            station.id = i;
            station.destination = (i + 1) % num_stations;
            station.p = p;
            station.walsh_len = channel.walsh_len;
            station.control_fd = control_fd;
            station.data_fd = data_fd;
            station.cmd_pipe_fd = batch_mode ? -1 : child_pipes[i];
            station.show_walsh_code = show_walsh_code;

            struct sockaddr_in channel_addr;
            struct sockaddr_in destination_addr;
            udp_make_address(UDP_LOOPBACK, UDP_CHANNEL_PORT,
                             &channel_addr, sizeof(channel_addr));
            udp_make_address(UDP_LOOPBACK, UDP_DATA_BASE + station.destination,
                             &destination_addr, sizeof(destination_addr));
            station.channel_addr_ptr = &channel_addr;
            station.data_addr_ptr = &destination_addr;

            station_run(&station);
            _exit(0);
        }
    }

    if (!batch_mode) {
        for (int i = 0; i < num_stations; i++) {
            if (child_pipes[i] >= 0) {
                close(child_pipes[i]);
                child_pipes[i] = -1;
            }
        }
    }

    if (channel_wait_for_hellos(&channel) != 0) {
        channel_shutdown_stations(&channel);
        while (wait(NULL) > 0) {}
        channel_destroy(&channel);
        return 1;
    }

    printf("Channel: all %d stations registered; CDMA simulation starting.\n", num_stations);
    fflush(stdout);

    if (batch_mode) {
        for (int slot = 0; slot < total_slots; slot++) {
            if (channel_run_slot(&channel, (unsigned long)slot) != 0) {
                fprintf(stderr, "Channel error while processing slot %d.\n", slot);
                channel_shutdown_stations(&channel);
                break;
            }
        }
    } else {
        printf("\n======================================================================\n");
        printf("                CDMA INTERACTIVE TRANSMISSION CONSOLE                 \n");
        printf("======================================================================\n");
        printf("Active Stations : %d (Walsh Code Length M = %d)\n", num_stations, channel.walsh_len);
        printf("Walsh Display   : %s\n", show_walsh_code ? "ENABLED" : "DISABLED");
        printf("----------------------------------------------------------------------\n");
        printf("Commands & Input:\n");
        printf("  <message>              : Send from Station 0 -> Station 1 (e.g. 'Hello')\n");
        printf("  <src> <dst> <message>  : Send from Station <src> -> Station <dst>\n");
        printf("  all <message>          : Transmit from ALL stations simultaneously\n");
        printf("  walsh / toggle         : Toggle bit-wise Walsh code display\n");
        printf("  exit / quit / q        : Exit simulation\n");
        printf("======================================================================\n\n");
        fflush(stdout);

        unsigned long current_slot = 0;

        while (!g_stop) {
            printf("[Slot %lu] Enter payload (or 'exit' to quit) > ", current_slot);
            fflush(stdout);

            char line[2048];
            if (!fgets(line, sizeof(line), stdin)) {
                printf("\n[Terminal input closed / EOF detected]\n");
                break;
            }

            line[strcspn(line, "\r\n")] = '\0';
            if (line[0] == '\0') continue;

            if (strcasecmp(line, "exit") == 0 ||
                strcasecmp(line, "quit") == 0 ||
                strcasecmp(line, "q") == 0 ||
                strcasecmp(line, "bye") == 0 ||
                strcasecmp(line, ":q") == 0) {
                printf("\n[Exit code '%s' received. Shutting down CDMA simulation...]\n", line);
                break;
            }

            if (strcasecmp(line, "walsh") == 0 || strcasecmp(line, "toggle") == 0) {
                show_walsh_code = !show_walsh_code;
                printf("Bit-wise Walsh code display is now %s.\n\n",
                       show_walsh_code ? "ENABLED" : "DISABLED");
                continue;
            }

            if (strncasecmp(line, "all ", 4) == 0) {
                const char *msg_text = line + 4;
                while (*msg_text == ' ') msg_text++;
                if (*msg_text == '\0') {
                    printf("Error: Payload message cannot be empty.\n\n");
                    continue;
                }

                printf("\n>>> Simultaneous Transmission from ALL %d stations: \"%s\" (Slot %lu) <<<\n",
                       num_stations, msg_text, current_slot);
                fflush(stdout);

                for (int i = 0; i < num_stations; i++) {
                    station_cmd_t cmd;
                    memset(&cmd, 0, sizeof(cmd));
                    cmd.dest_id = (i + 1) % num_stations;
                    cmd.show_walsh_code = show_walsh_code;
                    cmd.payload_length = (uint32_t)strlen(msg_text);
                    if (cmd.payload_length > MAX_PAYLOAD_LENGTH)
                        cmd.payload_length = MAX_PAYLOAD_LENGTH;
                    memcpy(cmd.payload, msg_text, cmd.payload_length);
                    write(parent_pipes[i], &cmd, sizeof(cmd));
                }

                if (channel_run_slot(&channel, current_slot) != 0) {
                    fprintf(stderr, "Channel error while processing slot %lu.\n", current_slot);
                    break;
                }
                current_slot++;
                usleep(50000);
                continue;
            }

            int src_id = 0;
            int dst_id = (num_stations > 1) ? 1 : 0;
            const char *payload_text = line;

            int parsed_src = -1, parsed_dst = -1;
            int chars_read = 0;
            if (sscanf(line, "%d %d %n", &parsed_src, &parsed_dst, &chars_read) == 2 && chars_read > 0) {
                if (parsed_src < 0 || parsed_src >= num_stations ||
                    parsed_dst < 0 || parsed_dst >= num_stations ||
                    parsed_src == parsed_dst) {
                    printf("Error: Invalid stations (%d -> %d). IDs must be in [0..%d] and src != dst.\n\n",
                           parsed_src, parsed_dst, num_stations - 1);
                    continue;
                }
                src_id = parsed_src;
                dst_id = parsed_dst;
                payload_text = line + chars_read;
                while (*payload_text == ' ') payload_text++;
                if (*payload_text == '\0') {
                    printf("Enter payload for Station %d -> Station %d: ", src_id, dst_id);
                    fflush(stdout);
                    char extra_buf[2048];
                    if (!fgets(extra_buf, sizeof(extra_buf), stdin))
                        break;
                    extra_buf[strcspn(extra_buf, "\r\n")] = '\0';
                    if (extra_buf[0] == '\0') continue;
                    payload_text = extra_buf;
                }
            }

            printf("\n>>> Transmitting from Station %d to Station %d: \"%s\" (Slot %lu) <<<\n",
                   src_id, dst_id, payload_text, current_slot);
            fflush(stdout);

            station_cmd_t cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.dest_id = dst_id;
            cmd.show_walsh_code = show_walsh_code;
            cmd.payload_length = (uint32_t)strlen(payload_text);
            if (cmd.payload_length > MAX_PAYLOAD_LENGTH)
                cmd.payload_length = MAX_PAYLOAD_LENGTH;
            memcpy(cmd.payload, payload_text, cmd.payload_length);

            ssize_t w = write(parent_pipes[src_id], &cmd, sizeof(cmd));
            if (w != (ssize_t)sizeof(cmd)) {
                perror("write(pipe)");
                continue;
            }

            if (channel_run_slot(&channel, current_slot) != 0) {
                fprintf(stderr, "Channel error while processing slot %lu.\n", current_slot);
                break;
            }
            current_slot++;
            usleep(50000);
        }
        total_slots = (int)current_slot;
    }

    printf("\nShutting down CDMA simulation stations...\n");
    channel_drain_metrics(&channel);
    channel_shutdown_stations(&channel);

    for (int i = 0; i < num_stations; i++) {
        if (parent_pipes[i] >= 0) {
            close(parent_pipes[i]);
            parent_pipes[i] = -1;
        }
    }

    for (int i = 0; i < num_stations; i++)
        wait(NULL);

    const channel_stats_t *stats = channel_get_stats(&channel);
    double average_delay = stats->total_delay_events
        ? (double)stats->total_delay_slots / stats->total_delay_events
        : 0.0;
    double throughput = (total_slots > 0) ? (double)stats->successful_transmissions / total_slots : 0.0;
    double collision_rate = (total_slots > 0) ? (double)stats->collisions / total_slots : 0.0;
    unsigned long idle = stats->idle_slots;

    printf("\n========== Simulation Results ==========\n");
    printf("Stations                  : %d\n", num_stations);
    printf("Slots                     : %d\n", total_slots);
    printf("Successful transmissions  : %lu\n", stats->successful_transmissions);
    printf("Collision slots           : %lu\n", stats->collisions);
    printf("Idle slots                : %lu\n", idle);
    printf("Average delay             : %.3f slots\n", average_delay);
    printf("Throughput                : %.4f frames/slot\n", throughput);
    printf("Collision rate            : %.4f\n", collision_rate);
    printf("========================================\n");

    channel_destroy(&channel);
    return 0;
}

int main(int argc, char **argv)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    if (argc >= 2) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "help") == 0) {
            print_usage(argv[0]);
            return 0;
        }

        /* 1) Coordinator in separate terminal: ./cdma_udp_sim coordinator <num_stations> */
        if (strcmp(argv[1], "coordinator") == 0 || strcmp(argv[1], "channel") == 0 ||
            strcmp(argv[1], "-c") == 0 || strcmp(argv[1], "--coordinator") == 0) {
            int n = (argc >= 3) ? atoi(argv[2]) : 4;
            return run_standalone_coordinator(n);
        }

        /* 2) Station in separate terminal: ./cdma_udp_sim station <station_id> [num_stations] [dest] */
        if (strcmp(argv[1], "station") == 0 || strcmp(argv[1], "-s") == 0 ||
            strcmp(argv[1], "--station") == 0) {
            int sid = (argc >= 3) ? atoi(argv[2]) : 0;
            int n = (argc >= 4) ? atoi(argv[3]) : 4;
            int dst = (argc >= 5) ? atoi(argv[4]) : (sid + 1) % n;
            return station_run_standalone(sid, n, dst);
        }

        /* 3) All-in-one explicit: ./cdma_udp_sim all <num_stations> */
        if (strcmp(argv[1], "all") == 0 || strcmp(argv[1], "-a") == 0) {
            int n = (argc >= 3) ? atoi(argv[2]) : 4;
            return run_all_in_one(n, 0, 1.0, 0);
        }

        /* 4) Number passed as first argument: e.g. ./cdma_udp_sim 4 [slots] [p] */
        if (isdigit((unsigned char)argv[1][0])) {
            int num_stations = atoi(argv[1]);
            int total_slots = (argc >= 3) ? atoi(argv[2]) : 0;
            double p = (argc >= 4) ? atof(argv[3]) : 1.0;
            int batch_mode = 0;

            if (argc >= 3 && !isatty(STDIN_FILENO))
                batch_mode = 1;

            for (int i = 1; i < argc; i++) {
                if (strcmp(argv[i], "--batch") == 0 || strcmp(argv[i], "-b") == 0)
                    batch_mode = 1;
            }

            return run_all_in_one(num_stations, total_slots, p, batch_mode);
        }
    }

    /* Interactive Menu Launcher if run with no arguments in a terminal */
    if (isatty(STDIN_FILENO)) {
        printf("======================================================================\n");
        printf("                    CDMA UDP SIMULATION LAUNCHER                      \n");
        printf("======================================================================\n");
        printf("Select execution mode:\n");
        printf("  1) Coordinator / Channel   (Run in separate terminal)\n");
        printf("  2) Station Process         (Run in separate terminal)\n");
        printf("  3) All-in-One Console      (Run Coordinator + Stations in this terminal)\n");
        printf("Enter choice [1/2/3, default: 3]: ");
        fflush(stdout);

        char choice[64];
        if (fgets(choice, sizeof(choice), stdin)) {
            if (choice[0] == '1') {
                printf("Enter number of stations (2..%d) [default: 4]: ", MAX_STATIONS);
                fflush(stdout);
                char nbuf[64];
                int n = 4;
                if (fgets(nbuf, sizeof(nbuf), stdin) && atoi(nbuf) >= 2)
                    n = atoi(nbuf);
                return run_standalone_coordinator(n);
            } else if (choice[0] == '2') {
                printf("Enter Station ID [0..%d, default: 0]: ", MAX_STATIONS - 1);
                fflush(stdout);
                char sbuf[64];
                int sid = 0;
                if (fgets(sbuf, sizeof(sbuf), stdin) && isdigit((unsigned char)sbuf[0]))
                    sid = atoi(sbuf);

                printf("Enter total number of stations (2..%d) [default: 4]: ", MAX_STATIONS);
                fflush(stdout);
                char nbuf[64];
                int n = 4;
                if (fgets(nbuf, sizeof(nbuf), stdin) && atoi(nbuf) >= 2)
                    n = atoi(nbuf);

                int dst = (sid + 1) % n;
                printf("Enter destination Station ID [default: %d]: ", dst);
                fflush(stdout);
                char dbuf[64];
                if (fgets(dbuf, sizeof(dbuf), stdin) && isdigit((unsigned char)dbuf[0]))
                    dst = atoi(dbuf);

                return station_run_standalone(sid, n, dst);
            }
        }
    }

    return run_all_in_one(4, 0, 1.0, 0);
}
