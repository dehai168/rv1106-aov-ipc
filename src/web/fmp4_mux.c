#include "web/fmp4_mux.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void w16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

static void w32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static void w64(uint8_t *p, uint64_t v)
{
  w32(p, (uint32_t)(v >> 32));
  w32(p + 4, (uint32_t)v);
}

typedef struct {
  const uint8_t *d;
  int len;
  int bit;
} Br;

static int br_u(Br *b, int n)
{
  int v = 0;
  while (n-- > 0) {
    int byte = b->bit >> 3;
    if (byte >= b->len) {
      return 0;
    }
    v = (v << 1) | ((b->d[byte] >> (7 - (b->bit & 7))) & 1);
    b->bit++;
  }
  return v;
}

static int br_ue(Br *b)
{
  int z = 0;
  while (br_u(b, 1) == 0 && z < 31) {
    z++;
  }
  return ((1 << z) - 1) + (z ? br_u(b, z) : 0);
}

static int br_se(Br *b)
{
  int v = br_ue(b);
  return (v & 1) ? ((v + 1) / 2) : -(v / 2);
}

static void sps_dim(const uint8_t *sps, int len, int *w, int *h)
{
  Br b = {.d = sps, .len = len, .bit = 8};
  int profile_idc = br_u(&b, 8);
  br_u(&b, 8);
  br_u(&b, 8);
  br_ue(&b);
  if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 || profile_idc == 244 ||
      profile_idc == 44 || profile_idc == 83 || profile_idc == 86 || profile_idc == 118 ||
      profile_idc == 128) {
    int chroma = br_ue(&b);
    if (chroma == 3) {
      br_u(&b, 1);
    }
    br_ue(&b);
    br_ue(&b);
    br_u(&b, 1);
    if (br_u(&b, 1)) {
      int i;
      for (i = 0; i < (chroma != 3 ? 8 : 12); i++) {
        if (br_u(&b, 1)) {
          int next = 8, j, count = i < 6 ? 16 : 64;
          for (j = 0; j < count; j++) {
            next = (next + br_se(&b) + 256) % 256;
          }
        }
      }
    }
  }
  br_ue(&b);
  {
    int poc = br_ue(&b);
    if (poc == 0) {
      br_ue(&b);
    } else if (poc == 1) {
      int i, n;
      br_u(&b, 1);
      br_se(&b);
      br_se(&b);
      n = br_ue(&b);
      for (i = 0; i < n; i++) {
        br_se(&b);
      }
    }
  }
  br_ue(&b);
  br_u(&b, 1);
  *w = (br_ue(&b) + 1) * 16;
  *h = (br_ue(&b) + 1) * 16;
  if (!br_u(&b, 1)) {
    br_u(&b, 1);
  }
  br_u(&b, 1);
  if (br_u(&b, 1)) {
    int l = br_ue(&b), r = br_ue(&b), t = br_ue(&b), bot = br_ue(&b);
    *w -= (l + r) * 2;
    *h -= (t + bot) * 2;
  }
  if (*w < 16 || *h < 16) {
    *w = 704;
    *h = 576;
  }
}

int fmp4_mux_init(Fmp4Mux *m, Fmp4EmitCb emit, void *user)
{
  memset(m, 0, sizeof(*m));
  m->emit = emit;
  m->emit_user = user;
  m->width = 704;
  m->height = 576;
  m->seq = 1;
  m->scratch_cap = 512 * 1024;
  m->scratch = (uint8_t *)malloc(m->scratch_cap);
  return m->scratch ? 0 : -1;
}

void fmp4_mux_reset(Fmp4Mux *m)
{
  m->sps_len = m->pps_len = 0;
  m->ready = 0;
  m->init_sent = 0;
  m->seq = 1;
  m->has_base = 0;
  m->codec[0] = '\0';
}

void fmp4_mux_deinit(Fmp4Mux *m)
{
  free(m->scratch);
  memset(m, 0, sizeof(*m));
}

static int find_start(const uint8_t *d, uint32_t len, uint32_t from, int *sc)
{
  uint32_t i;
  for (i = from; i + 3 < len; i++) {
    if (d[i] == 0 && d[i + 1] == 0) {
      if (d[i + 2] == 1) {
        *sc = 3;
        return (int)i;
      }
      if (d[i + 2] == 0 && d[i + 3] == 1) {
        *sc = 4;
        return (int)i;
      }
    }
  }
  return -1;
}

static uint32_t annexb_to_avcc(const uint8_t *in, uint32_t in_len, uint8_t *out, uint32_t out_cap,
                               Fmp4Mux *m, int *got_vcl, int *is_idr)
{
  uint32_t out_len = 0;
  int sc = 0;
  int pos = find_start(in, in_len, 0, &sc);
  *got_vcl = 0;
  *is_idr = 0;
  while (pos >= 0) {
    int nsc = 0;
    int npos = find_start(in, in_len, (uint32_t)pos + (uint32_t)sc, &nsc);
    uint32_t nal_off = (uint32_t)pos + (uint32_t)sc;
    uint32_t nal_end = npos >= 0 ? (uint32_t)npos : in_len;
    uint32_t nal_len;
    const uint8_t *nal;
    int ntype;
    while (nal_end > nal_off && in[nal_end - 1] == 0) {
      nal_end--;
    }
    if (nal_end <= nal_off) {
      pos = npos;
      sc = nsc;
      continue;
    }
    nal = in + nal_off;
    nal_len = nal_end - nal_off;
    ntype = nal[0] & 0x1f;
    if (ntype == 7 && nal_len < sizeof(m->sps)) {
      memcpy(m->sps, nal, nal_len);
      m->sps_len = (int)nal_len;
      sps_dim(m->sps, m->sps_len, &m->width, &m->height);
      snprintf(m->codec, sizeof(m->codec), "avc1.%02X%02X%02X", m->sps[1], m->sps[2], m->sps[3]);
    } else if (ntype == 8 && nal_len < sizeof(m->pps)) {
      memcpy(m->pps, nal, nal_len);
      m->pps_len = (int)nal_len;
    } else if (ntype == 5 || ntype == 1) {
      if (out_len + 4 + nal_len > out_cap) {
        return 0;
      }
      w32(out + out_len, nal_len);
      memcpy(out + out_len + 4, nal, nal_len);
      out_len += 4 + nal_len;
      *got_vcl = 1;
      if (ntype == 5) {
        *is_idr = 1;
      }
    }
    pos = npos;
    sc = nsc;
  }
  if (m->sps_len > 0 && m->pps_len > 0) {
    m->ready = 1;
  }
  return out_len;
}

static uint32_t build_init(Fmp4Mux *m, uint8_t *o, uint32_t cap)
{
  uint32_t p = 0;
  uint32_t moov, trak, mdia, minf, stbl, stsd, avc1, avcC, mvex;
  uint32_t avcc_payload;

  if (cap < 800) {
    return 0;
  }

  /* ftyp */
  w32(o + p, 24);
  p += 4;
  memcpy(o + p, "ftyp", 4);
  p += 4;
  memcpy(o + p, "isom", 4);
  p += 4;
  w32(o + p, 0x200);
  p += 4;
  memcpy(o + p, "isom", 4);
  p += 4;
  memcpy(o + p, "iso5", 4);
  p += 4;

  moov = p;
  w32(o + p, 0);
  p += 4;
  memcpy(o + p, "moov", 4);
  p += 4;

  /* mvhd 108 */
  w32(o + p, 108);
  p += 4;
  memcpy(o + p, "mvhd", 4);
  p += 4;
  memset(o + p, 0, 100);
  w32(o + p + 12, 1000);      /* timescale */
  w32(o + p + 20, 0x00010000); /* rate */
  w16(o + p + 24, 0x0100);    /* volume */
  w32(o + p + 36, 0x00010000);
  w32(o + p + 52, 0x00010000);
  w32(o + p + 68, 0x40000000);
  w32(o + p + 96, 2); /* next_track_ID */
  p += 100;

  trak = p;
  w32(o + p, 0);
  p += 4;
  memcpy(o + p, "trak", 4);
  p += 4;

  /* tkhd 92 */
  w32(o + p, 92);
  p += 4;
  memcpy(o + p, "tkhd", 4);
  p += 4;
  memset(o + p, 0, 84);
  w32(o + p, 0x00000007);
  w32(o + p + 12, 1);
  w32(o + p + 40, 0x00010000);
  w32(o + p + 56, 0x00010000);
  w32(o + p + 72, 0x40000000);
  w32(o + p + 76, (uint32_t)m->width << 16);
  w32(o + p + 80, (uint32_t)m->height << 16);
  p += 84;

  mdia = p;
  w32(o + p, 0);
  p += 4;
  memcpy(o + p, "mdia", 4);
  p += 4;

  /* mdhd 32 */
  w32(o + p, 32);
  p += 4;
  memcpy(o + p, "mdhd", 4);
  p += 4;
  memset(o + p, 0, 24);
  w32(o + p + 12, 90000);
  w32(o + p + 20, 0x55c40000);
  p += 24;

  /* hdlr 33+ */
  w32(o + p, 33);
  p += 4;
  memcpy(o + p, "hdlr", 4);
  p += 4;
  memset(o + p, 0, 8);
  p += 8;
  memcpy(o + p, "vide", 4);
  p += 4;
  memset(o + p, 0, 12);
  p += 12;
  o[p++] = 0;

  minf = p;
  w32(o + p, 0);
  p += 4;
  memcpy(o + p, "minf", 4);
  p += 4;

  /* vmhd */
  w32(o + p, 20);
  p += 4;
  memcpy(o + p, "vmhd", 4);
  p += 4;
  w32(o + p, 1);
  p += 4;
  memset(o + p, 0, 8);
  p += 8;

  /* dinf/dref/url */
  w32(o + p, 36);
  p += 4;
  memcpy(o + p, "dinf", 4);
  p += 4;
  w32(o + p, 28);
  p += 4;
  memcpy(o + p, "dref", 4);
  p += 4;
  w32(o + p, 0);
  p += 4;
  w32(o + p, 1);
  p += 4;
  w32(o + p, 12);
  p += 4;
  memcpy(o + p, "url ", 4);
  p += 4;
  w32(o + p, 1);
  p += 4;

  stbl = p;
  w32(o + p, 0);
  p += 4;
  memcpy(o + p, "stbl", 4);
  p += 4;

  stsd = p;
  w32(o + p, 0);
  p += 4;
  memcpy(o + p, "stsd", 4);
  p += 4;
  w32(o + p, 0);
  p += 4;
  w32(o + p, 1);
  p += 4;

  avc1 = p;
  w32(o + p, 0);
  p += 4;
  memcpy(o + p, "avc1", 4);
  p += 4;
  memset(o + p, 0, 6);
  p += 6;
  w16(o + p, 1);
  p += 2;
  memset(o + p, 0, 16);
  p += 16;
  w16(o + p, (uint16_t)m->width);
  p += 2;
  w16(o + p, (uint16_t)m->height);
  p += 2;
  w32(o + p, 0x00480000);
  p += 4;
  w32(o + p, 0x00480000);
  p += 4;
  w32(o + p, 0);
  p += 4;
  w16(o + p, 1);
  p += 2;
  memset(o + p, 0, 32);
  p += 32;
  w16(o + p, 0x0018);
  p += 2;
  w16(o + p, 0xffff);
  p += 2;

  avcc_payload = 6 + 2 + (uint32_t)m->sps_len + 1 + 2 + (uint32_t)m->pps_len;
  avcC = p;
  w32(o + p, 8 + avcc_payload);
  p += 4;
  memcpy(o + p, "avcC", 4);
  p += 4;
  o[p++] = 1;
  o[p++] = m->sps[1];
  o[p++] = m->sps[2];
  o[p++] = m->sps[3];
  o[p++] = 0xff;
  o[p++] = 0xe1;
  w16(o + p, (uint16_t)m->sps_len);
  p += 2;
  memcpy(o + p, m->sps, (size_t)m->sps_len);
  p += (uint32_t)m->sps_len;
  o[p++] = 1;
  w16(o + p, (uint16_t)m->pps_len);
  p += 2;
  memcpy(o + p, m->pps, (size_t)m->pps_len);
  p += (uint32_t)m->pps_len;
  w32(o + avcC, p - avcC);
  w32(o + avc1, p - avc1);
  w32(o + stsd, p - stsd);

  /* empty sample tables */
  w32(o + p, 16);
  p += 4;
  memcpy(o + p, "stts", 4);
  p += 4;
  w32(o + p, 0);
  p += 4;
  w32(o + p, 0);
  p += 4;
  w32(o + p, 16);
  p += 4;
  memcpy(o + p, "stsc", 4);
  p += 4;
  w32(o + p, 0);
  p += 4;
  w32(o + p, 0);
  p += 4;
  w32(o + p, 20);
  p += 4;
  memcpy(o + p, "stsz", 4);
  p += 4;
  w32(o + p, 0);
  p += 4;
  w32(o + p, 0);
  p += 4;
  w32(o + p, 0);
  p += 4;
  w32(o + p, 16);
  p += 4;
  memcpy(o + p, "stco", 4);
  p += 4;
  w32(o + p, 0);
  p += 4;
  w32(o + p, 0);
  p += 4;

  w32(o + stbl, p - stbl);
  w32(o + minf, p - minf);
  w32(o + mdia, p - mdia);
  w32(o + trak, p - trak);

  mvex = p;
  w32(o + p, 0);
  p += 4;
  memcpy(o + p, "mvex", 4);
  p += 4;
  w32(o + p, 32);
  p += 4;
  memcpy(o + p, "trex", 4);
  p += 4;
  w32(o + p, 0);
  p += 4;
  w32(o + p, 1);
  p += 4;
  w32(o + p, 1);
  p += 4;
  w32(o + p, 0);
  p += 4;
  w32(o + p, 0);
  p += 4;
  w32(o + p, 0);
  p += 4;
  w32(o + mvex, p - mvex);

  w32(o + moov, p - moov);
  return p;
}

static uint32_t build_frag(Fmp4Mux *m, uint8_t *o, uint32_t cap, const uint8_t *avcc,
                           uint32_t avcc_len, int key, uint64_t dts90)
{
  uint32_t p = 0;
  uint32_t moof, traf, trun, mdat;
  uint32_t dur = 90000 / 15; /* assume 15fps */
  uint32_t data_offset;

  if (cap < avcc_len + 256) {
    return 0;
  }

  moof = p;
  w32(o + p, 0);
  p += 4;
  memcpy(o + p, "moof", 4);
  p += 4;

  /* mfhd */
  w32(o + p, 16);
  p += 4;
  memcpy(o + p, "mfhd", 4);
  p += 4;
  w32(o + p, 0);
  p += 4;
  w32(o + p, m->seq++);
  p += 4;

  traf = p;
  w32(o + p, 0);
  p += 4;
  memcpy(o + p, "traf", 4);
  p += 4;

  /* tfhd */
  w32(o + p, 16);
  p += 4;
  memcpy(o + p, "tfhd", 4);
  p += 4;
  w32(o + p, 0x00020000); /* default-base-is-moof */
  p += 4;
  w32(o + p, 1);
  p += 4;

  /* tfdt version 1 */
  w32(o + p, 20);
  p += 4;
  memcpy(o + p, "tfdt", 4);
  p += 4;
  w32(o + p, 0x01000000);
  p += 4;
  w64(o + p, dts90);
  p += 8;

  trun = p;
  w32(o + p, 0);
  p += 4;
  memcpy(o + p, "trun", 4);
  p += 4;
  /* data-offset, sample-duration, sample-size, sample-flags */
  w32(o + p, 0x00000105);
  p += 4;
  w32(o + p, 1);
  p += 4;
  data_offset = p; /* fill later: offset from moof start to mdat payload */
  w32(o + p, 0);
  p += 4;
  w32(o + p, dur);
  p += 4;
  w32(o + p, avcc_len);
  p += 4;
  w32(o + p, key ? 0x02000000 : 0x01010000);
  p += 4;
  w32(o + trun, p - trun);
  w32(o + traf, p - traf);
  w32(o + moof, p - moof);

  mdat = p;
  w32(o + p, 8 + avcc_len);
  p += 4;
  memcpy(o + p, "mdat", 4);
  p += 4;
  w32(o + data_offset, p - moof);
  memcpy(o + p, avcc, avcc_len);
  p += avcc_len;
  (void)mdat;
  return p;
}

int fmp4_mux_push(Fmp4Mux *m, const uint8_t *data, uint32_t len, int key_frame, int64_t pts_us)
{
  uint8_t *avcc;
  uint32_t avcc_cap;
  uint32_t avcc_len;
  int got_vcl = 0, is_idr = 0;
  uint32_t frag_len;
  uint64_t dts90;

  if (!m || !m->scratch || !data || len == 0) {
    return -1;
  }

  avcc = m->scratch;
  avcc_cap = m->scratch_cap / 2;
  if (avcc_cap < 64 * 1024) {
    avcc_cap = m->scratch_cap / 2;
  }
  avcc_len = annexb_to_avcc(data, len, avcc, avcc_cap, m, &got_vcl, &is_idr);
  if (!m->ready) {
    return 0;
  }

  if (!m->codec[0] && m->sps_len >= 4) {
    snprintf(m->codec, sizeof(m->codec), "avc1.%02X%02X%02X", m->sps[1], m->sps[2], m->sps[3]);
  }

  if (m->emit && !m->init_sent && (key_frame || is_idr)) {
    uint8_t *init = m->scratch + avcc_cap;
    uint32_t init_cap = m->scratch_cap - avcc_cap;
    uint32_t init_len = build_init(m, init, init_cap);
    if (init_len > 0) {
      m->emit(init, init_len, 1, 1, m->emit_user);
      m->init_sent = 1;
    }
  }

  if (!got_vcl || avcc_len == 0) {
    return 0;
  }
  if (!m->has_base) {
    if (!(key_frame || is_idr)) {
      return 0;
    }
    m->base_pts_us = pts_us;
    m->has_base = 1;
  }

  dts90 = (uint64_t)((pts_us - m->base_pts_us) * 90 / 1000);
  {
    int key = key_frame || is_idr;
    uint8_t *frag = m->scratch + avcc_cap;
    uint32_t frag_cap = m->scratch_cap - avcc_cap;
    uint8_t *avcc_copy = m->scratch;
    frag_len = build_frag(m, frag, frag_cap, avcc_copy, avcc_len, key, dts90);
    if (frag_len > 0 && m->emit) {
      m->emit(frag, frag_len, 0, key, m->emit_user);
    }
  }
  return 0;
}
