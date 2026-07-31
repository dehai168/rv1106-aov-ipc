#include "media/media_osd.h"

#include "common/log.h"
#include "system/config_service.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "rk_comm_rgn.h"
#include "rk_defines.h"
#include "rk_mpi_rgn.h"

/* Overlay: Rockchip documents BGRA5551 (or ARGB8888) for OVERLAY_RGN. */
#define OSD_PIX_FMT RK_FMT_BGRA5551
#define OSD_COLOR_WHITE 0xFFFFu
#define OSD_COLOR_TRANS 0x0000u

#define OSD_HANDLE 0
#define OSD_W 960
#define OSD_H 64
#define OSD_DEFAULT_X 16
#define OSD_DEFAULT_Y 16
#define OSD_FG_ALPHA 128
#define OSD_BG_ALPHA 0
#define OSD_FT_SIZE 24

#define OSD_FONT_W 8
#define OSD_FONT_H 8
#define OSD_SCALE 2 /* drawn glyph = 16x16 (ASCII fallback) */
#define OSD_GLYPH_W (OSD_FONT_W * OSD_SCALE)
#define OSD_GLYPH_H (OSD_FONT_H * OSD_SCALE)

typedef struct {
  int created;
  int attached0;
  int attached1;
  int attached3;
  int thread_running;
  int ft_ready;
  volatile int quit;
  MediaOsdConfig cfg;
  pthread_t tid;
  pthread_mutex_t lock;
  uint16_t *canvas; /* OSD_W * OSD_H */
  FT_Library ft_lib;
  FT_Face ft_face;
} OsdCtx;

static OsdCtx g_osd = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

/*
 * Compact 8x8 font for ASCII 32..126 (public-domain style bitmaps).
 * Each glyph: 8 rows, LSB = leftmost pixel.
 */
static const uint8_t g_font8x8[95][8] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*   */
    {0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00}, /* ! */
    {0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* " */
    {0x36, 0x36, 0x7F, 0x36, 0x7F, 0x36, 0x36, 0x00}, /* # */
    {0x0C, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x0C, 0x00}, /* $ */
    {0x00, 0x63, 0x33, 0x18, 0x0C, 0x66, 0x63, 0x00}, /* % */
    {0x1C, 0x36, 0x1C, 0x6E, 0x3B, 0x33, 0x6E, 0x00}, /* & */
    {0x06, 0x06, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00}, /* ' */
    {0x18, 0x0C, 0x06, 0x06, 0x06, 0x0C, 0x18, 0x00}, /* ( */
    {0x06, 0x0C, 0x18, 0x18, 0x18, 0x0C, 0x06, 0x00}, /* ) */
    {0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00}, /* * */
    {0x00, 0x0C, 0x0C, 0x3F, 0x0C, 0x0C, 0x00, 0x00}, /* + */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x06}, /* , */
    {0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00}, /* - */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00}, /* . */
    {0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x01, 0x00}, /* / */
    {0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0x00}, /* 0 */
    {0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x00}, /* 1 */
    {0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F, 0x00}, /* 2 */
    {0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E, 0x00}, /* 3 */
    {0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78, 0x00}, /* 4 */
    {0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E, 0x00}, /* 5 */
    {0x1C, 0x06, 0x03, 0x1F, 0x33, 0x33, 0x1E, 0x00}, /* 6 */
    {0x3F, 0x33, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x00}, /* 7 */
    {0x1E, 0x33, 0x33, 0x1E, 0x33, 0x33, 0x1E, 0x00}, /* 8 */
    {0x1E, 0x33, 0x33, 0x3E, 0x30, 0x18, 0x0E, 0x00}, /* 9 */
    {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x00}, /* : */
    {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x06}, /* ; */
    {0x18, 0x0C, 0x06, 0x03, 0x06, 0x0C, 0x18, 0x00}, /* < */
    {0x00, 0x00, 0x3F, 0x00, 0x00, 0x3F, 0x00, 0x00}, /* = */
    {0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00}, /* > */
    {0x1E, 0x33, 0x30, 0x18, 0x0C, 0x00, 0x0C, 0x00}, /* ? */
    {0x3E, 0x63, 0x7B, 0x7B, 0x7B, 0x03, 0x1E, 0x00}, /* @ */
    {0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00}, /* A */
    {0x3F, 0x66, 0x66, 0x3E, 0x66, 0x66, 0x3F, 0x00}, /* B */
    {0x3C, 0x66, 0x03, 0x03, 0x03, 0x66, 0x3C, 0x00}, /* C */
    {0x1F, 0x36, 0x66, 0x66, 0x66, 0x36, 0x1F, 0x00}, /* D */
    {0x7F, 0x06, 0x06, 0x3E, 0x06, 0x06, 0x7F, 0x00}, /* E */
    {0x7F, 0x06, 0x06, 0x3E, 0x06, 0x06, 0x06, 0x00}, /* F */
    {0x3C, 0x66, 0x03, 0x03, 0x73, 0x66, 0x7C, 0x00}, /* G */
    {0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00}, /* H */
    {0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00}, /* I */
    {0x78, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E, 0x00}, /* J */
    {0x67, 0x66, 0x36, 0x1E, 0x36, 0x66, 0x67, 0x00}, /* K */
    {0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x7F, 0x00}, /* L */
    {0x63, 0x77, 0x7F, 0x6B, 0x63, 0x63, 0x63, 0x00}, /* M */
    {0x63, 0x67, 0x6F, 0x7B, 0x73, 0x63, 0x63, 0x00}, /* N */
    {0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x00}, /* O */
    {0x3F, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x06, 0x00}, /* P */
    {0x1E, 0x33, 0x33, 0x33, 0x3B, 0x1E, 0x38, 0x00}, /* Q */
    {0x3F, 0x66, 0x66, 0x3E, 0x36, 0x66, 0x67, 0x00}, /* R */
    {0x1E, 0x33, 0x07, 0x0E, 0x38, 0x33, 0x1E, 0x00}, /* S */
    {0x3F, 0x2D, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00}, /* T */
    {0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3F, 0x00}, /* U */
    {0x33, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00}, /* V */
    {0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00}, /* W */
    {0x63, 0x63, 0x36, 0x1C, 0x1C, 0x36, 0x63, 0x00}, /* X */
    {0x33, 0x33, 0x33, 0x1E, 0x0C, 0x0C, 0x1E, 0x00}, /* Y */
    {0x7F, 0x63, 0x31, 0x18, 0x4C, 0x66, 0x7F, 0x00}, /* Z */
    {0x1E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x1E, 0x00}, /* [ */
    {0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00}, /* \ */
    {0x1E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1E, 0x00}, /* ] */
    {0x08, 0x1C, 0x36, 0x63, 0x00, 0x00, 0x00, 0x00}, /* ^ */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF}, /* _ */
    {0x0C, 0x0C, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00}, /* ` */
    {0x00, 0x00, 0x1E, 0x30, 0x3E, 0x33, 0x6E, 0x00}, /* a */
    {0x07, 0x06, 0x06, 0x3E, 0x66, 0x66, 0x3B, 0x00}, /* b */
    {0x00, 0x00, 0x1E, 0x33, 0x03, 0x33, 0x1E, 0x00}, /* c */
    {0x38, 0x30, 0x30, 0x3e, 0x33, 0x33, 0x6E, 0x00}, /* d */
    {0x00, 0x00, 0x1E, 0x33, 0x3f, 0x03, 0x1E, 0x00}, /* e */
    {0x1C, 0x36, 0x06, 0x0f, 0x06, 0x06, 0x0f, 0x00}, /* f */
    {0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x1F}, /* g */
    {0x07, 0x06, 0x36, 0x6E, 0x66, 0x66, 0x67, 0x00}, /* h */
    {0x0C, 0x00, 0x0E, 0x0C, 0x0C, 0x0C, 0x1E, 0x00}, /* i */
    {0x30, 0x00, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E}, /* j */
    {0x07, 0x06, 0x66, 0x36, 0x1E, 0x36, 0x67, 0x00}, /* k */
    {0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00}, /* l */
    {0x00, 0x00, 0x33, 0x7F, 0x7F, 0x6B, 0x63, 0x00}, /* m */
    {0x00, 0x00, 0x1F, 0x33, 0x33, 0x33, 0x33, 0x00}, /* n */
    {0x00, 0x00, 0x1E, 0x33, 0x33, 0x33, 0x1E, 0x00}, /* o */
    {0x00, 0x00, 0x3B, 0x66, 0x66, 0x3E, 0x06, 0x0F}, /* p */
    {0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x78}, /* q */
    {0x00, 0x00, 0x3B, 0x6E, 0x66, 0x06, 0x0F, 0x00}, /* r */
    {0x00, 0x00, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x00}, /* s */
    {0x08, 0x0C, 0x3E, 0x0C, 0x0C, 0x2C, 0x18, 0x00}, /* t */
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x33, 0x6E, 0x00}, /* u */
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00}, /* v */
    {0x00, 0x00, 0x63, 0x6B, 0x7F, 0x7F, 0x36, 0x00}, /* w */
    {0x00, 0x00, 0x63, 0x36, 0x1C, 0x36, 0x63, 0x00}, /* x */
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x3E, 0x30, 0x1F}, /* y */
    {0x00, 0x00, 0x3F, 0x19, 0x0C, 0x26, 0x3F, 0x00}, /* z */
    {0x38, 0x0C, 0x0C, 0x07, 0x0C, 0x0C, 0x38, 0x00}, /* { */
    {0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00}, /* | */
    {0x07, 0x0C, 0x0C, 0x38, 0x0C, 0x0C, 0x07, 0x00}, /* } */
    {0x6E, 0x3B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* ~ */
};

static int osd_align16(int v)
{
  if (v < 0) {
    v = 0;
  }
  return v & ~15;
}

static void osd_normalize(MediaOsdConfig *cfg)
{
  if (!cfg) {
    return;
  }
  cfg->enabled = cfg->enabled ? 1 : 0;
  cfg->show_time = cfg->show_time ? 1 : 0;
  cfg->show_name = cfg->show_name ? 1 : 0;
  /* rk_comm_rgn.h: OVERLAY stPoint X/Y must be 16-aligned on VENC. */
  cfg->pos_x = osd_align16(cfg->pos_x);
  cfg->pos_y = osd_align16(cfg->pos_y);
  cfg->name[sizeof(cfg->name) - 1] = '\0';
}

static void osd_resolve_name(const MediaOsdConfig *cfg, char *out, int out_len)
{
  if (!out || out_len <= 0) {
    return;
  }
  out[0] = '\0';
  if (cfg && cfg->name[0] != '\0') {
    snprintf(out, (size_t)out_len, "%s", cfg->name);
    return;
  }
  config_get_string("device.name", out, out_len, "rv1106-aov-ipc");
}

static void osd_format_text(const MediaOsdConfig *cfg, char *out, int out_len)
{
  char name[64];
  char timestr[32];
  time_t now;
  struct tm tm_now;

  if (!out || out_len <= 0) {
    return;
  }
  out[0] = '\0';
  if (!cfg) {
    return;
  }

  timestr[0] = '\0';
  name[0] = '\0';
  if (cfg->show_time) {
    now = time(NULL);
    localtime_r(&now, &tm_now);
    strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", &tm_now);
  }
  if (cfg->show_name) {
    osd_resolve_name(cfg, name, (int)sizeof(name));
  }

  if (timestr[0] && name[0]) {
    snprintf(out, (size_t)out_len, "%s  %s", timestr, name);
  } else if (timestr[0]) {
    snprintf(out, (size_t)out_len, "%s", timestr);
  } else if (name[0]) {
    snprintf(out, (size_t)out_len, "%s", name);
  }
}

static int utf8_next(const char **pp, unsigned int *cp)
{
  const unsigned char *p;
  if (!pp || !*pp || !cp) {
    return 0;
  }
  p = (const unsigned char *)*pp;
  if (p[0] == 0) {
    return 0;
  }
  if (p[0] < 0x80) {
    *cp = p[0];
    *pp += 1;
    return 1;
  }
  if ((p[0] & 0xE0) == 0xC0 && p[1] != 0) {
    *cp = ((unsigned int)(p[0] & 0x1F) << 6) | (unsigned int)(p[1] & 0x3F);
    *pp += 2;
    return 1;
  }
  if ((p[0] & 0xF0) == 0xE0 && p[1] != 0 && p[2] != 0) {
    *cp = ((unsigned int)(p[0] & 0x0F) << 12) | ((unsigned int)(p[1] & 0x3F) << 6) |
          (unsigned int)(p[2] & 0x3F);
    *pp += 3;
    return 1;
  }
  if ((p[0] & 0xF8) == 0xF0 && p[1] != 0 && p[2] != 0 && p[3] != 0) {
    *cp = ((unsigned int)(p[0] & 0x07) << 18) | ((unsigned int)(p[1] & 0x3F) << 12) |
          ((unsigned int)(p[2] & 0x3F) << 6) | (unsigned int)(p[3] & 0x3F);
    *pp += 4;
    return 1;
  }
  *cp = (unsigned int)'?';
  *pp += 1;
  return 1;
}

static void osd_draw_char_ascii(uint16_t *canvas, int canvas_w, int canvas_h, int x, int y, char ch)
{
  const uint8_t *glyph;
  int gx, gy, sx, sy;
  int idx;

  if (!canvas || ch < 32 || ch > 126) {
    return;
  }
  idx = ch - 32;
  glyph = g_font8x8[idx];
  for (gy = 0; gy < OSD_FONT_H; gy++) {
    uint8_t row = glyph[gy];
    for (gx = 0; gx < OSD_FONT_W; gx++) {
      if ((row & (1u << gx)) == 0) {
        continue;
      }
      for (sy = 0; sy < OSD_SCALE; sy++) {
        for (sx = 0; sx < OSD_SCALE; sx++) {
          int px = x + gx * OSD_SCALE + sx;
          int py = y + gy * OSD_SCALE + sy;
          if (px >= 0 && px < canvas_w && py >= 0 && py < canvas_h) {
            canvas[py * canvas_w + px] = OSD_COLOR_WHITE;
          }
        }
      }
    }
  }
}

static void osd_ft_close(void)
{
  if (g_osd.ft_face) {
    FT_Done_Face(g_osd.ft_face);
    g_osd.ft_face = NULL;
  }
  if (g_osd.ft_lib) {
    FT_Done_FreeType(g_osd.ft_lib);
    g_osd.ft_lib = NULL;
  }
  g_osd.ft_ready = 0;
}

static int osd_ft_try_open(const char *path)
{
  if (!path || !path[0]) {
    return -1;
  }
  if (access(path, R_OK) != 0) {
    return -1;
  }
  if (FT_New_Face(g_osd.ft_lib, path, 0, &g_osd.ft_face) != 0) {
    g_osd.ft_face = NULL;
    return -1;
  }
  if (FT_Set_Pixel_Sizes(g_osd.ft_face, 0, OSD_FT_SIZE) != 0) {
    FT_Done_Face(g_osd.ft_face);
    g_osd.ft_face = NULL;
    return -1;
  }
  log_info("osd", "freetype font %s size=%d", path, OSD_FT_SIZE);
  return 0;
}

static int osd_ft_ensure(void)
{
  char path[160];
  static const char *k_paths[] = {
      "/userdata/fonts/simsun_cn_3000.ttf",
      "/oem/usr/share/simsun_cn_3000.ttf",
      "/oem/usr/share/simsun_en.ttf",
      NULL,
  };
  int i;

  if (g_osd.ft_ready) {
    return 0;
  }
  if (FT_Init_FreeType(&g_osd.ft_lib) != 0) {
    g_osd.ft_lib = NULL;
    log_warn("osd", "FT_Init_FreeType failed");
    return -1;
  }
  path[0] = '\0';
  config_get_string("video.osd.font_path", path, (int)sizeof(path), "");
  if (path[0] && osd_ft_try_open(path) == 0) {
    g_osd.ft_ready = 1;
    return 0;
  }
  for (i = 0; k_paths[i]; i++) {
    if (osd_ft_try_open(k_paths[i]) == 0) {
      g_osd.ft_ready = 1;
      return 0;
    }
  }
  log_warn("osd", "no CJK font; ASCII bitmap fallback only");
  FT_Done_FreeType(g_osd.ft_lib);
  g_osd.ft_lib = NULL;
  return -1;
}

static void osd_draw_glyph_ft(uint16_t *canvas, int canvas_w, int canvas_h, int *x, int y_base,
                              unsigned int cp)
{
  FT_GlyphSlot slot;
  int row, col;
  int pen_x;

  if (!g_osd.ft_ready || !g_osd.ft_face || !x) {
    return;
  }
  if (FT_Load_Char(g_osd.ft_face, cp, FT_LOAD_RENDER) != 0) {
    return;
  }
  slot = g_osd.ft_face->glyph;
  pen_x = *x + slot->bitmap_left;
  for (row = 0; row < (int)slot->bitmap.rows; row++) {
    int py = y_base + (OSD_FT_SIZE - slot->bitmap_top) + row;
    const unsigned char *src = slot->bitmap.buffer + row * slot->bitmap.pitch;
    for (col = 0; col < (int)slot->bitmap.width; col++) {
      int px = pen_x + col;
      if (src[col] < 64) {
        continue;
      }
      if (px >= 0 && px < canvas_w && py >= 0 && py < canvas_h) {
        canvas[py * canvas_w + px] = OSD_COLOR_WHITE;
      }
    }
  }
  *x += (int)(slot->advance.x >> 6);
  if (*x < 0) {
    *x = 0;
  }
}

static void osd_render_text(uint16_t *canvas, int canvas_w, int canvas_h, const char *text)
{
  int x = 4;
  int y_ascii = (canvas_h - OSD_GLYPH_H) / 2;
  int y_ft = (canvas_h - OSD_FT_SIZE) / 2;
  const char *p;
  unsigned int cp;
  int use_ft;

  if (!canvas) {
    return;
  }
  {
    int i;
    for (i = 0; i < canvas_w * canvas_h; i++) {
      canvas[i] = OSD_COLOR_TRANS;
    }
  }
  if (!text || !text[0]) {
    return;
  }

  use_ft = (osd_ft_ensure() == 0);
  p = text;
  while (utf8_next(&p, &cp)) {
    if (use_ft) {
      if (x >= canvas_w - 4) {
        break;
      }
      osd_draw_glyph_ft(canvas, canvas_w, canvas_h, &x, y_ft, cp);
    } else {
      if (x + OSD_GLYPH_W > canvas_w) {
        break;
      }
      if (cp >= 32 && cp <= 126) {
        osd_draw_char_ascii(canvas, canvas_w, canvas_h, x, y_ascii, (char)cp);
      }
      x += OSD_GLYPH_W;
    }
  }
}

static int osd_push_bitmap(void)
{
  BITMAP_S bmp;
  int ret;

  if (!g_osd.canvas || !g_osd.created) {
    return -1;
  }
  memset(&bmp, 0, sizeof(bmp));
  bmp.enPixelFormat = OSD_PIX_FMT;
  bmp.u32Width = OSD_W;
  bmp.u32Height = OSD_H;
  bmp.pData = g_osd.canvas;
  ret = RK_MPI_RGN_SetBitMap(OSD_HANDLE, &bmp);
  if (ret != RK_SUCCESS) {
    log_warn("osd", "SetBitMap failed %#x", ret);
    return -1;
  }
  return 0;
}

static int osd_refresh_with_cfg(const MediaOsdConfig *cfg)
{
  char text[160];
  if (!cfg || !g_osd.canvas || !g_osd.created) {
    return -1;
  }
  osd_format_text(cfg, text, (int)sizeof(text));
  osd_render_text(g_osd.canvas, OSD_W, OSD_H, text);
  return osd_push_bitmap();
}

static int osd_refresh_once(void)
{
  MediaOsdConfig cfg;
  pthread_mutex_lock(&g_osd.lock);
  cfg = g_osd.cfg;
  pthread_mutex_unlock(&g_osd.lock);
  return osd_refresh_with_cfg(&cfg);
}

static void *osd_thread(void *arg)
{
  (void)arg;
  while (!g_osd.quit) {
    osd_refresh_once();
    {
      int i;
      for (i = 0; i < 10 && !g_osd.quit; i++) {
        usleep(100 * 1000);
      }
    }
  }
  return NULL;
}

static void osd_stop_thread_locked(void)
{
  if (g_osd.thread_running) {
    g_osd.quit = 1;
    pthread_mutex_unlock(&g_osd.lock);
    pthread_join(g_osd.tid, NULL);
    pthread_mutex_lock(&g_osd.lock);
    g_osd.thread_running = 0;
    g_osd.quit = 0;
  }
}

static void osd_detach_destroy_locked(void)
{
  MPP_CHN_S chn;

  osd_stop_thread_locked();

  memset(&chn, 0, sizeof(chn));
  chn.enModId = RK_ID_VENC;
  chn.s32DevId = 0;
  if (g_osd.attached0) {
    chn.s32ChnId = 0;
    RK_MPI_RGN_DetachFromChn(OSD_HANDLE, &chn);
    g_osd.attached0 = 0;
  }
  if (g_osd.attached1) {
    chn.s32ChnId = 1;
    RK_MPI_RGN_DetachFromChn(OSD_HANDLE, &chn);
    g_osd.attached1 = 0;
  }
  if (g_osd.attached3) {
    chn.s32ChnId = 3;
    RK_MPI_RGN_DetachFromChn(OSD_HANDLE, &chn);
    g_osd.attached3 = 0;
  }
  if (g_osd.created) {
    RK_MPI_RGN_Destroy(OSD_HANDLE);
    g_osd.created = 0;
  }
  if (g_osd.canvas) {
    free(g_osd.canvas);
    g_osd.canvas = NULL;
  }
  osd_ft_close();
}

static int osd_attach_chn(int venc_chn, int pos_x, int pos_y)
{
  MPP_CHN_S chn;
  RGN_CHN_ATTR_S chn_attr;
  int ret;

  memset(&chn, 0, sizeof(chn));
  chn.enModId = RK_ID_VENC;
  chn.s32DevId = 0;
  chn.s32ChnId = venc_chn;

  memset(&chn_attr, 0, sizeof(chn_attr));
  chn_attr.bShow = RK_TRUE;
  chn_attr.enType = OVERLAY_RGN;
  chn_attr.unChnAttr.stOverlayChn.stPoint.s32X = pos_x;
  chn_attr.unChnAttr.stOverlayChn.stPoint.s32Y = pos_y;
  chn_attr.unChnAttr.stOverlayChn.u32FgAlpha = OSD_FG_ALPHA;
  chn_attr.unChnAttr.stOverlayChn.u32BgAlpha = OSD_BG_ALPHA;
  chn_attr.unChnAttr.stOverlayChn.u32Layer = 0;

  ret = RK_MPI_RGN_AttachToChn(OSD_HANDLE, &chn, &chn_attr);
  if (ret != RK_SUCCESS) {
    log_warn("osd", "AttachToChn venc%d failed %#x", venc_chn, ret);
    return -1;
  }
  return 0;
}

static int osd_create_and_attach(int pos_x, int pos_y)
{
  RGN_ATTR_S attr;
  int ret;

  if (!g_osd.canvas) {
    g_osd.canvas = (uint16_t *)calloc((size_t)OSD_W * (size_t)OSD_H, sizeof(uint16_t));
    if (!g_osd.canvas) {
      log_warn("osd", "canvas alloc failed");
      return -1;
    }
  }

  memset(&attr, 0, sizeof(attr));
  attr.enType = OVERLAY_RGN;
  attr.unAttr.stOverlay.enPixelFmt = OSD_PIX_FMT;
  attr.unAttr.stOverlay.stSize.u32Width = OSD_W;
  attr.unAttr.stOverlay.stSize.u32Height = OSD_H;
  attr.unAttr.stOverlay.u32CanvasNum = 2;

  ret = RK_MPI_RGN_Create(OSD_HANDLE, &attr);
  if (ret != RK_SUCCESS) {
    log_warn("osd", "RGN_Create failed %#x", ret);
    return -1;
  }
  g_osd.created = 1;
  (void)osd_ft_ensure();

  if (osd_attach_chn(0, pos_x, pos_y) != 0) {
    RK_MPI_RGN_Destroy(OSD_HANDLE);
    g_osd.created = 0;
    return -2;
  }
  g_osd.attached0 = 1;

  if (osd_attach_chn(1, pos_x, pos_y) != 0) {
    log_warn("osd", "sub stream attach failed; continuing");
  } else {
    g_osd.attached1 = 1;
  }
  if (osd_attach_chn(3, pos_x, pos_y) != 0) {
    log_warn("osd", "preview-main attach failed; continuing");
  } else {
    g_osd.attached3 = 1;
  }
  return 0;
}

static int osd_start_thread_locked(void)
{
  int err;
  if (g_osd.thread_running) {
    return 0;
  }
  g_osd.quit = 0;
  err = pthread_create(&g_osd.tid, NULL, osd_thread, NULL);
  if (err != 0) {
    log_warn("osd", "pthread_create failed %d", err);
    return -1;
  }
  g_osd.thread_running = 1;
  return 0;
}

int media_osd_load_config(MediaOsdConfig *out)
{
  if (!out) {
    return -1;
  }
  memset(out, 0, sizeof(*out));
  config_get_bool("video.osd.enabled", &out->enabled, 0);
  config_get_bool("video.osd.show_time", &out->show_time, 1);
  config_get_bool("video.osd.show_name", &out->show_name, 1);
  config_get_int("video.osd.pos_x", &out->pos_x, OSD_DEFAULT_X);
  config_get_int("video.osd.pos_y", &out->pos_y, OSD_DEFAULT_Y);
  config_get_string("video.osd.name", out->name, (int)sizeof(out->name), "");
  osd_normalize(out);
  return 0;
}

int media_osd_save_config(const MediaOsdConfig *in)
{
  MediaOsdConfig cfg;
  if (!in) {
    return -1;
  }
  cfg = *in;
  osd_normalize(&cfg);

  if (config_set_bool("video.osd.enabled", cfg.enabled) != 0 ||
      config_set_bool("video.osd.show_time", cfg.show_time) != 0 ||
      config_set_bool("video.osd.show_name", cfg.show_name) != 0 ||
      config_set_int("video.osd.pos_x", cfg.pos_x) != 0 ||
      config_set_int("video.osd.pos_y", cfg.pos_y) != 0 ||
      config_set_string("video.osd.name", cfg.name) != 0 || config_save() != 0) {
    return -1;
  }
  return 0;
}

int media_osd_apply(const MediaOsdConfig *cfg)
{
  MediaOsdConfig local;
  MediaOsdConfig prev;
  int rc = 0;
  int pos_changed;

  if (!cfg) {
    return -1;
  }
  local = *cfg;
  osd_normalize(&local);

  pthread_mutex_lock(&g_osd.lock);
  prev = g_osd.cfg;
  g_osd.cfg = local;
  pos_changed = (prev.pos_x != local.pos_x || prev.pos_y != local.pos_y);

  if (!local.enabled || (!local.show_time && !local.show_name)) {
    osd_detach_destroy_locked();
    pthread_mutex_unlock(&g_osd.lock);
    return 0;
  }

  if (!g_osd.created || pos_changed) {
    /* Recreate on position change: VENC overlay Y needs 16-align + re-attach. */
    if (g_osd.created) {
      osd_detach_destroy_locked();
      g_osd.cfg = local;
    }
    rc = osd_create_and_attach(local.pos_x, local.pos_y);
    if (rc != 0) {
      osd_detach_destroy_locked();
      pthread_mutex_unlock(&g_osd.lock);
      return rc;
    }
  }

  pthread_mutex_unlock(&g_osd.lock);
  if (osd_refresh_with_cfg(&local) != 0) {
    log_warn("osd", "bitmap refresh failed");
  }
  pthread_mutex_lock(&g_osd.lock);

  if (osd_start_thread_locked() != 0) {
    log_warn("osd", "refresh thread not started");
  }

  pthread_mutex_unlock(&g_osd.lock);
  log_info("osd", "apply enabled time=%d name=%d pos=%d,%d", local.show_time, local.show_name,
           local.pos_x, local.pos_y);
  return 0;
}

void media_osd_stop(void)
{
  pthread_mutex_lock(&g_osd.lock);
  osd_detach_destroy_locked();
  pthread_mutex_unlock(&g_osd.lock);
}

