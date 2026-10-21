#include "common/jpeg_writer.h"

#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "../../third_party/stb/stb_image_write.h"

typedef struct {
  uint8_t *out;
  size_t cap;
  size_t len;
  int overflow;
} JpgMem;

static void jpg_mem_write(void *context, void *data, int size)
{
  JpgMem *m = (JpgMem *)context;
  if (!m || m->overflow || size <= 0) return;
  if (m->len + (size_t)size > m->cap) {
    m->overflow = 1;
    return;
  }
  memcpy(m->out + m->len, data, (size_t)size);
  m->len += (size_t)size;
}

size_t jpeg_write_rgb(const uint8_t *rgb, int width, int height, int stride, uint8_t *out,
                      size_t out_cap, int quality)
{
  JpgMem mem;
  const uint8_t *src = rgb;
  uint8_t *packed = NULL;
  int ok;

  if (!rgb || !out || width <= 0 || height <= 0 || stride < width * 3 || out_cap < 512) {
    return 0;
  }
  if (quality < 1) quality = 1;
  if (quality > 100) quality = 100;

  if (stride != width * 3) {
    int y;
    packed = (uint8_t *)malloc((size_t)width * (size_t)height * 3u);
    if (!packed) return 0;
    for (y = 0; y < height; y++) {
      memcpy(packed + (size_t)y * (size_t)width * 3u, rgb + (size_t)y * (size_t)stride,
             (size_t)width * 3u);
    }
    src = packed;
  }

  mem.out = out;
  mem.cap = out_cap;
  mem.len = 0;
  mem.overflow = 0;
  ok = stbi_write_jpg_to_func(jpg_mem_write, &mem, width, height, 3, src, quality);
  free(packed);
  if (!ok || mem.overflow || mem.len == 0) return 0;
  return mem.len;
}
