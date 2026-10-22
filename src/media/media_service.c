#include "media/media_service.h"
#include "media/media_osd.h"

#include "common/log.h"
#include "system/config_service.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MEDIA_JPEG_VENC_CHN 2
#define MEDIA_JPEG_MASTER_CHN 1
/* Second JPEG slave sharing frames with the record main encoder (VENC0),
 * so a main-stream snapshot really is main_w x main_h. */
#define MEDIA_JPEG_MAIN_VENC_CHN 4
#define MEDIA_JPEG_MAIN_MASTER_CHN 0
#define MEDIA_PREVIEW_MAIN_CHN 3
#define MEDIA_ALIGN2(x) (((x) + 1) & ~1)

#include <rk_aiq_user_api2_sysctl.h>
#include <rk_aiq_user_api2_imgproc.h>

#include "rk_comm_venc.h"
#include "rk_debug.h"
#include "rk_defines.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_cal.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_vi.h"

#include "rga/im2d.h"
#include "rga/rga.h"

typedef struct {
  int inited;
  int width;
  int height;
  int vi_chn;
  char iq_dir[256];
  rk_aiq_sys_ctx_t *aiq_ctx;
} MediaCtx;

#define MEDIA_MAX_LISTENERS 4

typedef struct {
  MediaPacketCb cb;
  void *user;
} MediaListener;

typedef struct {
  int up;
  int pipeline_up;
  int vi2_up;
  int jpeg_up;
  int jpeg_main_up;
  int threads_running;
  MediaEncodeConfig cfg;
  char iq_dir[256];
  volatile int quit;
  pthread_t t0, t1, t3;
  int got_main;
  int got_sub;
  int got_preview_main;
  int preview_main_w;
  int preview_main_h;
  int preview_main_vi; /* VI channel feeding preview-main (0=main, 2=fallback) */
  int preview_main_thread;
  int preview_main_active;
  FILE *fp_main;
  FILE *fp_sub;
  MediaListener listeners[MEDIA_MAX_LISTENERS];
  int listener_n;
  pthread_mutex_t lock;
} StreamCtx;

typedef struct {
  int chn;
  int is_h265;
} StreamThreadArg;

static MediaCtx g_media;
static StreamCtx g_stream = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};
static pthread_mutex_t g_snap_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_quit;
static int g_frame_target;
static int g_frame_got;
static int g_aiq_paused;

static int media_image_apply_cfg(const MediaImageConfig *cfg);

static XCamReturn media_aiq_err_cb(rk_aiq_err_msg_t *msg)
{
  if (msg && msg->err_code == XCAM_RETURN_BYPASS) {
    log_warn("media", "aiq bypass err");
  }
  return XCAM_RETURN_NO_ERROR;
}

static XCamReturn media_aiq_sof_cb(rk_aiq_metas_t *meta)
{
  (void)meta;
  return XCAM_RETURN_NO_ERROR;
}

static int media_aiq_start(const char *iq_dir)
{
  rk_aiq_static_info_t info;
  memset(&info, 0, sizeof(info));
  if (rk_aiq_uapi2_sysctl_enumStaticMetas(0, &info) != 0) {
    log_error("media", "enumStaticMetas failed");
    return -1;
  }
  log_info("media", "sensor=%s iq=%s", info.sensor_info.sensor_name, iq_dir);

  setenv("HDR_MODE", "0", 1);
  g_media.aiq_ctx =
      rk_aiq_uapi2_sysctl_init(info.sensor_info.sensor_name, iq_dir, media_aiq_err_cb,
                               media_aiq_sof_cb);
  if (!g_media.aiq_ctx) {
    log_error("media", "aiq init failed");
    return -2;
  }
  if (rk_aiq_uapi2_sysctl_prepare(g_media.aiq_ctx, 0, 0, RK_AIQ_WORKING_MODE_NORMAL)) {
    log_error("media", "aiq prepare failed");
    rk_aiq_uapi2_sysctl_deinit(g_media.aiq_ctx);
    g_media.aiq_ctx = NULL;
    return -3;
  }
  if (rk_aiq_uapi2_sysctl_start(g_media.aiq_ctx)) {
    log_error("media", "aiq start failed");
    rk_aiq_uapi2_sysctl_deinit(g_media.aiq_ctx);
    g_media.aiq_ctx = NULL;
    return -4;
  }
  log_info("media", "aiq start ok");
  {
    MediaImageConfig img;
    if (media_image_get(&img) == 0) {
      media_image_apply_cfg(&img);
    }
  }
  return 0;
}

static void media_aiq_stop(void)
{
  if (!g_media.aiq_ctx) {
    return;
  }
  rk_aiq_uapi2_sysctl_stop(g_media.aiq_ctx, false);
  rk_aiq_uapi2_sysctl_deinit(g_media.aiq_ctx);
  g_media.aiq_ctx = NULL;
  g_aiq_paused = 0;
}

static int media_vi_dev_init(void)
{
  int ret;
  int dev_id = 0;
  VI_DEV_ATTR_S attr;
  VI_DEV_BIND_PIPE_S bind;
  memset(&attr, 0, sizeof(attr));
  memset(&bind, 0, sizeof(bind));

  ret = RK_MPI_VI_GetDevAttr(dev_id, &attr);
  if (ret == RK_ERR_VI_NOT_CONFIG) {
    ret = RK_MPI_VI_SetDevAttr(dev_id, &attr);
    if (ret != RK_SUCCESS) {
      log_error("media", "SetDevAttr %x", ret);
      return -1;
    }
  }

  ret = RK_MPI_VI_GetDevIsEnable(dev_id);
  if (ret != RK_SUCCESS) {
    ret = RK_MPI_VI_EnableDev(dev_id);
    if (ret != RK_SUCCESS) {
      log_error("media", "EnableDev %x", ret);
      return -2;
    }
    bind.u32Num = 1;
    bind.PipeId[0] = dev_id;
    ret = RK_MPI_VI_SetDevBindPipe(dev_id, &bind);
    if (ret != RK_SUCCESS) {
      log_error("media", "SetDevBindPipe %x", ret);
      return -3;
    }
  }
  return 0;
}

/* Sensor/ISP typically delivers ~30fps; VI dst + VENC src/dst must match config
 * or encoder accepts all frames while PTS advances at configured fps → MP4 duration
 * ≈ wall * (actual_fps / cfg_fps). */
#ifndef MEDIA_SENSOR_FPS
#define MEDIA_SENSOR_FPS 30
#endif

static int media_vi_chn_init(int chn, int width, int height, int depth, int dst_fps)
{
  VI_CHN_ATTR_S attr;
  memset(&attr, 0, sizeof(attr));
  /* VI0 feeds the record encoder plus preview-main; VI1/VI2 feed VENC+JPEG
   * combo+face grab. All three need a deeper buffer pool than a single
   * consumer would, otherwise a 1080p VI can starve and stop delivering. */
  attr.stIspOpt.u32BufCount = 6;
  attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
  attr.stSize.u32Width = (RK_U32)width;
  attr.stSize.u32Height = (RK_U32)height;
  attr.enPixelFormat = RK_FMT_YUV420SP;
  attr.enCompressMode = COMPRESS_MODE_NONE;
  attr.u32Depth = (RK_U32)depth;
  if (dst_fps > 0) {
    attr.stFrameRate.s32SrcFrameRate = MEDIA_SENSOR_FPS;
    attr.stFrameRate.s32DstFrameRate = dst_fps;
  } else {
    attr.stFrameRate.s32SrcFrameRate = -1;
    attr.stFrameRate.s32DstFrameRate = -1;
  }

  int ret = RK_MPI_VI_SetChnAttr(0, chn, &attr);
  ret |= RK_MPI_VI_EnableChn(0, chn);
  if (ret) {
    log_error("media", "VI chn%d init fail %x", chn, ret);
    return ret;
  }
  return 0;
}

static int media_venc_init(int chn, int width, int height, RK_CODEC_ID_E type, int bitrate_kbps,
                           int gop, int fps)
{
  VENC_CHN_ATTR_S attr;
  VENC_RECV_PIC_PARAM_S recv;
  int src_fps = MEDIA_SENSOR_FPS;
  memset(&attr, 0, sizeof(attr));
  memset(&recv, 0, sizeof(recv));

  if (fps <= 0) {
    fps = 15;
  }
  if (src_fps < fps) {
    src_fps = fps;
  }
  /* Src > Dst makes VENC drop; Src==Dst previously let all sensor frames
   * through while PTS advanced at cfg fps → MP4 duration ≈ 2× wall. */

  if (type == RK_VIDEO_ID_AVC) {
    attr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
    attr.stRcAttr.stH264Cbr.u32BitRate = (RK_U32)bitrate_kbps;
    attr.stRcAttr.stH264Cbr.u32Gop = (RK_U32)gop;
    attr.stRcAttr.stH264Cbr.u32SrcFrameRateNum = (RK_U32)src_fps;
    attr.stRcAttr.stH264Cbr.u32SrcFrameRateDen = 1;
    attr.stRcAttr.stH264Cbr.fr32DstFrameRateNum = (RK_U32)fps;
    attr.stRcAttr.stH264Cbr.fr32DstFrameRateDen = 1;
    attr.stVencAttr.u32Profile = H264E_PROFILE_HIGH;
  } else {
    attr.stRcAttr.enRcMode = VENC_RC_MODE_H265CBR;
    attr.stRcAttr.stH265Cbr.u32BitRate = (RK_U32)bitrate_kbps;
    attr.stRcAttr.stH265Cbr.u32Gop = (RK_U32)gop;
    attr.stRcAttr.stH265Cbr.u32SrcFrameRateNum = (RK_U32)src_fps;
    attr.stRcAttr.stH265Cbr.u32SrcFrameRateDen = 1;
    attr.stRcAttr.stH265Cbr.fr32DstFrameRateNum = (RK_U32)fps;
    attr.stRcAttr.stH265Cbr.fr32DstFrameRateDen = 1;
  }

  attr.stVencAttr.enType = type;
  attr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
  attr.stVencAttr.u32PicWidth = (RK_U32)width;
  attr.stVencAttr.u32PicHeight = (RK_U32)height;
  attr.stVencAttr.u32VirWidth = (RK_U32)width;
  attr.stVencAttr.u32VirHeight = (RK_U32)height;
  attr.stVencAttr.u32StreamBufCnt = 4;
  attr.stVencAttr.u32BufSize = (RK_U32)(width * height * 3 / 2);
  attr.stVencAttr.enMirror = MIRROR_NONE;

  int ret = RK_MPI_VENC_CreateChn(chn, &attr);
  if (ret != RK_SUCCESS) {
    log_error("media", "VENC_CreateChn %d fail %x", chn, ret);
    return ret;
  }
  recv.s32RecvPicNum = -1;
  ret = RK_MPI_VENC_StartRecvFrame(chn, &recv);
  if (ret != RK_SUCCESS) {
    log_error("media", "VENC_StartRecvFrame %d fail %x", chn, ret);
    return ret;
  }
  return 0;
}

static int media_bind_vi_venc(int vi_chn, int venc_chn)
{
  MPP_CHN_S src = {.enModId = RK_ID_VI, .s32DevId = 0, .s32ChnId = vi_chn};
  MPP_CHN_S dst = {.enModId = RK_ID_VENC, .s32DevId = 0, .s32ChnId = venc_chn};
  int ret = RK_MPI_SYS_Bind(&src, &dst);
  if (ret != RK_SUCCESS) {
    log_error("media", "bind VI%d->VENC%d fail %x", vi_chn, venc_chn, ret);
  }
  return ret;
}

static void media_unbind_vi_venc(int vi_chn, int venc_chn)
{
  MPP_CHN_S src = {.enModId = RK_ID_VI, .s32DevId = 0, .s32ChnId = vi_chn};
  MPP_CHN_S dst = {.enModId = RK_ID_VENC, .s32DevId = 0, .s32ChnId = venc_chn};
  RK_MPI_SYS_UnBind(&src, &dst);
}

static void media_preview_main_size(const MediaEncodeConfig *cfg, int *w, int *h)
{
  int mw = cfg && cfg->main_w > 0 ? cfg->main_w : 1920;
  int mh = cfg && cfg->main_h > 0 ? cfg->main_h : 1080;
  /* VI2 is shared with IVS/face grab: keep it at ≤1280x720, which is what
   * the ISP third path reliably sustains on RV1106. */
  if (mw > 1280) {
    mh = mh * 1280 / mw;
    mw = 1280;
  }
  if (mh > 720) {
    mw = mw * 720 / mh;
    mh = 720;
  }
  *w = MEDIA_ALIGN2(mw);
  *h = MEDIA_ALIGN2(mh);
  if (*w < 320) {
    *w = 320;
  }
  if (*h < 240) {
    *h = 240;
  }
}

static int media_preview_main_init(const MediaEncodeConfig *cfg)
{
  int w = 0;
  int h = 0;
  int br;
  int gop;
  int fps;
  int vi_chn;
  /* Preferred: hang preview-main off VI0 so the live view has exactly the
   * recorded main geometry (H.264 because browsers cannot decode HEVC).
   * A second encoder only fits within the VEPU/MMZ budget up to ~1.4MP, so
   * larger mains fall back to VI2 at the capped 720p-class size.
   * VI2 is shared with IVS/face grab, hence not the default. */
  w = cfg && cfg->main_w > 0 ? cfg->main_w : 1920;
  h = cfg && cfg->main_h > 0 ? cfg->main_h : 1080;
  w = MEDIA_ALIGN2(w);
  h = MEDIA_ALIGN2(h);
  vi_chn = 0;
  if (w <= 0 || h <= 0 || w > 1920 || w * h > 1920 * 1080) {
    media_preview_main_size(cfg, &w, &h);
    vi_chn = 2;
  }
  /* Live view does not need the record bitrate, but at main resolution a
   * 720p-sized budget would look mushy; keep it within 800..1536 kbps. */
  br = 800;
  if (cfg && cfg->main_bitrate_kbps > br) {
    br = cfg->main_bitrate_kbps > 1536 ? 1536 : cfg->main_bitrate_kbps;
  }
  gop = cfg && cfg->main_gop > 0 ? cfg->main_gop : 20;
  fps = 10;
  if (cfg && cfg->main_fps > 0 && cfg->main_fps < fps) {
    fps = cfg->main_fps;
  }
  if (!g_stream.vi2_up) {
    log_warn("media", "preview-main needs VI2");
    return -1;
  }
  if (media_venc_init(MEDIA_PREVIEW_MAIN_CHN, w, h, RK_VIDEO_ID_AVC, br, gop, fps) != 0) {
    return -2;
  }
  if (media_bind_vi_venc(vi_chn, MEDIA_PREVIEW_MAIN_CHN) != 0) {
    if (vi_chn == 0) {
      /* Some MPI builds refuse a second VENC on VI0; fall back to VI2. */
      log_warn("media", "preview-main VI0 bind failed, falling back to VI2");
      RK_MPI_VENC_StopRecvFrame(MEDIA_PREVIEW_MAIN_CHN);
      RK_MPI_VENC_DestroyChn(MEDIA_PREVIEW_MAIN_CHN);
      media_preview_main_size(cfg, &w, &h);
      vi_chn = 2;
      if (media_venc_init(MEDIA_PREVIEW_MAIN_CHN, w, h, RK_VIDEO_ID_AVC, br, gop, fps) != 0) {
        return -2;
      }
      if (media_bind_vi_venc(vi_chn, MEDIA_PREVIEW_MAIN_CHN) != 0) {
        RK_MPI_VENC_StopRecvFrame(MEDIA_PREVIEW_MAIN_CHN);
        RK_MPI_VENC_DestroyChn(MEDIA_PREVIEW_MAIN_CHN);
        return -3;
      }
    } else {
      RK_MPI_VENC_StopRecvFrame(MEDIA_PREVIEW_MAIN_CHN);
      RK_MPI_VENC_DestroyChn(MEDIA_PREVIEW_MAIN_CHN);
      return -3;
    }
  }
  /* VI0 path stays receiving: stopping a VENC bound to VI0 starves the VI
   * channel and kills the record stream too. The VI2 fallback is gated. */
  if (vi_chn == 2) {
    RK_MPI_VENC_StopRecvFrame(MEDIA_PREVIEW_MAIN_CHN);
  }
  g_stream.preview_main_active = 0;
  g_stream.preview_main_vi = vi_chn;
  g_stream.preview_main_w = w;
  g_stream.preview_main_h = h;
  log_info("media", "preview-main VI%d->venc%d %dx%d %dkbps@%dfps (%s)", vi_chn,
           MEDIA_PREVIEW_MAIN_CHN, w, h, br, fps, vi_chn == 0 ? "drain" : "idle");
  return 0;
}

static void media_preview_main_destroy(void)
{
  if (g_stream.preview_main_w <= 0) {
    return;
  }
  media_unbind_vi_venc(g_stream.preview_main_vi, MEDIA_PREVIEW_MAIN_CHN);
  RK_MPI_VENC_StopRecvFrame(MEDIA_PREVIEW_MAIN_CHN);
  RK_MPI_VENC_DestroyChn(MEDIA_PREVIEW_MAIN_CHN);
  g_stream.preview_main_w = 0;
  g_stream.preview_main_h = 0;
  g_stream.preview_main_active = 0;
  g_stream.preview_main_vi = 0;
}

/* RV1106 combo JPEG: slave VENC shares frames with a master H.264/H.265 chn
 * (no VI GetChnFrame needed). master=1 -> sub size, master=0 -> main size. */
static int jpeg_combo_create(int chn, int master, int width, int height)
{
  VENC_CHN_ATTR_S attr;
  VENC_RECV_PIC_PARAM_S recv;
  VENC_COMBO_ATTR_S combo;
  VENC_JPEG_PARAM_S jpeg;
  int ret;

  if (width <= 0 || height <= 0) {
    return -1;
  }

  memset(&attr, 0, sizeof(attr));
  attr.stVencAttr.enType = RK_VIDEO_ID_JPEG;
  attr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
  attr.stVencAttr.u32MaxPicWidth = (RK_U32)width;
  attr.stVencAttr.u32MaxPicHeight = (RK_U32)height;
  attr.stVencAttr.u32PicWidth = (RK_U32)width;
  attr.stVencAttr.u32PicHeight = (RK_U32)height;
  attr.stVencAttr.u32VirWidth = (RK_U32)MEDIA_ALIGN2(width);
  attr.stVencAttr.u32VirHeight = (RK_U32)MEDIA_ALIGN2(height);
  attr.stVencAttr.u32StreamBufCnt = 2;
  attr.stVencAttr.u32BufSize = (RK_U32)(width * height / 2);
  attr.stVencAttr.stAttrJpege.bSupportDCF = RK_FALSE;
  attr.stVencAttr.stAttrJpege.stMPFCfg.u8LargeThumbNailNum = 0;
  attr.stVencAttr.stAttrJpege.enReceiveMode = VENC_PIC_RECEIVE_SINGLE;
  attr.stGopAttr.enGopMode = VENC_GOPMODE_INIT;

  ret = RK_MPI_VENC_CreateChn(chn, &attr);
  if (ret != RK_SUCCESS) {
    log_error("media", "JPEG VENC CreateChn chn=%d fail %x", chn, ret);
    return ret;
  }

  memset(&jpeg, 0, sizeof(jpeg));
  jpeg.u32Qfactor = 90;
  ret = RK_MPI_VENC_SetJpegParam(chn, &jpeg);
  if (ret != RK_SUCCESS) {
    log_warn("media", "SetJpegParam fail %x", ret);
  }

  /* Match SDK sample order: StartRecvFrame then SetComboAttr, then idle. */
  memset(&recv, 0, sizeof(recv));
  recv.s32RecvPicNum = 1;
  ret = RK_MPI_VENC_StartRecvFrame(chn, &recv);
  if (ret != RK_SUCCESS) {
    log_error("media", "JPEG StartRecvFrame chn=%d fail %x", chn, ret);
    RK_MPI_VENC_DestroyChn(chn);
    return ret;
  }

  memset(&combo, 0, sizeof(combo));
  combo.bEnable = RK_TRUE;
  combo.s32ChnId = master;
  ret = RK_MPI_VENC_SetComboAttr(chn, &combo);
  if (ret != RK_SUCCESS) {
    log_error("media", "JPEG SetComboAttr chn=%d master=%d fail %x", chn, master, ret);
    RK_MPI_VENC_StopRecvFrame(chn);
    RK_MPI_VENC_DestroyChn(chn);
    return ret;
  }

  RK_MPI_VENC_StopRecvFrame(chn);
  log_info("media", "JPEG combo venc%d master=venc%d %dx%d", chn, master, width, height);
  return 0;
}

static void media_jpeg_venc_destroy(void)
{
  if (g_stream.jpeg_main_up) {
    RK_MPI_VENC_StopRecvFrame(MEDIA_JPEG_MAIN_VENC_CHN);
    RK_MPI_VENC_DestroyChn(MEDIA_JPEG_MAIN_VENC_CHN);
    g_stream.jpeg_main_up = 0;
  }
  if (g_stream.jpeg_up) {
    RK_MPI_VENC_StopRecvFrame(MEDIA_JPEG_VENC_CHN);
    RK_MPI_VENC_DestroyChn(MEDIA_JPEG_VENC_CHN);
    g_stream.jpeg_up = 0;
  }
}

static int pack_is_key(const VENC_PACK_S *pack, int is_h265)
{
  if (!pack) {
    return 0;
  }
  if (is_h265) {
    return pack->DataType.enH265EType == H265E_NALU_IDRSLICE ||
           pack->DataType.enH265EType == H265E_NALU_ISLICE;
  }
  return pack->DataType.enH264EType == H264E_NALU_IDRSLICE ||
         pack->DataType.enH264EType == H264E_NALU_ISLICE;
}

static void stream_copy_cfg(const MediaEncodeConfig *cfg)
{
  g_stream.cfg = *cfg;
  snprintf(g_stream.iq_dir, sizeof(g_stream.iq_dir), "%s",
           cfg->iq_dir && cfg->iq_dir[0] ? cfg->iq_dir : "/oem/usr/share/iqfiles");
  g_stream.cfg.iq_dir = g_stream.iq_dir;
  if (g_stream.cfg.detect_w < 0) {
    g_stream.cfg.detect_w = 0;
  }
  if (g_stream.cfg.detect_h < 0) {
    g_stream.cfg.detect_h = 0;
  }
}

static int stream_create_pipeline(const MediaEncodeConfig *cfg)
{
  int pw = 0;
  int ph = 0;
  int detect_wanted;

  /* VI unrestricted; VENC Src=sensor Dst=cfg drops to target fps. */
  /* VI1 depth>=3: sub VENC + JPEG combo + face GetChnFrame. */
  if (media_vi_chn_init(0, cfg->main_w, cfg->main_h, 0, 0) != 0 ||
      media_vi_chn_init(1, cfg->sub_w, cfg->sub_h, 3, 0) != 0) {
    return -1;
  }
  /* VI2 = ISP third path: IVS detect + optional preview-main + face grab (depth>=2). */
  media_preview_main_size(cfg, &pw, &ph);
  detect_wanted = (cfg->detect_w > 0 && cfg->detect_h > 0);
  g_stream.vi2_up = 0;
  if (media_vi_chn_init(2, pw, ph, 2, 0) != 0) {
    RK_MPI_VI_DisableChn(0, 0);
    RK_MPI_VI_DisableChn(0, 1);
    return -1;
  }
  g_stream.vi2_up = 1;
  if (detect_wanted) {
    /* IVS must match VI2 geometry (preview-main size ≤1280x720). */
    g_stream.cfg.detect_w = pw;
    g_stream.cfg.detect_h = ph;
  } else {
    g_stream.cfg.detect_w = 0;
    g_stream.cfg.detect_h = 0;
  }
  if (media_venc_init(0, cfg->main_w, cfg->main_h, RK_VIDEO_ID_HEVC, cfg->main_bitrate_kbps,
                      cfg->main_gop, cfg->main_fps) != 0 ||
      media_venc_init(1, cfg->sub_w, cfg->sub_h, RK_VIDEO_ID_AVC, cfg->sub_bitrate_kbps,
                      cfg->sub_gop, cfg->sub_fps) != 0) {
    if (g_stream.vi2_up) {
      RK_MPI_VI_DisableChn(0, 2);
      g_stream.vi2_up = 0;
    }
    RK_MPI_VI_DisableChn(0, 0);
    RK_MPI_VI_DisableChn(0, 1);
    return -2;
  }
  if (media_bind_vi_venc(0, 0) != 0 || media_bind_vi_venc(1, 1) != 0) {
    RK_MPI_VENC_StopRecvFrame(0);
    RK_MPI_VENC_DestroyChn(0);
    RK_MPI_VENC_StopRecvFrame(1);
    RK_MPI_VENC_DestroyChn(1);
    if (g_stream.vi2_up) {
      RK_MPI_VI_DisableChn(0, 2);
      g_stream.vi2_up = 0;
    }
    RK_MPI_VI_DisableChn(0, 0);
    RK_MPI_VI_DisableChn(0, 1);
    return -3;
  }
  if (media_preview_main_init(cfg) != 0) {
    log_warn("media", "preview-main venc init failed (sub preview only)");
  }
  if (jpeg_combo_create(MEDIA_JPEG_VENC_CHN, MEDIA_JPEG_MASTER_CHN, cfg->sub_w, cfg->sub_h) != 0) {
    media_preview_main_destroy();
    media_unbind_vi_venc(0, 0);
    media_unbind_vi_venc(1, 1);
    RK_MPI_VENC_StopRecvFrame(0);
    RK_MPI_VENC_DestroyChn(0);
    RK_MPI_VENC_StopRecvFrame(1);
    RK_MPI_VENC_DestroyChn(1);
    if (g_stream.vi2_up) {
      RK_MPI_VI_DisableChn(0, 2);
      g_stream.vi2_up = 0;
    }
    RK_MPI_VI_DisableChn(0, 0);
    RK_MPI_VI_DisableChn(0, 1);
    return -4;
  }
  g_stream.jpeg_up = 1;
  if (jpeg_combo_create(MEDIA_JPEG_MAIN_VENC_CHN, MEDIA_JPEG_MAIN_MASTER_CHN, cfg->main_w,
                        cfg->main_h) == 0) {
    g_stream.jpeg_main_up = 1;
  } else {
    log_warn("media", "main JPEG combo unavailable (main snapshot falls back to sub)");
  }
  g_stream.pipeline_up = 1;
  return 0;
}

static void stream_destroy_pipeline(int destroy_vi)
{
  if (!g_stream.pipeline_up && !destroy_vi) {
    return;
  }
  media_osd_stop();
  media_jpeg_venc_destroy();
  media_preview_main_destroy();
  media_unbind_vi_venc(0, 0);
  media_unbind_vi_venc(1, 1);
  RK_MPI_VENC_StopRecvFrame(0);
  RK_MPI_VENC_DestroyChn(0);
  RK_MPI_VENC_StopRecvFrame(1);
  RK_MPI_VENC_DestroyChn(1);
  if (destroy_vi) {
    if (g_stream.vi2_up) {
      RK_MPI_VI_DisableChn(0, 2);
      g_stream.vi2_up = 0;
    }
    RK_MPI_VI_DisableChn(0, 0);
    RK_MPI_VI_DisableChn(0, 1);
  }
  g_stream.pipeline_up = 0;
}

static void *media_get_frame_thread(void *arg)
{
  int chn = *(int *)arg;
  VIDEO_FRAME_INFO_S frame;
  memset(&frame, 0, sizeof(frame));

  while (!g_quit) {
    int ret = RK_MPI_VI_GetChnFrame(0, chn, &frame, 1000);
    if (ret == RK_SUCCESS) {
      g_frame_got++;
      if (g_frame_got <= 3 || (g_frame_got % 10) == 0) {
        log_info("media", "got frame #%d %ux%u", g_frame_got, frame.stVFrame.u32Width,
                 frame.stVFrame.u32Height);
      }
      RK_MPI_VI_ReleaseChnFrame(0, chn, &frame);
      if (g_frame_target >= 0 && g_frame_got >= g_frame_target) {
        g_quit = 1;
        break;
      }
    } else {
      log_warn("media", "GetChnFrame timeout/fail %x", ret);
    }
  }
  return NULL;
}

static void *media_stream_get_thread(void *arg)
{
  StreamThreadArg *a = (StreamThreadArg *)arg;
  VENC_STREAM_S st;
  memset(&st, 0, sizeof(st));
  st.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S));
  if (!st.pstPack) {
    return NULL;
  }

  while (!g_stream.quit) {
    int ret = RK_MPI_VENC_GetStream(a->chn, &st, 1000);
    if (ret != RK_SUCCESS) {
      continue;
    }
    void *data = RK_MPI_MB_Handle2VirAddr(st.pstPack->pMbBlk);
    uint32_t len = st.pstPack->u32Len;
    int key = pack_is_key(st.pstPack, a->is_h265);
    int64_t pts = (int64_t)st.pstPack->u64PTS;

    MediaListener local[MEDIA_MAX_LISTENERS];
    int n = 0;
    int i;
    pthread_mutex_lock(&g_stream.lock);
    FILE *fp = NULL;
    if (a->chn == 0) {
      fp = g_stream.fp_main;
      g_stream.got_main++;
    } else if (a->chn == 1) {
      fp = g_stream.fp_sub;
      g_stream.got_sub++;
    } else if (a->chn == MEDIA_PREVIEW_MAIN_CHN) {
      g_stream.got_preview_main++;
      if (g_stream.got_preview_main <= 3 || (g_stream.got_preview_main % 300) == 0) {
        log_info("media", "preview-main got #%d len=%u key=%d", g_stream.got_preview_main, len,
                 key);
      }
    }
    if (fp && data && len > 0) {
      fwrite(data, 1, len, fp);
    }
    n = g_stream.listener_n;
    if (n > MEDIA_MAX_LISTENERS) {
      n = MEDIA_MAX_LISTENERS;
    }
    memcpy(local, g_stream.listeners, (size_t)n * sizeof(MediaListener));
    pthread_mutex_unlock(&g_stream.lock);

    if (data && len > 0) {
      for (i = 0; i < n; i++) {
        if (local[i].cb) {
          local[i].cb(a->chn, (const uint8_t *)data, len, key, pts, local[i].user);
        }
      }
    }
    RK_MPI_VENC_ReleaseStream(a->chn, &st);
  }

  free(st.pstPack);
  return NULL;
}

static int stream_start_threads(void)
{
  static StreamThreadArg a0 = {.chn = 0, .is_h265 = 1};
  static StreamThreadArg a1 = {.chn = 1, .is_h265 = 0};
  static StreamThreadArg a3 = {.chn = MEDIA_PREVIEW_MAIN_CHN, .is_h265 = 0};
  g_stream.quit = 0;
  g_stream.got_main = 0;
  g_stream.got_sub = 0;
  g_stream.got_preview_main = 0;
  if (pthread_create(&g_stream.t0, NULL, media_stream_get_thread, &a0) != 0) {
    return -1;
  }
  if (pthread_create(&g_stream.t1, NULL, media_stream_get_thread, &a1) != 0) {
    g_stream.quit = 1;
    pthread_join(g_stream.t0, NULL);
    return -2;
  }
  if (g_stream.preview_main_w > 0) {
    if (pthread_create(&g_stream.t3, NULL, media_stream_get_thread, &a3) == 0) {
      g_stream.preview_main_thread = 1;
    } else {
      log_warn("media", "preview-main thread create failed");
    }
  }
  g_stream.threads_running = 1;
  return 0;
}

static void stream_prepare_stop_threads(void)
{
  g_stream.quit = 1;
  if (g_stream.pipeline_up) {
    RK_MPI_VENC_StopRecvFrame(0);
    RK_MPI_VENC_StopRecvFrame(1);
    if (g_stream.preview_main_w > 0 || g_stream.preview_main_thread) {
      RK_MPI_VENC_StopRecvFrame(MEDIA_PREVIEW_MAIN_CHN);
    }
  }
}

static void stream_stop_threads(void)
{
  if (!g_stream.threads_running) {
    return;
  }
  pthread_join(g_stream.t0, NULL);
  pthread_join(g_stream.t1, NULL);
  if (g_stream.preview_main_thread) {
    pthread_join(g_stream.t3, NULL);
    g_stream.preview_main_thread = 0;
  }
  g_stream.threads_running = 0;
}

static int cfg_res_changed(const MediaEncodeConfig *a, const MediaEncodeConfig *b)
{
  return a->main_w != b->main_w || a->main_h != b->main_h || a->sub_w != b->sub_w ||
         a->sub_h != b->sub_h || a->detect_w != b->detect_w || a->detect_h != b->detect_h;
}

int media_init(const MediaConfig *cfg)
{
  if (g_media.inited || g_stream.up) {
    return 0;
  }
  if (!cfg || cfg->width <= 0 || cfg->height <= 0) {
    return -1;
  }

  memset(&g_media, 0, sizeof(g_media));
  g_media.width = cfg->width;
  g_media.height = cfg->height;
  g_media.vi_chn = cfg->vi_chn;
  snprintf(g_media.iq_dir, sizeof(g_media.iq_dir), "%s",
           cfg->iq_dir && cfg->iq_dir[0] ? cfg->iq_dir : "/oem/usr/share/iqfiles");

  if (media_aiq_start(g_media.iq_dir) != 0) {
    return -2;
  }
  if (RK_MPI_SYS_Init() != RK_SUCCESS) {
    log_error("media", "RK_MPI_SYS_Init fail");
    media_aiq_stop();
    return -3;
  }
  if (media_vi_dev_init() != 0 ||
      media_vi_chn_init(g_media.vi_chn, g_media.width, g_media.height, 2, 0) != 0) {
    RK_MPI_SYS_Exit();
    media_aiq_stop();
    return -4;
  }

  g_media.inited = 1;
  log_info("media", "init ok %dx%d chn=%d", g_media.width, g_media.height, g_media.vi_chn);
  return 0;
}

void media_deinit(void)
{
  if (!g_media.inited) {
    media_aiq_stop();
    return;
  }
  RK_MPI_VI_DisableChn(0, g_media.vi_chn);
  RK_MPI_VI_DisableDev(0);
  RK_MPI_SYS_Exit();
  media_aiq_stop();
  g_media.inited = 0;
  log_info("media", "deinit ok");
}

int media_capture_nv12(const char *out_dir, const char *file_name, int frame_count)
{
  if (!g_media.inited) {
    return -1;
  }
  if (frame_count <= 0) {
    frame_count = 5;
  }

  const int skip = 15;
  g_quit = 0;
  g_frame_target = skip;
  g_frame_got = 0;

  pthread_t tid;
  int chn = g_media.vi_chn;
  if (pthread_create(&tid, NULL, media_get_frame_thread, &chn) != 0) {
    return -2;
  }
  int wait_sec = skip + 10;
  while (!g_quit && wait_sec-- > 0) {
    sleep(1);
  }
  g_quit = 1;
  pthread_join(tid, NULL);
  log_info("media", "warmup done discarded=%d", g_frame_got);

  VI_SAVE_FILE_INFO_S save;
  memset(&save, 0, sizeof(save));
  save.bCfg = RK_TRUE;
  snprintf(save.aFilePath, sizeof(save.aFilePath), "%s",
           out_dir && out_dir[0] ? out_dir : "/mnt/sdcard/");
  size_t n = strlen(save.aFilePath);
  if (n > 0 && save.aFilePath[n - 1] != '/') {
    if (n + 1 < sizeof(save.aFilePath)) {
      save.aFilePath[n] = '/';
      save.aFilePath[n + 1] = '\0';
    }
  }
  snprintf(save.aFileName, sizeof(save.aFileName), "%s",
           file_name && file_name[0] ? file_name : "capture_0.yuv");
  RK_MPI_VI_ChnSaveFile(0, g_media.vi_chn, &save);
  log_info("media", "saving to %s%s count=%d", save.aFilePath, save.aFileName, frame_count);

  g_quit = 0;
  g_frame_target = frame_count;
  g_frame_got = 0;
  if (pthread_create(&tid, NULL, media_get_frame_thread, &chn) != 0) {
    return -2;
  }
  wait_sec = frame_count + 10;
  while (!g_quit && wait_sec-- > 0) {
    sleep(1);
  }
  g_quit = 1;
  pthread_join(tid, NULL);

  save.bCfg = RK_FALSE;
  RK_MPI_VI_ChnSaveFile(0, g_media.vi_chn, &save);

  if (g_frame_got < 1) {
    log_error("media", "no frame captured");
    return -3;
  }
  log_info("media", "capture done frames=%d", g_frame_got);
  return 0;
}

int media_stream_is_up(void)
{
  return g_stream.up;
}

int media_stream_pack_count_main(void)
{
  return g_stream.got_main;
}

int media_stream_detect_enabled(void)
{
  return g_stream.up && g_stream.vi2_up && g_stream.cfg.detect_w > 0 && g_stream.cfg.detect_h > 0;
}

int media_stream_detect_width(void)
{
  return g_stream.cfg.detect_w;
}

int media_stream_detect_height(void)
{
  return g_stream.cfg.detect_h;
}

int media_stream_preview_main_active(void)
{
  return g_stream.up && g_stream.preview_main_w > 0 && g_stream.preview_main_active;
}

int media_stream_preview_main_set_active(int on)
{
  VENC_RECV_PIC_PARAM_S recv;

  on = on ? 1 : 0;
  if (!g_stream.up || g_stream.preview_main_w <= 0) {
    return -1;
  }
  if (g_stream.preview_main_vi == 0) {
    /* Bound to VI0: StopRecvFrame starves the VI channel, which also starves
     * the record encoder, so the channel keeps draining and only the IDR and
     * the active flag are toggled here. */
    if (!on) {
      if (g_stream.preview_main_active) {
        log_info("media", "preview-main idle (VI0 keeps draining)");
      }
      g_stream.preview_main_active = 0;
      return 0;
    }
    RK_MPI_VENC_RequestIDR(MEDIA_PREVIEW_MAIN_CHN, RK_TRUE);
    g_stream.preview_main_active = 1;
    log_info("media", "preview-main encode ON");
    return 0;
  }
  if (g_stream.preview_main_active == on) {
    return 0;
  }
  if (on) {
    memset(&recv, 0, sizeof(recv));
    recv.s32RecvPicNum = -1;
    if (RK_MPI_VENC_StartRecvFrame(MEDIA_PREVIEW_MAIN_CHN, &recv) != RK_SUCCESS) {
      log_warn("media", "preview-main StartRecvFrame failed");
      return -2;
    }
    RK_MPI_VENC_RequestIDR(MEDIA_PREVIEW_MAIN_CHN, RK_TRUE);
    g_stream.preview_main_active = 1;
    log_info("media", "preview-main encode ON");
  } else {
    RK_MPI_VENC_StopRecvFrame(MEDIA_PREVIEW_MAIN_CHN);
    g_stream.preview_main_active = 0;
    log_info("media", "preview-main encode OFF");
  }
  return 0;
}

int media_stream_add_packet_listener(MediaPacketCb cb, void *user)
{
  int i;
  if (!cb) {
    return -1;
  }
  pthread_mutex_lock(&g_stream.lock);
  for (i = 0; i < g_stream.listener_n; i++) {
    if (g_stream.listeners[i].cb == cb && g_stream.listeners[i].user == user) {
      pthread_mutex_unlock(&g_stream.lock);
      return 0;
    }
  }
  if (g_stream.listener_n >= MEDIA_MAX_LISTENERS) {
    pthread_mutex_unlock(&g_stream.lock);
    return -2;
  }
  g_stream.listeners[g_stream.listener_n].cb = cb;
  g_stream.listeners[g_stream.listener_n].user = user;
  g_stream.listener_n++;
  pthread_mutex_unlock(&g_stream.lock);
  return 0;
}

void media_stream_remove_packet_listener(MediaPacketCb cb, void *user)
{
  int i, j;
  pthread_mutex_lock(&g_stream.lock);
  for (i = 0; i < g_stream.listener_n; i++) {
    if (g_stream.listeners[i].cb == cb && g_stream.listeners[i].user == user) {
      for (j = i + 1; j < g_stream.listener_n; j++) {
        g_stream.listeners[j - 1] = g_stream.listeners[j];
      }
      g_stream.listener_n--;
      memset(&g_stream.listeners[g_stream.listener_n], 0, sizeof(MediaListener));
      break;
    }
  }
  pthread_mutex_unlock(&g_stream.lock);
}

void media_stream_set_packet_cb(MediaPacketCb cb, void *user)
{
  pthread_mutex_lock(&g_stream.lock);
  memset(g_stream.listeners, 0, sizeof(g_stream.listeners));
  g_stream.listener_n = 0;
  if (cb) {
    g_stream.listeners[0].cb = cb;
    g_stream.listeners[0].user = user;
    g_stream.listener_n = 1;
  }
  pthread_mutex_unlock(&g_stream.lock);
}

int media_stream_request_idr(int chn)
{
  if (!g_stream.up) {
    return -1;
  }
  return RK_MPI_VENC_RequestIDR(chn, RK_TRUE) == RK_SUCCESS ? 0 : -2;
}

int media_stream_set_dump(const char *main_path, const char *sub_path)
{
  FILE *fm = NULL;
  FILE *fs = NULL;
  if (main_path && main_path[0]) {
    fm = fopen(main_path, "wb");
    if (!fm) {
      log_error("media", "open dump %s failed", main_path);
      return -1;
    }
  }
  if (sub_path && sub_path[0]) {
    fs = fopen(sub_path, "wb");
    if (!fs) {
      if (fm) {
        fclose(fm);
      }
      log_error("media", "open dump %s failed", sub_path);
      return -2;
    }
  }
  pthread_mutex_lock(&g_stream.lock);
  if (g_stream.fp_main) {
    fflush(g_stream.fp_main);
    fclose(g_stream.fp_main);
  }
  if (g_stream.fp_sub) {
    fflush(g_stream.fp_sub);
    fclose(g_stream.fp_sub);
  }
  g_stream.fp_main = fm;
  g_stream.fp_sub = fs;
  pthread_mutex_unlock(&g_stream.lock);
  return 0;
}

void media_stream_clear_dump(void)
{
  pthread_mutex_lock(&g_stream.lock);
  if (g_stream.fp_main) {
    fflush(g_stream.fp_main);
    fclose(g_stream.fp_main);
    g_stream.fp_main = NULL;
  }
  if (g_stream.fp_sub) {
    fflush(g_stream.fp_sub);
    fclose(g_stream.fp_sub);
    g_stream.fp_sub = NULL;
  }
  pthread_mutex_unlock(&g_stream.lock);
}

int media_stream_start(const MediaEncodeConfig *cfg)
{
  if (!cfg) {
    return -1;
  }
  if (g_media.inited) {
    log_error("media", "capture session active; stop first");
    return -2;
  }
  if (g_stream.up) {
    return 0;
  }

  stream_copy_cfg(cfg);
  if (media_aiq_start(g_stream.iq_dir) != 0) {
    return -3;
  }
  if (RK_MPI_SYS_Init() != RK_SUCCESS) {
    media_aiq_stop();
    return -4;
  }
  if (media_vi_dev_init() != 0) {
    RK_MPI_SYS_Exit();
    media_aiq_stop();
    return -5;
  }
  if (stream_create_pipeline(&g_stream.cfg) != 0) {
    RK_MPI_VI_DisableDev(0);
    RK_MPI_SYS_Exit();
    media_aiq_stop();
    return -6;
  }

  sleep(2); /* AE warmup */
  if (stream_start_threads() != 0) {
    stream_destroy_pipeline(1);
    RK_MPI_VI_DisableDev(0);
    RK_MPI_SYS_Exit();
    media_aiq_stop();
    return -7;
  }

  g_stream.up = 1;
  {
    MediaOsdConfig osd;
    if (media_osd_load_config(&osd) == 0 && osd.enabled) {
      if (media_osd_apply(&osd) != 0) {
        log_warn("media", "osd apply on stream start failed");
      }
    }
  }
  log_info("media", "stream start main=%dx%d@%d %dkbps sub=%dx%d@%d %dkbps detect=%dx%d",
           g_stream.cfg.main_w, g_stream.cfg.main_h, g_stream.cfg.main_fps,
           g_stream.cfg.main_bitrate_kbps, g_stream.cfg.sub_w, g_stream.cfg.sub_h,
           g_stream.cfg.sub_fps, g_stream.cfg.sub_bitrate_kbps, g_stream.cfg.detect_w,
           g_stream.cfg.detect_h);
  return 0;
}

int media_stream_apply(const MediaEncodeConfig *cfg)
{
  int need_vi;
  if (!cfg || !g_stream.up) {
    return -1;
  }

  need_vi = cfg_res_changed(&g_stream.cfg, cfg);
  log_info("media", "stream apply rebuild_vi=%d bitrate %d->%d", need_vi,
           g_stream.cfg.main_bitrate_kbps, cfg->main_bitrate_kbps);

  media_isp_multi_frame();
  stream_prepare_stop_threads();
  stream_stop_threads();
  stream_destroy_pipeline(need_vi);

  stream_copy_cfg(cfg);

  if (need_vi) {
    int pw = 0;
    int ph = 0;
    int detect_wanted = (g_stream.cfg.detect_w > 0 && g_stream.cfg.detect_h > 0);
    if (media_vi_chn_init(0, g_stream.cfg.main_w, g_stream.cfg.main_h, 0, 0) != 0 ||
        media_vi_chn_init(1, g_stream.cfg.sub_w, g_stream.cfg.sub_h, 3, 0) != 0) {
      return -2;
    }
    media_preview_main_size(&g_stream.cfg, &pw, &ph);
    g_stream.vi2_up = 0;
    if (media_vi_chn_init(2, pw, ph, 2, 0) != 0) {
      return -2;
    }
    g_stream.vi2_up = 1;
    if (detect_wanted) {
      g_stream.cfg.detect_w = pw;
      g_stream.cfg.detect_h = ph;
    } else {
      g_stream.cfg.detect_w = 0;
      g_stream.cfg.detect_h = 0;
    }
  }

  if (media_venc_init(0, g_stream.cfg.main_w, g_stream.cfg.main_h, RK_VIDEO_ID_HEVC,
                      g_stream.cfg.main_bitrate_kbps, g_stream.cfg.main_gop,
                      g_stream.cfg.main_fps) != 0 ||
      media_venc_init(1, g_stream.cfg.sub_w, g_stream.cfg.sub_h, RK_VIDEO_ID_AVC,
                      g_stream.cfg.sub_bitrate_kbps, g_stream.cfg.sub_gop,
                      g_stream.cfg.sub_fps) != 0) {
    return -3;
  }
  if (media_bind_vi_venc(0, 0) != 0 || media_bind_vi_venc(1, 1) != 0) {
    return -4;
  }
  if (media_preview_main_init(&g_stream.cfg) != 0) {
    log_warn("media", "preview-main venc init failed on apply");
  }
  if (jpeg_combo_create(MEDIA_JPEG_VENC_CHN, MEDIA_JPEG_MASTER_CHN, g_stream.cfg.sub_w,
                        g_stream.cfg.sub_h) != 0) {
    return -4;
  }
  g_stream.jpeg_up = 1;
  if (jpeg_combo_create(MEDIA_JPEG_MAIN_VENC_CHN, MEDIA_JPEG_MAIN_MASTER_CHN,
                        g_stream.cfg.main_w, g_stream.cfg.main_h) == 0) {
    g_stream.jpeg_main_up = 1;
  }
  g_stream.pipeline_up = 1;

  if (stream_start_threads() != 0) {
    return -5;
  }
  RK_MPI_VENC_RequestIDR(0, RK_TRUE);
  RK_MPI_VENC_RequestIDR(1, RK_TRUE);
  {
    MediaOsdConfig osd;
    if (media_osd_load_config(&osd) == 0 && osd.enabled) {
      if (media_osd_apply(&osd) != 0) {
        log_warn("media", "osd apply on stream apply failed");
      }
    }
  }
  log_info("media", "stream apply ok");
  return 0;
}

void media_stream_stop(void)
{
  if (!g_stream.up) {
    return;
  }
  /* AIQ pause can block VENC StopRecvFrame / GetStream teardown after AOV. */
  media_isp_multi_frame();
  stream_prepare_stop_threads();
  stream_stop_threads();
  media_stream_clear_dump();
  stream_destroy_pipeline(1);
  RK_MPI_VI_DisableDev(0);
  RK_MPI_SYS_Exit();
  media_aiq_stop();
  g_stream.up = 0;
  log_info("media", "stream stop packs main=%d sub=%d", g_stream.got_main, g_stream.got_sub);
}

int media_stream_reinit(const MediaEncodeConfig *cfg)
{
  if (!cfg) {
    return -1;
  }
  if (g_stream.up) {
    media_stream_stop();
    sleep(2);
  }
  if (media_stream_start(cfg) != 0) {
    return -2;
  }
  media_stream_request_idr(0);
  media_stream_request_idr(1);
  log_info("media", "stream reinit ok");
  return 0;
}

int media_encode_raw(const MediaEncodeConfig *cfg, const char *out_dir, int seconds)
{
  char main_path[256];
  char sub_path[256];
  const char *dir = out_dir && out_dir[0] ? out_dir : "/mnt/sdcard";

  if (!cfg || seconds <= 0) {
    return -1;
  }
  if (media_stream_start(cfg) != 0) {
    return -2;
  }
  snprintf(main_path, sizeof(main_path), "%s/main.h265", dir);
  snprintf(sub_path, sizeof(sub_path), "%s/sub.h264", dir);
  if (media_stream_set_dump(main_path, sub_path) != 0) {
    media_stream_stop();
    return -3;
  }
  log_info("media", "encode dump %ds -> %s", seconds, dir);
  sleep((unsigned)seconds);
  media_stream_clear_dump();
  int ok = (g_stream.got_main > 0 && g_stream.got_sub > 0);
  media_stream_stop();
  if (!ok) {
    log_error("media", "encode produced too few packets");
    return -10;
  }
  log_info("media", "encode done -> %s %s", main_path, sub_path);
  return 0;
}

static int clamp_pct(int v)
{
  if (v < 0) {
    return 0;
  }
  if (v > 100) {
    return 100;
  }
  return v;
}

static unsigned int pct_to_aiq(int pct)
{
  return (unsigned int)(clamp_pct(pct) * 255 / 100);
}

static int aiq_to_pct(unsigned int level)
{
  if (level > 255) {
    level = 255;
  }
  return (int)((level * 100 + 127) / 255);
}

static int media_apply_vi_mirror_flip(int mirror, int flip)
{
  int chn;
  int ok = 0;
  for (chn = 0; chn <= 2; chn++) {
    VI_CHN_ATTR_S attr;
    memset(&attr, 0, sizeof(attr));
    if (RK_MPI_VI_GetChnAttr(0, chn, &attr) != RK_SUCCESS) {
      continue;
    }
    attr.bMirror = mirror ? RK_TRUE : RK_FALSE;
    attr.bFlip = flip ? RK_TRUE : RK_FALSE;
    if (RK_MPI_VI_SetChnAttr(0, chn, &attr) != RK_SUCCESS) {
      log_warn("media", "VI%d SetChnAttr mirror/flip failed", chn);
      continue;
    }
    ok++;
  }
  return ok > 0 ? 0 : -1;
}

static int media_image_apply_cfg(const MediaImageConfig *cfg)
{
  XCamReturn rc;
  if (!cfg || !g_media.aiq_ctx) {
    return -1;
  }
  rc = rk_aiq_uapi2_setBrightness(g_media.aiq_ctx, pct_to_aiq(cfg->brightness));
  if (rc != XCAM_RETURN_NO_ERROR) {
    log_warn("media", "setBrightness rc=%d", rc);
  }
  rc = rk_aiq_uapi2_setContrast(g_media.aiq_ctx, pct_to_aiq(cfg->contrast));
  if (rc != XCAM_RETURN_NO_ERROR) {
    log_warn("media", "setContrast rc=%d", rc);
  }
  rc = rk_aiq_uapi2_setSaturation(g_media.aiq_ctx, pct_to_aiq(cfg->saturation));
  if (rc != XCAM_RETURN_NO_ERROR) {
    log_warn("media", "setSaturation rc=%d", rc);
  }
  /* AIQ sensor flip (skip frames like rkipc) + VI channel mirror/flip for preview/record. */
  rc = rk_aiq_uapi2_setMirrorFlip(g_media.aiq_ctx, cfg->mirror != 0, cfg->flip != 0, 4);
  if (rc != XCAM_RETURN_NO_ERROR) {
    log_warn("media", "setMirrorFlip rc=%d", rc);
  }
  if (media_apply_vi_mirror_flip(cfg->mirror, cfg->flip) != 0) {
    log_warn("media", "VI mirror/flip not applied (channels not ready?)");
  }
  log_info("media", "image mirror=%d flip=%d applied", cfg->mirror, cfg->flip);
  return 0;
}

static void image_load_cfg(MediaImageConfig *out)
{
  memset(out, 0, sizeof(*out));
  config_get_int("video.image.brightness", &out->brightness, 50);
  config_get_int("video.image.contrast", &out->contrast, 50);
  config_get_int("video.image.saturation", &out->saturation, 50);
  config_get_int("video.image.mirror", &out->mirror, 0);
  config_get_int("video.image.flip", &out->flip, 0);
  out->brightness = clamp_pct(out->brightness);
  out->contrast = clamp_pct(out->contrast);
  out->saturation = clamp_pct(out->saturation);
  out->mirror = out->mirror ? 1 : 0;
  out->flip = out->flip ? 1 : 0;
}

int media_image_get(MediaImageConfig *out)
{
  unsigned int level;
  if (!out) {
    return -1;
  }
  image_load_cfg(out);
  if (g_media.aiq_ctx) {
    if (rk_aiq_uapi2_getBrightness(g_media.aiq_ctx, &level) == XCAM_RETURN_NO_ERROR) {
      out->brightness = aiq_to_pct(level);
    }
    if (rk_aiq_uapi2_getContrast(g_media.aiq_ctx, &level) == XCAM_RETURN_NO_ERROR) {
      out->contrast = aiq_to_pct(level);
    }
    if (rk_aiq_uapi2_getSaturation(g_media.aiq_ctx, &level) == XCAM_RETURN_NO_ERROR) {
      out->saturation = aiq_to_pct(level);
    }
    /* Prefer saved config for mirror/flip: AIQ get can lag or disagree with VI. */
  }
  return 0;
}

int media_image_set(const MediaImageConfig *in, int apply)
{
  MediaImageConfig cfg;
  if (!in) {
    return -1;
  }
  cfg = *in;
  cfg.brightness = clamp_pct(cfg.brightness);
  cfg.contrast = clamp_pct(cfg.contrast);
  cfg.saturation = clamp_pct(cfg.saturation);
  cfg.mirror = cfg.mirror ? 1 : 0;
  cfg.flip = cfg.flip ? 1 : 0;

  if (config_set_int("video.image.brightness", cfg.brightness) != 0 ||
      config_set_int("video.image.contrast", cfg.contrast) != 0 ||
      config_set_int("video.image.saturation", cfg.saturation) != 0 ||
      config_set_int("video.image.mirror", cfg.mirror) != 0 ||
      config_set_int("video.image.flip", cfg.flip) != 0 || config_save() != 0) {
    return -1;
  }
  if (apply) {
    if (!g_media.aiq_ctx) {
      log_warn("media", "image apply deferred (aiq not running)");
      return 0;
    }
    if (media_image_apply_cfg(&cfg) != 0) {
      return -2;
    }
  }
  return 0;
}

int media_osd_get(MediaOsdConfig *out)
{
  return media_osd_load_config(out);
}

int media_osd_set(const MediaOsdConfig *in, int apply)
{
  if (!in) {
    return -1;
  }
  if (media_osd_save_config(in) != 0) {
    return -1;
  }
  if (apply) {
    if (!g_stream.up) {
      log_warn("media", "osd apply deferred (stream not up)");
      return 0;
    }
    if (media_osd_apply(in) != 0) {
      return -2;
    }
  }
  return 0;
}

static int jpeg_combo_capture(int chn, uint8_t **out, size_t *out_len)
{
  VENC_RECV_PIC_PARAM_S recv;
  VENC_STREAM_S stream;
  VENC_PACK_S pack;
  void *data;
  size_t len;
  uint8_t *copy;
  int ret;

  if (!out || !out_len) {
    return -1;
  }
  *out = NULL;
  *out_len = 0;

  pthread_mutex_lock(&g_snap_lock);
  memset(&stream, 0, sizeof(stream));
  memset(&pack, 0, sizeof(pack));
  stream.pstPack = &pack;
  while (RK_MPI_VENC_GetStream(chn, &stream, 0) == RK_SUCCESS) {
    RK_MPI_VENC_ReleaseStream(chn, &stream);
  }
  RK_MPI_VENC_StopRecvFrame(chn);
  memset(&recv, 0, sizeof(recv));
  recv.s32RecvPicNum = 1;
  ret = RK_MPI_VENC_StartRecvFrame(chn, &recv);
  if (ret != RK_SUCCESS) {
    pthread_mutex_unlock(&g_snap_lock);
    return -2;
  }
  memset(&stream, 0, sizeof(stream));
  memset(&pack, 0, sizeof(pack));
  stream.pstPack = &pack;
  ret = RK_MPI_VENC_GetStream(chn, &stream, 3000);
  if (ret != RK_SUCCESS) {
    RK_MPI_VENC_StopRecvFrame(chn);
    pthread_mutex_unlock(&g_snap_lock);
    return -3;
  }
  data = RK_MPI_MB_Handle2VirAddr(pack.pMbBlk);
  len = (size_t)pack.u32Len;
  if (!data || len == 0) {
    RK_MPI_VENC_ReleaseStream(chn, &stream);
    RK_MPI_VENC_StopRecvFrame(chn);
    pthread_mutex_unlock(&g_snap_lock);
    return -4;
  }
  copy = (uint8_t *)malloc(len);
  if (!copy) {
    RK_MPI_VENC_ReleaseStream(chn, &stream);
    RK_MPI_VENC_StopRecvFrame(chn);
    pthread_mutex_unlock(&g_snap_lock);
    return -5;
  }
  memcpy(copy, data, len);
  RK_MPI_VENC_ReleaseStream(chn, &stream);
  RK_MPI_VENC_StopRecvFrame(chn);
  pthread_mutex_unlock(&g_snap_lock);
  *out = copy;
  *out_len = len;
  return 0;
}

/* stream: 0 = sub (704x576), 1 = main (video.encode.main_w x main_h). */
int media_snapshot_jpeg_stream_mem(int stream, uint8_t **out, size_t *out_len)
{
  if (!out || !out_len || !g_stream.up) {
    return -1;
  }
  if (stream == 1) {
    if (!g_stream.jpeg_main_up) {
      return -1;
    }
    return jpeg_combo_capture(MEDIA_JPEG_MAIN_VENC_CHN, out, out_len);
  }
  if (!g_stream.jpeg_up) {
    return -1;
  }
  return jpeg_combo_capture(MEDIA_JPEG_VENC_CHN, out, out_len);
}

int media_snapshot_jpeg_mem(uint8_t **out, size_t *out_len)
{
  return media_snapshot_jpeg_stream_mem(0, out, out_len);
}

int media_snapshot_jpeg(const char *path)
{
  return media_snapshot_jpeg_stream(path, 0);
}

int media_snapshot_jpeg_stream(const char *path, int stream)
{
  uint8_t *buf = NULL;
  size_t len = 0;
  FILE *fp;
  int rc;

  if (!path || !path[0]) return -1;
  rc = media_snapshot_jpeg_stream_mem(stream, &buf, &len);
  if (rc != 0) return rc;
  fp = fopen(path, "wb");
  if (!fp) {
    free(buf);
    return -5;
  }
  if (fwrite(buf, 1, len, fp) != len) {
    fclose(fp);
    unlink(path);
    free(buf);
    return -6;
  }
  fclose(fp);
  free(buf);
  if (stream == 1) {
    log_info("media", "snapshot %s (%zu bytes jpeg-hw %dx%d main)", path, len, g_stream.cfg.main_w,
             g_stream.cfg.main_h);
  } else {
    log_info("media", "snapshot %s (%zu bytes jpeg-hw %dx%d)", path, len, g_stream.cfg.sub_w,
             g_stream.cfg.sub_h);
  }
  return 0;
}

int media_stream_sub_width(void)
{
  return g_stream.up ? g_stream.cfg.sub_w : 0;
}

int media_stream_main_width(void)
{
  return g_stream.up ? g_stream.cfg.main_w : 0;
}

int media_stream_main_height(void)
{
  return g_stream.up ? g_stream.cfg.main_h : 0;
}

int media_stream_sub_height(void)
{
  return g_stream.up ? g_stream.cfg.sub_h : 0;
}

/* RGA-copy VI → MMZ NV12 for CPU/RKNN.
 * Use importbuffer_fd (not bare wrapbuffer_fd) — rkipc pattern; VI1 preferred. */
typedef struct {
  MB_BLK blk;
  uint8_t *vir;
  size_t size;
} MediaNv12Owned;

static pthread_mutex_t g_rga_lock = PTHREAD_MUTEX_INITIALIZER;

static int media_export_vi_nv12(MediaNv12Frame *out, VIDEO_FRAME_INFO_S *frame, int vi_chn)
{
  int w, h, vw, vh, hor, src_fd, dst_fd, rc;
  size_t need;
  MB_BLK dst_blk = MB_INVALID_HANDLE;
  uint8_t *dst_vir;
  rga_buffer_t src, dstb;
  rga_buffer_handle_t src_h = 0, dst_h = 0;
  im_handle_param_t spar, dpar;
  IM_STATUS st;
  MediaNv12Owned *owned;
  RK_U64 phy;
  unsigned ysum = 0;
  int yi;

  if (!out || !frame) return -1;
  w = (int)frame->stVFrame.u32Width;
  h = (int)frame->stVFrame.u32Height;
  vw = (int)frame->stVFrame.u32VirWidth;
  vh = (int)frame->stVFrame.u32VirHeight;
  if (vw <= 0) vw = w;
  if (vh <= 0) vh = h;
  if (w <= 0 || h <= 0 || vw < w || vh < h) return -1;

  hor = (int)RK_MPI_CAL_COMM_GetHorStride((RK_U32)vw, RK_FMT_YUV420SP);
  if (hor < w) hor = vw;

  src_fd = RK_MPI_MB_Handle2Fd(frame->stVFrame.pMbBlk);
  phy = RK_MPI_MB_Handle2PhysAddr(frame->stVFrame.pMbBlk);
  if (src_fd < 0 && phy == 0) return -2;

  need = (size_t)w * (size_t)h * 3 / 2;
  need = (need + 4095u) & ~4095u;
  rc = RK_MPI_SYS_MmzAlloc_Cached(&dst_blk, "face_nv12", RK_NULL, (RK_U32)need);
  if (rc != RK_SUCCESS || !dst_blk) {
    log_warn("media", "MmzAlloc face_nv12 fail rc=%x need=%u", rc, (unsigned)need);
    return -3;
  }
  dst_vir = (uint8_t *)RK_MPI_MB_Handle2VirAddr(dst_blk);
  dst_fd = RK_MPI_MB_Handle2Fd(dst_blk);
  if (!dst_vir || dst_fd < 0) {
    RK_MPI_SYS_MmzFree(dst_blk);
    return -3;
  }

  memset(&spar, 0, sizeof(spar));
  spar.width = (uint32_t)hor;
  spar.height = (uint32_t)vh;
  spar.format = RK_FORMAT_YCbCr_420_SP;
  memset(&dpar, 0, sizeof(dpar));
  dpar.width = (uint32_t)w;
  dpar.height = (uint32_t)h;
  dpar.format = RK_FORMAT_YCbCr_420_SP;

  pthread_mutex_lock(&g_rga_lock);
  if (src_fd >= 0) src_h = importbuffer_fd(src_fd, &spar);
  if (!src_h && phy) src_h = importbuffer_physicaladdr(phy, &spar);
  dst_h = importbuffer_fd(dst_fd, &dpar);
  if (!src_h || !dst_h) {
    pthread_mutex_unlock(&g_rga_lock);
    if (src_h) releasebuffer_handle(src_h);
    if (dst_h) releasebuffer_handle(dst_h);
    RK_MPI_SYS_MmzFree(dst_blk);
    log_warn("media", "rga import fail vi%d src_h=%d dst_h=%d fd=%d phy=%llx", vi_chn, (int)src_h,
             (int)dst_h, src_fd, (unsigned long long)phy);
    return -4;
  }
  src = wrapbuffer_handle(src_h, w, h, RK_FORMAT_YCbCr_420_SP, hor, vh);
  dstb = wrapbuffer_handle(dst_h, w, h, RK_FORMAT_YCbCr_420_SP, w, h);
  st = imcopy(src, dstb);
  releasebuffer_handle(src_h);
  releasebuffer_handle(dst_h);
  pthread_mutex_unlock(&g_rga_lock);

  if (st != IM_STATUS_SUCCESS) {
    log_warn("media", "rga imcopy fail vi%d st=%d", vi_chn, (int)st);
    RK_MPI_SYS_MmzFree(dst_blk);
    return -4;
  }
  (void)RK_MPI_SYS_MmzFlushCache(dst_blk, RK_FALSE);

  for (yi = 0; yi < 256 && yi < w * h; yi++) ysum += dst_vir[yi];
  if (ysum < 16 * 256 || ysum > 250 * 256) {
    static int bad_logged;
    if (!bad_logged) {
      bad_logged = 1;
      log_warn("media", "vi%d nv12 Y sanity fail mean~%u (comp=%d)", vi_chn, ysum / 256,
               (int)frame->stVFrame.enCompressMode);
    }
  }

  owned = (MediaNv12Owned *)calloc(1, sizeof(*owned));
  if (!owned) {
    RK_MPI_SYS_MmzFree(dst_blk);
    return -3;
  }
  owned->blk = dst_blk;
  owned->vir = dst_vir;
  owned->size = need;

  {
    static int logged;
    if (!logged) {
      logged = 1;
      log_info("media",
               "grab vi%d rga+mmz ok %dx%d hor=%d src_fd=%d phy=%llx ymean~%u comp=%d", vi_chn, w, h,
               hor, src_fd, (unsigned long long)phy, ysum / 256,
               (int)frame->stVFrame.enCompressMode);
    }
  }

  out->width = w;
  out->height = h;
  out->vir_width = w;
  out->vir_height = h;
  out->y_stride = w;
  out->uv_stride = w;
  out->y = dst_vir;
  out->uv = dst_vir + (size_t)w * (size_t)h;
  out->nv12_size = (size_t)w * (size_t)h * 3 / 2;
  out->priv = owned;
  out->vi_chn = vi_chn;
  return 0;
}

int media_grab_sub_nv12(MediaNv12Frame *out, int timeout_ms)
{
  VIDEO_FRAME_INFO_S frame;
  int ret;
  if (!out || !g_stream.up) return -1;
  memset(out, 0, sizeof(*out));
  memset(&frame, 0, sizeof(frame));
  ret = RK_MPI_VI_GetChnFrame(0, 1, &frame, timeout_ms > 0 ? timeout_ms : 200);
  if (ret != RK_SUCCESS) return -3;
  ret = media_export_vi_nv12(out, &frame, 1);
  RK_MPI_VI_ReleaseChnFrame(0, 1, &frame);
  if (ret != 0) {
    memset(out, 0, sizeof(*out));
    return -4;
  }
  return 0;
}

void media_release_sub_nv12(MediaNv12Frame *frame)
{
  MediaNv12Owned *owned;
  if (!frame) return;
  owned = (MediaNv12Owned *)frame->priv;
  if (owned) {
    if (owned->blk) RK_MPI_SYS_MmzFree(owned->blk);
    free(owned);
  }
  memset(frame, 0, sizeof(*frame));
}

int media_grab_detect_nv12(MediaNv12Frame *out, int timeout_ms)
{
  VIDEO_FRAME_INFO_S frame;
  int ret;
  if (!out || !g_stream.up || !g_stream.vi2_up) return -1;
  memset(out, 0, sizeof(*out));
  memset(&frame, 0, sizeof(frame));
  ret = RK_MPI_VI_GetChnFrame(0, 2, &frame, timeout_ms > 0 ? timeout_ms : 200);
  if (ret != RK_SUCCESS) return -3;
  ret = media_export_vi_nv12(out, &frame, 2);
  RK_MPI_VI_ReleaseChnFrame(0, 2, &frame);
  if (ret != 0) {
    memset(out, 0, sizeof(*out));
    return -4;
  }
  return 0;
}

void media_release_detect_nv12(MediaNv12Frame *frame)
{
  media_release_sub_nv12(frame);
}

int media_isp_single_frame(void)
{
  if (!g_media.aiq_ctx && !g_stream.up) {
    log_warn("media", "single_frame: no AIQ context");
    return -1;
  }
  if (g_media.aiq_ctx && !g_aiq_paused) {
    rk_aiq_uapi2_sysctl_pause(g_media.aiq_ctx, true);
    g_aiq_paused = 1;
    log_info("media", "ISP single-frame (AIQ pause)");
  }
  return 0;
}

int media_isp_multi_frame(void)
{
  if (g_media.aiq_ctx && g_aiq_paused) {
    rk_aiq_uapi2_sysctl_resume(g_media.aiq_ctx);
    g_aiq_paused = 0;
    log_info("media", "ISP multi-frame (AIQ resume)");
  }
  return 0;
}

int media_aov_apply_event_fps(int fps)
{
  MediaEncodeConfig cfg;
  int before;

  if (fps <= 0) {
    fps = 25;
  }
  if (!g_stream.up) {
    log_warn("media", "event_fps: stream not up (deferred)");
    return 0;
  }
  cfg = g_stream.cfg;
  cfg.main_fps = fps;
  if (cfg.main_gop < fps) {
    cfg.main_gop = fps * 2;
  }
  cfg.iq_dir = g_stream.iq_dir;
  before = g_stream.got_main;
  if (media_stream_apply(&cfg) != 0) {
    return -1;
  }
  sleep(1); /* AE/VENC warmup after fps rebuild */
  media_stream_request_idr(0);
  media_stream_request_idr(1);
  log_info("media", "event_fps=%d packs_main %d->%d", fps, before, g_stream.got_main);
  return 0;
}
