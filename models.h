#ifndef MODELS_H
#define MODELS_H

#include <stdint.h>
#include <stddef.h>

#define MIN_PAYLOAD_LENGTH 46
#define MAX_PAYLOAD_LENGTH 1500

typedef struct header {
    uint8_t source_id;
    uint8_t dest_id;
    uint16_t payload_length;
    uint8_t seqNo;
} header_t;

typedef struct payload {
    uint8_t *data;
} payload_t;

typedef struct trailer {
    uint32_t fcs;
} trailer_t;

typedef struct frame {
    header_t header;
    payload_t payload;
    trailer_t trailer;
} frame_t;

/*
 * A frame representation that can safely be placed
 * inside shared memory or serialized UDP buffers.
 */
#define MAX_FRAME_SIZE (sizeof(header_t) + MAX_PAYLOAD_LENGTH + sizeof(trailer_t))

typedef struct {
    size_t length;
    uint8_t data[MAX_FRAME_SIZE];
} shared_frame_t;

#endif
