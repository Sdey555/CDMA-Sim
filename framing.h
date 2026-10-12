#ifndef FRAMING_H
#define FRAMING_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "models.h"

/*
 * Serialize a frame.
 * The function allocates the output buffer.
 *
 * Returns:
 *     size of serialized frame
 *     0 on failure
 */
size_t SerializeFrame(frame_t *frame, uint8_t **buffer);

/*
 * Deserialize a serialized frame.
 *
 * Returns:
 *     1 -> success
 *     2 -> special/end frame
 *     0 -> failure
 */
int DeserializeFrame(uint8_t *buffer, size_t buffer_size, frame_t *frame);

/*
 * Build a frame from input data.
 */
int Framing(frame_t *frame, int source, int dest, uint8_t sequence_no, FILE *input);

/*
 * Create a saturated-traffic test frame.
 */
frame_t *create_test_frame(int source, int destination, uint8_t sequence_no);

/*
 * Free dynamically allocated frame memory.
 */
void free_frame(frame_t *frame);

#endif
