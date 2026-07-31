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

/* Encode NV12 as baseline YCbCr 4:2:0 JPEG (color).
 * Contiguous layout: UV at nv12 + y_stride * y_rows. */
size_t jpeg_write_nv12(const uint8_t *nv12, int width, int height, int y_stride, int y_rows,
                       uint8_t *out, size_t out_cap, int quality /*1-100*/);

/* Same as jpeg_write_nv12 but Y/UV may be non-contiguous (Rockchip pVirAddr[0/1]). */
size_t jpeg_write_nv12_planes(const uint8_t *y, const uint8_t *uv, int width, int height,
                              int y_stride, int uv_stride, uint8_t *out, size_t out_cap,
                              int quality /*1-100*/);

#ifdef __cplusplus
}
#endif

#endif
