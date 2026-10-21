#ifndef IPC_COMMON_JPEG_READER_H
#define IPC_COMMON_JPEG_READER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decode baseline JPEG to packed RGB24. Caller frees *rgb_out with free(). */
int jpeg_decode_rgb(const uint8_t *jpeg, size_t jpeg_len, uint8_t **rgb_out, int *width,
                    int *height);

#ifdef __cplusplus
}
#endif

#endif /* IPC_COMMON_JPEG_READER_H */
