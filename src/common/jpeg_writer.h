#ifndef IPC_COMMON_JPEG_WRITER_H
#define IPC_COMMON_JPEG_WRITER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Encode 8-bit grayscale plane to baseline JPEG. Returns bytes written, or 0 on error. */
size_t jpeg_write_gray(const uint8_t *gray, int width, int height, int stride, uint8_t *out,
                       size_t out_cap, int quality /*1-100*/);

/* Encode NV12 (Y + interleaved UV) as grayscale JPEG using Y plane only. */
size_t jpeg_write_nv12_gray(const uint8_t *nv12, int width, int height, uint8_t *out,
                            size_t out_cap, int quality);

#ifdef __cplusplus
}
#endif

#endif
