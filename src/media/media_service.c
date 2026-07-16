#include "media/media_service.h"

#include "common/log.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <rk_aiq_user_api2_sysctl.h>

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

typedef struct {
  int chn;
  FILE *fp;
  volatile int *quit;
  int *got;
  int target_sec;
  int fps;
} VencThreadArg;

static MediaCtx g_media;
static volatile int g_quit;
static int g_frame_target;
static int g_frame_got;

static XCamReturn media_aiq_err_cb(rk_aiq_err_msg_t *msg) {
  if (msg && msg->err_code == XCAM_RETURN_BYPASS) {
    log_warn("media", "aiq bypass err");
  }
  return XCAM_RETURN_NO_ERROR;
}

static XCamReturn media_aiq_sof_cb(rk_aiq_metas_t *meta) {
  (void)meta;
  return XCAM_RETURN_NO_ERROR;
}

static int media_aiq_start(const char *iq_dir) {
  rk_aiq_static_info_t info;
  memset(&info, 0, sizeof(info));
  if (rk_aiq_uapi2_sysctl_enumStaticMetas(0, &info) != 0) {
    log_error("media", "enumStaticMetas failed");
    return -1;
  }
  log_info("media", "sensor=%s iq=%s", info.sensor_info.sensor_name, iq_dir);

  setenv("HDR_MODE", "0", 1);
  g_media.aiq_ctx = rk_aiq_uapi2_sysctl_init(info.sensor_info.sensor_name, iq_dir,
                                              media_aiq_err_cb, media_aiq_sof_cb);
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
  return 0;
}

static void media_aiq_stop(void) {
  if (!g_media.aiq_ctx) {
    return;
  }
  rk_aiq_uapi2_sysctl_stop(g_media.aiq_ctx, false);
  rk_aiq_uapi2_sysctl_deinit(g_media.aiq_ctx);
  g_media.aiq_ctx = NULL;
}

static int media_vi_dev_init(void) {
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

static int media_vi_chn_init(int chn, int width, int height, int depth) {
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
                           int gop, int fps) {
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

static int media_bind_vi_venc(int vi_chn, int venc_chn) {
  MPP_CHN_S src = {.enModId = RK_ID_VI, .s32DevId = 0, .s32ChnId = vi_chn};
  MPP_CHN_S dst = {.enModId = RK_ID_VENC, .s32DevId = 0, .s32ChnId = venc_chn};
  int ret = RK_MPI_SYS_Bind(&src, &dst);
  if (ret != RK_SUCCESS) {
    log_error("media", "bind VI%d->VENC%d fail %x", vi_chn, venc_chn, ret);
  }
  return ret;
}

static void media_unbind_vi_venc(int vi_chn, int venc_chn) {
  MPP_CHN_S src = {.enModId = RK_ID_VI, .s32DevId = 0, .s32ChnId = vi_chn};
  MPP_CHN_S dst = {.enModId = RK_ID_VENC, .s32DevId = 0, .s32ChnId = venc_chn};
  RK_MPI_SYS_UnBind(&src, &dst);
}

static void *media_get_frame_thread(void *arg) {
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

static void *media_venc_get_thread(void *arg) {
  VencThreadArg *a = (VencThreadArg *)arg;
  VENC_STREAM_S st;
  memset(&st, 0, sizeof(st));
  st.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S));
  if (!st.pstPack) {
    return NULL;
  }

  int packs = 0;
  while (!*(a->quit)) {
    int ret = RK_MPI_VENC_GetStream(a->chn, &st, 1000);
    if (ret == RK_SUCCESS) {
      void *data = RK_MPI_MB_Handle2VirAddr(st.pstPack->pMbBlk);
      if (a->fp && data && st.pstPack->u32Len > 0) {
        fwrite(data, 1, st.pstPack->u32Len, a->fp);
      }
      RK_MPI_VENC_ReleaseStream(a->chn, &st);
      packs++;
      *(a->got) = packs;
    }
  }

  if (a->fp) {
    fflush(a->fp);
  }
  free(st.pstPack);
  return NULL;
}

int media_init(const MediaConfig *cfg) {
  if (g_media.inited) {
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

void media_deinit(void) {
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

int media_capture_nv12(const char *out_dir, const char *file_name, int frame_count) {
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

int media_encode_raw(const MediaEncodeConfig *cfg, const char *out_dir, int seconds) {
  if (!cfg || seconds <= 0) {
    return -1;
  }
  if (g_media.inited) {
    log_error("media", "already inited; encode needs exclusive session");
    return -2;
  }

  const char *iq = cfg->iq_dir && cfg->iq_dir[0] ? cfg->iq_dir : "/oem/usr/share/iqfiles";
  const char *dir = out_dir && out_dir[0] ? out_dir : "/mnt/sdcard";

  if (media_aiq_start(iq) != 0) {
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

  /* depth=0 when binding to venc */
  if (media_vi_chn_init(0, cfg->main_w, cfg->main_h, 0) != 0 ||
      media_vi_chn_init(1, cfg->sub_w, cfg->sub_h, 0) != 0) {
    RK_MPI_VI_DisableDev(0);
    RK_MPI_SYS_Exit();
    media_aiq_stop();
    return -6;
  }

  if (media_venc_init(0, cfg->main_w, cfg->main_h, RK_VIDEO_ID_HEVC, cfg->main_bitrate_kbps,
                      cfg->main_gop, cfg->main_fps) != 0 ||
      media_venc_init(1, cfg->sub_w, cfg->sub_h, RK_VIDEO_ID_AVC, cfg->sub_bitrate_kbps,
                      cfg->sub_gop, cfg->sub_fps) != 0) {
    RK_MPI_VI_DisableChn(0, 0);
    RK_MPI_VI_DisableChn(0, 1);
    RK_MPI_VI_DisableDev(0);
    RK_MPI_SYS_Exit();
    media_aiq_stop();
    return -7;
  }

  if (media_bind_vi_venc(0, 0) != 0 || media_bind_vi_venc(1, 1) != 0) {
    RK_MPI_VENC_StopRecvFrame(0);
    RK_MPI_VENC_DestroyChn(0);
    RK_MPI_VENC_StopRecvFrame(1);
    RK_MPI_VENC_DestroyChn(1);
    RK_MPI_VI_DisableChn(0, 0);
    RK_MPI_VI_DisableChn(0, 1);
    RK_MPI_VI_DisableDev(0);
    RK_MPI_SYS_Exit();
    media_aiq_stop();
    return -8;
  }

  char main_path[256];
  char sub_path[256];
  snprintf(main_path, sizeof(main_path), "%s/main.h265", dir);
  snprintf(sub_path, sizeof(sub_path), "%s/sub.h264", dir);
  FILE *fp_main = fopen(main_path, "wb");
  FILE *fp_sub = fopen(sub_path, "wb");
  if (!fp_main || !fp_sub) {
    log_error("media", "open output files failed under %s", dir);
    if (fp_main) {
      fclose(fp_main);
    }
    if (fp_sub) {
      fclose(fp_sub);
    }
    media_unbind_vi_venc(0, 0);
    media_unbind_vi_venc(1, 1);
    RK_MPI_VENC_StopRecvFrame(0);
    RK_MPI_VENC_DestroyChn(0);
    RK_MPI_VENC_StopRecvFrame(1);
    RK_MPI_VENC_DestroyChn(1);
    RK_MPI_VI_DisableChn(0, 0);
    RK_MPI_VI_DisableChn(0, 1);
    RK_MPI_VI_DisableDev(0);
    RK_MPI_SYS_Exit();
    media_aiq_stop();
    return -9;
  }

  log_info("media", "encode start main=%dx%d@%d H265 sub=%dx%d@%d H264 for %ds", cfg->main_w,
           cfg->main_h, cfg->main_fps, cfg->sub_w, cfg->sub_h, cfg->sub_fps, seconds);

  /* AE warmup without writing */
  sleep(2);

  volatile int quit = 0;
  int got_main = 0;
  int got_sub = 0;
  VencThreadArg a0 = {.chn = 0, .fp = fp_main, .quit = &quit, .got = &got_main};
  VencThreadArg a1 = {.chn = 1, .fp = fp_sub, .quit = &quit, .got = &got_sub};
  pthread_t t0, t1;
  pthread_create(&t0, NULL, media_venc_get_thread, &a0);
  pthread_create(&t1, NULL, media_venc_get_thread, &a1);

  sleep((unsigned)seconds);
  quit = 1;
  pthread_join(t0, NULL);
  pthread_join(t1, NULL);
  fclose(fp_main);
  fclose(fp_sub);

  log_info("media", "encode done packs main=%d sub=%d -> %s %s", got_main, got_sub, main_path,
           sub_path);

  media_unbind_vi_venc(0, 0);
  media_unbind_vi_venc(1, 1);
  RK_MPI_VENC_StopRecvFrame(0);
  RK_MPI_VENC_DestroyChn(0);
  RK_MPI_VENC_StopRecvFrame(1);
  RK_MPI_VENC_DestroyChn(1);
  RK_MPI_VI_DisableChn(0, 0);
  RK_MPI_VI_DisableChn(0, 1);
  RK_MPI_VI_DisableDev(0);
  RK_MPI_SYS_Exit();
  media_aiq_stop();

  if (got_main < 1 || got_sub < 1) {
    log_error("media", "encode produced too few packets");
    return -10;
  }
  return 0;
}
