#include "common/jpeg_writer.h"

#include <string.h>

/* Minimal baseline grayscale JPEG encoder (quality approx via quant scale). */

static const unsigned char k_zz[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

static const unsigned char k_std_lum_quant[64] = {
    16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
    14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
    18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};

static const unsigned char k_std_dc_lum_nrcodes[17] = {0, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
static const unsigned char k_std_dc_lum_values[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static const unsigned char k_std_ac_lum_nrcodes[17] = {0, 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
static const unsigned char k_std_ac_lum_values[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa};

typedef struct {
  uint8_t *out;
  size_t cap;
  size_t len;
  unsigned bitbuf;
  int bitn;
  int err;
  unsigned char quant[64];
  unsigned short dc_code[12];
  unsigned char dc_len[12];
  unsigned short ac_code[256];
  unsigned char ac_len[256];
} Jpg;

static void put_byte(Jpg *j, unsigned char b)
{
  if (j->err) {
    return;
  }
  if (j->len >= j->cap) {
    j->err = 1;
    return;
  }
  j->out[j->len++] = b;
}

static void put_bits(Jpg *j, unsigned code, int size)
{
  if (size <= 0 || j->err) {
    return;
  }
  j->bitbuf = (j->bitbuf << size) | (code & ((1u << size) - 1));
  j->bitn += size;
  while (j->bitn >= 8) {
    unsigned char b = (unsigned char)((j->bitbuf >> (j->bitn - 8)) & 0xff);
    put_byte(j, b);
    if (b == 0xff) {
      put_byte(j, 0x00);
    }
    j->bitn -= 8;
  }
}

static void flush_bits(Jpg *j)
{
  if (j->bitn > 0) {
    put_bits(j, (1u << (8 - j->bitn)) - 1, 8 - j->bitn);
  }
}

static void compute_huff(const unsigned char *nrcodes, const unsigned char *values, int nvalues,
                         unsigned short *code, unsigned char *len)
{
  unsigned char bits[17];
  unsigned char huffval[256];
  int i, j, k = 0;
  unsigned int c = 0;
  int si;
  memcpy(bits, nrcodes, 17);
  memcpy(huffval, values, (size_t)nvalues);
  for (i = 0; i < 256; i++) {
    code[i] = 0;
    len[i] = 0;
  }
  for (i = 1; i <= 16; i++) {
    for (j = 0; j < bits[i]; j++) {
      len[huffval[k]] = (unsigned char)i;
      k++;
    }
  }
  k = 0;
  c = 0;
  si = len[huffval[0]];
  while (k < nvalues) {
    while (len[huffval[k]] == si) {
      code[huffval[k]] = (unsigned short)c;
      c++;
      k++;
      if (k >= nvalues) {
        break;
      }
    }
    c <<= 1;
    si++;
  }
}

static void fdct(float *d)
{
  int i;
  for (i = 0; i < 8; i++) {
    float *p = d + i * 8;
    float t0 = p[0] + p[7], t7 = p[0] - p[7];
    float t1 = p[1] + p[6], t6 = p[1] - p[6];
    float t2 = p[2] + p[5], t5 = p[2] - p[5];
    float t3 = p[3] + p[4], t4 = p[3] - p[4];
    float t10 = t0 + t3, t13 = t0 - t3;
    float t11 = t1 + t2, t12 = t1 - t2;
    p[0] = t10 + t11;
    p[4] = t10 - t11;
    float z1 = (t12 + t13) * 0.707106781f;
    p[2] = t13 + z1;
    p[6] = t13 - z1;
    t10 = t4 + t5;
    t11 = t5 + t6;
    t12 = t6 + t7;
    float z5 = (t10 - t12) * 0.382683433f;
    float z2 = 0.541196100f * t10 + z5;
    float z4 = 1.306562965f * t12 + z5;
    float z3 = t11 * 0.707106781f;
    float z11 = t7 + z3, z13 = t7 - z3;
    p[5] = z13 + z2;
    p[3] = z13 - z2;
    p[1] = z11 + z4;
    p[7] = z11 - z4;
  }
  for (i = 0; i < 8; i++) {
    float *p = d + i;
    float t0 = p[0] + p[56], t7 = p[0] - p[56];
    float t1 = p[8] + p[48], t6 = p[8] - p[48];
    float t2 = p[16] + p[40], t5 = p[16] - p[40];
    float t3 = p[24] + p[32], t4 = p[24] - p[32];
    float t10 = t0 + t3, t13 = t0 - t3;
    float t11 = t1 + t2, t12 = t1 - t2;
    p[0] = t10 + t11;
    p[32] = t10 - t11;
    float z1 = (t12 + t13) * 0.707106781f;
    p[16] = t13 + z1;
    p[48] = t13 - z1;
    t10 = t4 + t5;
    t11 = t5 + t6;
    t12 = t6 + t7;
    float z5 = (t10 - t12) * 0.382683433f;
    float z2 = 0.541196100f * t10 + z5;
    float z4 = 1.306562965f * t12 + z5;
    float z3 = t11 * 0.707106781f;
    float z11 = t7 + z3, z13 = t7 - z3;
    p[40] = z13 + z2;
    p[24] = z13 - z2;
    p[8] = z11 + z4;
    p[56] = z11 - z4;
  }
}

static int bitcount(int v)
{
  int n = 0;
  if (v < 0) {
    v = -v;
  }
  while (v) {
    n++;
    v >>= 1;
  }
  return n;
}

static void encode_block(Jpg *j, float *du, int *dc)
{
  int duq[64];
  int i;
  int last = -1;
  fdct(du);
  for (i = 0; i < 64; i++) {
    float v = du[k_zz[i]] / (float)j->quant[i];
    duq[i] = (int)(v < 0 ? v - 0.5f : v + 0.5f);
  }
  {
    int diff = duq[0] - *dc;
    *dc = duq[0];
    int bits = bitcount(diff);
    put_bits(j, j->dc_code[bits], j->dc_len[bits]);
    if (bits) {
      int v = diff;
      if (v < 0) {
        v = v - 1;
      }
      put_bits(j, (unsigned)v, bits);
    }
  }
  for (i = 1; i < 64; i++) {
    if (duq[i] == 0) {
      continue;
    }
    {
      int run = i - last - 1;
      while (run > 15) {
        put_bits(j, j->ac_code[0xf0], j->ac_len[0xf0]);
        run -= 16;
      }
      {
        int bits = bitcount(duq[i]);
        int rs = (run << 4) | bits;
        put_bits(j, j->ac_code[rs], j->ac_len[rs]);
        {
          int v = duq[i];
          if (v < 0) {
            v = v - 1;
          }
          put_bits(j, (unsigned)v, bits);
        }
      }
      last = i;
    }
  }
  if (last < 63) {
    put_bits(j, j->ac_code[0], j->ac_len[0]);
  }
}

size_t jpeg_write_gray(const uint8_t *gray, int width, int height, int stride, uint8_t *out,
                       size_t out_cap, int quality)
{
  Jpg j;
  int q;
  int i;
  int dc = 0;
  int x, y;

  if (!gray || !out || width <= 0 || height <= 0 || stride < width || out_cap < 256) {
    return 0;
  }
  memset(&j, 0, sizeof(j));
  j.out = out;
  j.cap = out_cap;
  if (quality < 1) {
    quality = 1;
  }
  if (quality > 100) {
    quality = 100;
  }
  q = quality < 50 ? 5000 / quality : 200 - quality * 2;
  for (i = 0; i < 64; i++) {
    int v = (k_std_lum_quant[i] * q + 50) / 100;
    if (v < 1) {
      v = 1;
    }
    if (v > 255) {
      v = 255;
    }
    j.quant[k_zz[i]] = (unsigned char)v;
  }
  compute_huff(k_std_dc_lum_nrcodes, k_std_dc_lum_values, 12, j.dc_code, j.dc_len);
  compute_huff(k_std_ac_lum_nrcodes, k_std_ac_lum_values, 162, j.ac_code, j.ac_len);

  put_byte(&j, 0xff);
  put_byte(&j, 0xd8);
  put_byte(&j, 0xff);
  put_byte(&j, 0xe0);
  put_byte(&j, 0x00);
  put_byte(&j, 0x10);
  put_byte(&j, 'J');
  put_byte(&j, 'F');
  put_byte(&j, 'I');
  put_byte(&j, 'F');
  put_byte(&j, 0);
  put_byte(&j, 1);
  put_byte(&j, 1);
  put_byte(&j, 0);
  put_byte(&j, 0);
  put_byte(&j, 1);
  put_byte(&j, 0);
  put_byte(&j, 1);
  put_byte(&j, 0);
  put_byte(&j, 0);

  put_byte(&j, 0xff);
  put_byte(&j, 0xdb);
  put_byte(&j, 0x00);
  put_byte(&j, 0x43);
  put_byte(&j, 0x00);
  for (i = 0; i < 64; i++) {
    put_byte(&j, j.quant[k_zz[i]]);
  }

  put_byte(&j, 0xff);
  put_byte(&j, 0xc0);
  put_byte(&j, 0x00);
  put_byte(&j, 0x0b);
  put_byte(&j, 0x08);
  put_byte(&j, (unsigned char)(height >> 8));
  put_byte(&j, (unsigned char)height);
  put_byte(&j, (unsigned char)(width >> 8));
  put_byte(&j, (unsigned char)width);
  put_byte(&j, 1);
  put_byte(&j, 1);
  put_byte(&j, 0x11);
  put_byte(&j, 0x00);

  put_byte(&j, 0xff);
  put_byte(&j, 0xc4);
  put_byte(&j, 0x00);
  put_byte(&j, 0x1f);
  put_byte(&j, 0x00);
  for (i = 1; i <= 16; i++) {
    put_byte(&j, k_std_dc_lum_nrcodes[i]);
  }
  for (i = 0; i < 12; i++) {
    put_byte(&j, k_std_dc_lum_values[i]);
  }
  put_byte(&j, 0xff);
  put_byte(&j, 0xc4);
  put_byte(&j, 0x00);
  put_byte(&j, 0xb5);
  put_byte(&j, 0x10);
  for (i = 1; i <= 16; i++) {
    put_byte(&j, k_std_ac_lum_nrcodes[i]);
  }
  for (i = 0; i < 162; i++) {
    put_byte(&j, k_std_ac_lum_values[i]);
  }

  put_byte(&j, 0xff);
  put_byte(&j, 0xda);
  put_byte(&j, 0x00);
  put_byte(&j, 0x08);
  put_byte(&j, 0x01);
  put_byte(&j, 0x01);
  put_byte(&j, 0x00);
  put_byte(&j, 0x00);
  put_byte(&j, 0x3f);
  put_byte(&j, 0x00);

  for (y = 0; y < height; y += 8) {
    for (x = 0; x < width; x += 8) {
      float du[64];
      int yy, xx;
      for (yy = 0; yy < 8; yy++) {
        for (xx = 0; xx < 8; xx++) {
          int px = x + xx;
          int py = y + yy;
          if (px >= width) {
            px = width - 1;
          }
          if (py >= height) {
            py = height - 1;
          }
          du[yy * 8 + xx] = (float)gray[py * stride + px] - 128.0f;
        }
      }
      encode_block(&j, du, &dc);
      if (j.err) {
        return 0;
      }
    }
  }
  flush_bits(&j);
  put_byte(&j, 0xff);
  put_byte(&j, 0xd9);
  return j.err ? 0 : j.len;
}

size_t jpeg_write_nv12_gray(const uint8_t *nv12, int width, int height, uint8_t *out,
                            size_t out_cap, int quality)
{
  if (!nv12) {
    return 0;
  }
  return jpeg_write_gray(nv12, width, height, width, out, out_cap, quality);
}
