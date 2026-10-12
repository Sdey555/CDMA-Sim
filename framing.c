#include "framing.h"
#include "calc_crc.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------
 * SerializeFrame
 * --------------------------------------------------------- */

size_t SerializeFrame(frame_t *frame, uint8_t **buffer)
{
    if (frame == NULL || buffer == NULL)
        return 0;

    uint16_t payload_length = ntohs(frame->header.payload_length);

    if (payload_length < MIN_PAYLOAD_LENGTH || payload_length > MAX_PAYLOAD_LENGTH)
        return 0;

    size_t frame_size = sizeof(header_t) + payload_length + sizeof(trailer_t);

    *buffer = malloc(frame_size);
    if (*buffer == NULL) {
        perror("malloc");
        return 0;
    }

    size_t offset = 0;

    /* Header */
    memcpy(*buffer + offset, &frame->header, sizeof(header_t));
    offset += sizeof(header_t);

    /* Payload */
    memcpy(*buffer + offset, frame->payload.data, payload_length);
    offset += payload_length;

    /* Trailer */
    memcpy(*buffer + offset, &frame->trailer, sizeof(trailer_t));

    return frame_size;
}

/* ---------------------------------------------------------
 * DeserializeFrame
 * --------------------------------------------------------- */

int DeserializeFrame(uint8_t *buffer, size_t buffer_size, frame_t *frame)
{
    if (buffer == NULL || frame == NULL)
        return 0;

    if (buffer_size < sizeof(header_t) + sizeof(trailer_t))
        return 0;

    memset(frame, 0, sizeof(frame_t));

    size_t offset = 0;

    /* Header */
    memcpy(&frame->header, buffer + offset, sizeof(header_t));
    offset += sizeof(header_t);

    uint16_t payload_length = ntohs(frame->header.payload_length);

    /* Special end frame. */
    if (payload_length == 0)
        return 2;

    if (payload_length < MIN_PAYLOAD_LENGTH || payload_length > MAX_PAYLOAD_LENGTH)
        return 0;

    size_t expected_size = sizeof(header_t) + payload_length + sizeof(trailer_t);
    if (buffer_size != expected_size)
        return 0;

    /* Allocate payload. */
    frame->payload.data = malloc(payload_length);
    if (frame->payload.data == NULL) {
        perror("malloc");
        return 0;
    }

    /* Payload */
    memcpy(frame->payload.data, buffer + offset, payload_length);
    offset += payload_length;

    /* Trailer */
    memcpy(&frame->trailer, buffer + offset, sizeof(trailer_t));

    return 1;
}

/* ---------------------------------------------------------
 * Framing
 * --------------------------------------------------------- */

int Framing(frame_t *frame, int source, int dest, uint8_t sequence_no, FILE *input)
{
    if (frame == NULL || input == NULL)
        return 0;

    memset(frame, 0, sizeof(frame_t));

    frame->header.source_id = (uint8_t)source;
    frame->header.dest_id = (uint8_t)dest;

    frame->payload.data = calloc(MAX_PAYLOAD_LENGTH, 1);
    if (frame->payload.data == NULL) {
        perror("calloc");
        return 0;
    }

    size_t bytes = fread(frame->payload.data, 1, MAX_PAYLOAD_LENGTH, input);
    if (bytes == 0) {
        free(frame->payload.data);
        frame->payload.data = NULL;
        return 0;
    }

    /* Ethernet-style minimum payload. */
    size_t payload_length = (bytes < MIN_PAYLOAD_LENGTH) ? MIN_PAYLOAD_LENGTH : bytes;

    frame->header.payload_length = htons((uint16_t)payload_length);
    frame->header.seqNo = sequence_no;

    /* Calculate CRC over header + payload. */
    size_t crc_length = sizeof(header_t) + payload_length;

    uint8_t *crc_buffer = malloc(crc_length);
    if (crc_buffer == NULL) {
        perror("malloc");
        free(frame->payload.data);
        frame->payload.data = NULL;
        return 0;
    }

    memcpy(crc_buffer, &frame->header, sizeof(header_t));
    memcpy(crc_buffer + sizeof(header_t), frame->payload.data, payload_length);

    uint32_t crc = crc32(crc_buffer, crc_length);
    free(crc_buffer);

    frame->trailer.fcs = htonl(crc);

    return 1;
}

/* ---------------------------------------------------------
 * create_test_frame
 * --------------------------------------------------------- */

frame_t *create_test_frame(int source, int destination, uint8_t sequence_no)
{
    frame_t *frame = calloc(1, sizeof(frame_t));
    if (frame == NULL) {
        perror("calloc");
        return NULL;
    }

    frame->header.source_id = (uint8_t)source;
    frame->header.dest_id = (uint8_t)destination;
    frame->header.seqNo = sequence_no;

    /*
     * Saturated traffic:
     * every station always has a frame to send.
     */
    frame->header.payload_length = htons(MAX_PAYLOAD_LENGTH);

    frame->payload.data = malloc(MAX_PAYLOAD_LENGTH);
    if (frame->payload.data == NULL) {
        perror("malloc");
        free(frame);
        return NULL;
    }

    /*
     * Fill payload with deterministic data.
     */
    for (size_t i = 0; i < MAX_PAYLOAD_LENGTH; i++) {
        frame->payload.data[i] = (uint8_t)((source + i) & 0xFF);
    }

    /* Calculate CRC. */
    size_t crc_length = sizeof(header_t) + MAX_PAYLOAD_LENGTH;

    uint8_t *crc_buffer = malloc(crc_length);
    if (crc_buffer == NULL) {
        perror("malloc");
        free(frame->payload.data);
        free(frame);
        return NULL;
    }

    memcpy(crc_buffer, &frame->header, sizeof(header_t));
    memcpy(crc_buffer + sizeof(header_t), frame->payload.data, MAX_PAYLOAD_LENGTH);

    uint32_t crc = crc32(crc_buffer, crc_length);
    free(crc_buffer);

    frame->trailer.fcs = htonl(crc);

    return frame;
}

/* ---------------------------------------------------------
 * free_frame
 * --------------------------------------------------------- */

void free_frame(frame_t *frame)
{
    if (frame == NULL)
        return;

    free(frame->payload.data);
    frame->payload.data = NULL;
    free(frame);
}
