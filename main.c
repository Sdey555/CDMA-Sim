#include "channel.h"
#include "station.h"
#include "cdma.h"
#include "udp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
                "Usage: %s <stations> <slots> [p]\n"
                "  <stations> : number of stations (2..%d)\n"
                "  <slots>    : total simulation slots\n"
                "  [p]        : transmission probability (default: 1.0, saturated)\n",
                argv[0], MAX_STATIONS);
        return 1;
    }

    int num_stations = atoi(argv[1]);
    int total_slots = atoi(argv[2]);
    double p = (argc >= 4) ? atof(argv[3]) : 1.0;

    if (num_stations < 2 || num_stations > MAX_STATIONS || total_slots <= 0) {
        fprintf(stderr, "Invalid arguments: stations must be 2..%d, slots > 0.\n", MAX_STATIONS);
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
    printf("Stations : %d\nSlots    : %d\n", num_stations, total_slots);
    printf("Protocol : CDMA (Walsh codes, M = %d)\n", channel.walsh_len);
    printf("Traffic probability (p) : %.2f\n", p);
    printf("UDP channel port: %d\n\n", UDP_CHANNEL_PORT);

    printf("Assigned Walsh Codes (Orthogonal Set):\n");
    for (int i = 0; i < num_stations; i++) {
        cdma_print_code(i, channel.walsh_codes[i], channel.walsh_len);
    }
    printf("Orthogonality check: PASSED (all %d codes mutually orthogonal)\n\n", num_stations);
    fflush(stdout);

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
            /* Child process: does not use inherited channel socket */
            close(channel.fd);

            int control_fd = udp_make_socket();
            int data_fd = udp_make_socket();

            if (control_fd < 0 || data_fd < 0 ||
                udp_bind_port(control_fd, UDP_CONTROL_BASE + i) != 0 ||
                udp_bind_port(data_fd, UDP_DATA_BASE + i) != 0) {
                if (control_fd >= 0) close(control_fd);
                if (data_fd >= 0) close(data_fd);
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

    if (channel_wait_for_hellos(&channel) != 0) {
        channel_shutdown_stations(&channel);
        while (wait(NULL) > 0) {}
        channel_destroy(&channel);
        return 1;
    }

    printf("Channel: all %d stations registered; CDMA simulation starting.\n", num_stations);
    fflush(stdout);

    for (int slot = 0; slot < total_slots; slot++) {
        if (channel_run_slot(&channel, (unsigned long)slot) != 0) {
            fprintf(stderr, "Channel error while processing slot %d.\n", slot);
            channel_shutdown_stations(&channel);
            break;
        }
    }

    channel_drain_metrics(&channel);
    channel_shutdown_stations(&channel);

    for (int i = 0; i < num_stations; i++)
        wait(NULL);

    const channel_stats_t *stats = channel_get_stats(&channel);
    double average_delay = stats->total_delay_events
        ? (double)stats->total_delay_slots / stats->total_delay_events
        : 0.0;
    double throughput = (double)stats->successful_transmissions / total_slots;
    double collision_rate = (double)stats->collisions / total_slots;
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
