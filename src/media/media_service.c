#include "media/media_service.h"

#include "common/jpeg_writer.h"
#include "common/log.h"
#include "system/config_service.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <rk_aiq_user_api2_sysctl.h>
#include <rk_aiq_user_api2_imgproc.h>

#include "rk_comm_venc.h"
#include "rk_debug.h"
#include "rk_defines.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_vi.h"

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
  int threads_running;
  MediaEncodeConfig cfg;
  char iq_dir[256];
  volatile int quit;
  pthread_t t0, t1;
  int got_main;
  int got_sub;
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
static volatile int g_quit;
static int g_frame_target;
static int g_frame_got;

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

static int media_vi_chn_init(int chn, int width, int height, int depth)
{
  VI_CHN_ATTR_S attr;
  memset(&attr, 0, sizeof(attr));
  attr.stIspOpt.u32BufCount = 3;
  attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
  attr.stSize.u32Width = (RK_U32)width;
  attr.stSize.u32Height = (RK_U32)height;
  attr.enPixelFormat = RK_FMT_YUV420SP;
  attr.enCompressMode = COMPRESS_MODE_NONE;
  attr.u32Depth = (RK_U32)depth;

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
  memset(&attr, 0, sizeof(attr));
  memset(&recv, 0, sizeof(recv));

  if (type == RK_VIDEO_ID_AVC) {
    attr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
    attr.stRcAttr.stH264Cbr.u32BitRate = (RK_U32)bitrate_kbps;
    attr.stRcAttr.stH264Cbr.u32Gop = (RK_U32)gop;
    attr.stRcAttr.stH264Cbr.u32SrcFrameRateNum = (RK_U32)fps;
    attr.stRcAttr.stH264Cbr.u32SrcFrameRateDen = 1;
    attr.stRcAttr.stH264Cbr.fr32DstFrameRateNum = (RK_U32)fps;
    attr.stRcAttr.stH264Cbr.fr32DstFrameRateDen = 1;
    attr.stVencAttr.u32Profile = H264E_PROFILE_HIGH;
  } else {
    attr.stRcAttr.enRcMode = VENC_RC_MODE_H265CBR;
    attr.stRcAttr.stH265Cbr.u32BitRate = (RK_U32)bitrate_kbps;
    attr.stRcAttr.stH265Cbr.u32Gop = (RK_U32)gop;
    attr.stRcAttr.stH265Cbr.u32SrcFrameRateNum = (RK_U32)fps;
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
  if (media_vi_chn_init(0, cfg->main_w, cfg->main_h, 0) != 0 ||
      media_vi_chn_init(1, cfg->sub_w, cfg->sub_h, 2) != 0) {
    return -1;
  }
  g_stream.vi2_up = 0;
  if (cfg->detect_w > 0 && cfg->detect_h > 0) {
    if (media_vi_chn_init(2, cfg->detect_w, cfg->detect_h, 0) != 0) {
      RK_MPI_VI_DisableChn(0, 0);
      RK_MPI_VI_DisableChn(0, 1);
      return -1;
    }
    g_stream.vi2_up = 1;
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
  g_stream.pipeline_up = 1;
  return 0;
}

static void stream_destroy_pipeline(int destroy_vi)
{
  if (!g_stream.pipeline_up && !destroy_vi) {
    return;
  }
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
    FILE *fp = (a->chn == 0) ? g_stream.fp_main : g_stream.fp_sub;
    if (fp && data && len > 0) {
      fwrite(data, 1, len, fp);
    }
    if (a->chn == 0) {
      g_stream.got_main++;
    } else {
      g_stream.got_sub++;
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
  g_stream.quit = 0;
  g_stream.got_main = 0;
  g_stream.got_sub = 0;
  if (pthread_create(&g_stream.t0, NULL, media_stream_get_thread, &a0) != 0) {
    return -1;
  }
  if (pthread_create(&g_stream.t1, NULL, media_stream_get_thread, &a1) != 0) {
    g_stream.quit = 1;
    pthread_join(g_stream.t0, NULL);
    return -2;
  }
  g_stream.threads_running = 1;
  return 0;
}

static void stream_stop_threads(void)
{
  if (!g_stream.threads_running) {
    return;
  }
  g_stream.quit = 1;
  pthread_join(g_stream.t0, NULL);
  pthread_join(g_stream.t1, NULL);
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
      media_vi_chn_init(g_media.vi_chn, g_media.width, g_media.height, 2) != 0) {
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

  stream_stop_threads();
  stream_destroy_pipeline(need_vi);

  stream_copy_cfg(cfg);

  if (need_vi) {
    if (media_vi_chn_init(0, g_stream.cfg.main_w, g_stream.cfg.main_h, 0) != 0 ||
        media_vi_chn_init(1, g_stream.cfg.sub_w, g_stream.cfg.sub_h, 2) != 0) {
      return -2;
    }
    g_stream.vi2_up = 0;
    if (g_stream.cfg.detect_w > 0 && g_stream.cfg.detect_h > 0) {
      if (media_vi_chn_init(2, g_stream.cfg.detect_w, g_stream.cfg.detect_h, 0) != 0) {
        return -2;
      }
      g_stream.vi2_up = 1;
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
  g_stream.pipeline_up = 1;

  if (stream_start_threads() != 0) {
    return -5;
  }
  RK_MPI_VENC_RequestIDR(0, RK_TRUE);
  RK_MPI_VENC_RequestIDR(1, RK_TRUE);
  log_info("media", "stream apply ok");
  return 0;
}

void media_stream_stop(void)
{
  if (!g_stream.up) {
    return;
  }
  stream_stop_threads();
  media_stream_clear_dump();
  stream_destroy_pipeline(1);
  RK_MPI_VI_DisableDev(0);
  RK_MPI_SYS_Exit();
  media_aiq_stop();
  g_stream.up = 0;
  log_info("media", "stream stop packs main=%d sub=%d", g_stream.got_main, g_stream.got_sub);
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
  rc = rk_aiq_uapi2_setMirrorFlip(g_media.aiq_ctx, cfg->mirror != 0, cfg->flip != 0, 0);
  if (rc != XCAM_RETURN_NO_ERROR) {
    log_warn("media", "setMirrorFlip rc=%d", rc);
  }
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
    {
      bool mirror = false;
      bool flip = false;
      if (rk_aiq_uapi2_getMirrorFlip(g_media.aiq_ctx, &mirror, &flip) == XCAM_RETURN_NO_ERROR) {
        out->mirror = mirror ? 1 : 0;
        out->flip = flip ? 1 : 0;
      }
    }
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

int media_snapshot_jpeg(const char *path)
{
  VIDEO_FRAME_INFO_S frame;
  FILE *fp;
  uint8_t *jpg = NULL;
  size_t jpg_cap;
  size_t jpg_len;
  const uint8_t *y;
  int w;
  int h;
  int ret;

  if (!path || !path[0] || !g_stream.up) {
    return -1;
  }
  memset(&frame, 0, sizeof(frame));
  ret = RK_MPI_VI_GetChnFrame(0, 1, &frame, 500);
  if (ret != RK_SUCCESS) {
    log_warn("media", "snapshot GetChnFrame fail %x", ret);
    return -2;
  }
  w = (int)frame.stVFrame.u32Width;
  h = (int)frame.stVFrame.u32Height;
  y = (const uint8_t *)RK_MPI_MB_Handle2VirAddr(frame.stVFrame.pMbBlk);
  if (!y || w <= 0 || h <= 0) {
    RK_MPI_VI_ReleaseChnFrame(0, 1, &frame);
    return -3;
  }
  jpg_cap = (size_t)(w * h + 65536);
  jpg = (uint8_t *)malloc(jpg_cap);
  if (!jpg) {
    RK_MPI_VI_ReleaseChnFrame(0, 1, &frame);
    return -4;
  }
  jpg_len = jpeg_write_gray(y, w, h, (int)frame.stVFrame.u32VirWidth, jpg, jpg_cap, 75);
  RK_MPI_VI_ReleaseChnFrame(0, 1, &frame);
  if (jpg_len == 0) {
    free(jpg);
    return -5;
  }
  fp = fopen(path, "wb");
  if (!fp) {
    free(jpg);
    return -6;
  }
  if (fwrite(jpg, 1, jpg_len, fp) != jpg_len) {
    fclose(fp);
    free(jpg);
    unlink(path);
    return -7;
  }
  fclose(fp);
  free(jpg);
  log_info("media", "snapshot %s (%ux%u %zu bytes)", path, (unsigned)w, (unsigned)h, jpg_len);
  return 0;
}
