#include "common/jpeg_reader.h"

#include <stdlib.h>
#include <string.h>

/* NanoJPEG is not thread-safe; face_service is the sole caller. */
#define NJ_USE_LIBC 1
#include "../../third_party/nanojpeg/nanojpeg.c"

int jpeg_decode_rgb(const uint8_t *jpeg, size_t jpeg_len, uint8_t **rgb_out, int *width,
                    int *height)
{
  uint8_t *copy;
  int w, h, n;
  nj_result_t rc;

  if (!jpeg || jpeg_len == 0 || !rgb_out || !width || !height) return -1;
  *rgb_out = NULL;
  *width = 0;
  *height = 0;

  njInit();
  rc = njDecode(jpeg, (int)jpeg_len);
  if (rc != NJ_OK) {
    njDone();
    return -2;
  }
  w = njGetWidth();
  h = njGetHeight();
  n = w * h * 3;
  if (w <= 0 || h <= 0 || n <= 0 || njIsColor() == 0) {
    /* Grayscale: expand to RGB. */
    if (w > 0 && h > 0 && njIsColor() == 0) {
      const unsigned char *g = njGetImage();
      int i;
      copy = (uint8_t *)malloc((size_t)w * (size_t)h * 3);
      if (!copy) {
        njDone();
        return -3;
      }
      for (i = 0; i < w * h; i++) {
        copy[i * 3] = g[i];
        copy[i * 3 + 1] = g[i];
        copy[i * 3 + 2] = g[i];
      }
      *rgb_out = copy;
      *width = w;
      *height = h;
      njDone();
      return 0;
    }
    njDone();
    return -2;
  }
  copy = (uint8_t *)malloc((size_t)n);
  if (!copy) {
    njDone();
    return -3;
  }
  memcpy(copy, njGetImage(), (size_t)n);
  *rgb_out = copy;
  *width = w;
  *height = h;
  njDone();
  return 0;
}
