#ifndef IPC_WEB_FMP4_MUX_H
#define IPC_WEB_FMP4_MUX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* is_init: ftyp+moov; is_key meaningful for fragments only. */
typedef void (*Fmp4EmitCb)(const uint8_t *data, uint32_t len, int is_init, int is_key, void *user);

typedef struct {
  uint8_t sps[256];
  int sps_len;
  uint8_t pps[128];
  int pps_len;
  int width;
  int height;
  int ready; /* SPS+PPS seen */
  int init_sent;
  uint32_t seq;
  int64_t base_pts_us;
  int has_base;
  char codec[32];
  Fmp4EmitCb emit;
  void *emit_user;
  uint8_t *scratch;
  uint32_t scratch_cap;
} Fmp4Mux;

int fmp4_mux_init(Fmp4Mux *m, Fmp4EmitCb emit, void *user);
void fmp4_mux_reset(Fmp4Mux *m);
void fmp4_mux_deinit(Fmp4Mux *m);

/* Push one VENC Annex-B access unit (may contain multiple NALs). */
int fmp4_mux_push(Fmp4Mux *m, const uint8_t *data, uint32_t len, int key_frame, int64_t pts_us);

#ifdef __cplusplus
}
#endif

#endif
