#include "face/rknn_face.h"

#include "common/log.h"
#include "rknn_api.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FACE_OUT_MAX 16

struct RknnFaceCtx {
  rknn_context ctx;
  rknn_input_output_num io_num;
  rknn_tensor_attr input_attr;
  rknn_tensor_mem *input_mem;
  rknn_tensor_attr out_attr[FACE_OUT_MAX];
  rknn_tensor_mem *out_mem[FACE_OUT_MAX];
  float *out_f[FACE_OUT_MAX];
  int out_elems[FACE_OUT_MAX];
  int ready;
};

static float clampf(float v, float lo, float hi)
{
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static float sigmoidf_safe(float x)
{
  if (x >= 0.f) {
    float z = expf(-x);
    return 1.f / (1.f + z);
  }
  float z = expf(x);
  return z / (1.f + z);
}

static float iou_box(const RknnFaceBox *a, const RknnFaceBox *b)
{
  float x1 = a->x1 > b->x1 ? a->x1 : b->x1;
  float y1 = a->y1 > b->y1 ? a->y1 : b->y1;
  float x2 = a->x2 < b->x2 ? a->x2 : b->x2;
  float y2 = a->y2 < b->y2 ? a->y2 : b->y2;
  float iw = x2 - x1;
  float ih = y2 - y1;
  float inter, uni, aa, bb;
  if (iw <= 0.f || ih <= 0.f) return 0.f;
  inter = iw * ih;
  aa = (a->x2 - a->x1) * (a->y2 - a->y1);
  bb = (b->x2 - b->x1) * (b->y2 - b->y1);
  uni = aa + bb - inter;
  return uni > 0.f ? inter / uni : 0.f;
}

static void nms_inplace(RknnFaceBox *boxes, int *n, float iou_thr)
{
  int i, j, keep = 0;
  for (i = 1; i < *n; i++) {
    RknnFaceBox key = boxes[i];
    j = i - 1;
    while (j >= 0 && boxes[j].score < key.score) {
      boxes[j + 1] = boxes[j];
      j--;
    }
    boxes[j + 1] = key;
  }
  for (i = 0; i < *n; i++) {
    int suppressed = 0;
    for (j = 0; j < keep; j++) {
      if (iou_box(&boxes[i], &boxes[j]) > iou_thr) {
        suppressed = 1;
        break;
      }
    }
    if (!suppressed) boxes[keep++] = boxes[i];
  }
  *n = keep;
}

static unsigned char *read_file(const char *path, int *size_out)
{
  FILE *fp = fopen(path, "rb");
  long sz;
  unsigned char *buf;
  if (!fp) return NULL;
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return NULL;
  }
  sz = ftell(fp);
  if (sz <= 0) {
    fclose(fp);
    return NULL;
  }
  rewind(fp);
  buf = (unsigned char *)malloc((size_t)sz);
  if (!buf) {
    fclose(fp);
    return NULL;
  }
  if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
    free(buf);
    fclose(fp);
    return NULL;
  }
  fclose(fp);
  *size_out = (int)sz;
  return buf;
}

static int dequant_to_float(const rknn_tensor_attr *attr, const void *src, float *dst, int elems)
{
  int i;
  float scale = attr->scale;
  int32_t zp = attr->zp;
  if (elems <= 0) return -1;
  if (attr->type == RKNN_TENSOR_FLOAT32) {
    memcpy(dst, src, (size_t)elems * sizeof(float));
    return 0;
  }
  if (attr->type == RKNN_TENSOR_INT8) {
    const int8_t *p = (const int8_t *)src;
    for (i = 0; i < elems; i++) dst[i] = ((float)p[i] - (float)zp) * scale;
    return 0;
  }
  if (attr->type == RKNN_TENSOR_UINT8) {
    const uint8_t *p = (const uint8_t *)src;
    for (i = 0; i < elems; i++) dst[i] = ((float)p[i] - (float)zp) * scale;
    return 0;
  }
  if (attr->type == RKNN_TENSOR_INT16) {
    const int16_t *p = (const int16_t *)src;
    for (i = 0; i < elems; i++) dst[i] = ((float)p[i] - (float)zp) * scale;
    return 0;
  }
  memcpy(dst, src, (size_t)elems * sizeof(float));
  return 0;
}

RknnFaceCtx *rknn_face_create(const char *model_path)
{
  RknnFaceCtx *c;
  unsigned char *model;
  int model_len = 0;
  int ret;
  uint32_t i;

  if (!model_path || !model_path[0]) return NULL;
  c = (RknnFaceCtx *)calloc(1, sizeof(*c));
  if (!c) return NULL;

  model = read_file(model_path, &model_len);
  if (!model) {
    log_error("face", "model open failed %s", model_path);
    free(c);
    return NULL;
  }
  ret = rknn_init(&c->ctx, model, model_len, 0, NULL);
  free(model);
  if (ret != RKNN_SUCC) {
    log_error("face", "rknn_init failed %d", ret);
    free(c);
    return NULL;
  }

  ret = rknn_query(c->ctx, RKNN_QUERY_IN_OUT_NUM, &c->io_num, sizeof(c->io_num));
  if (ret != RKNN_SUCC || c->io_num.n_input < 1 || c->io_num.n_output < 9) {
    log_error("face", "unexpected io in=%u out=%u", c->io_num.n_input, c->io_num.n_output);
    rknn_destroy(c->ctx);
    free(c);
    return NULL;
  }
  if (c->io_num.n_output > FACE_OUT_MAX) c->io_num.n_output = FACE_OUT_MAX;

  c->input_attr.index = 0;
  ret = rknn_query(c->ctx, RKNN_QUERY_INPUT_ATTR, &c->input_attr, sizeof(c->input_attr));
  if (ret != RKNN_SUCC) goto fail;
  c->input_attr.type = RKNN_TENSOR_UINT8;
  c->input_attr.fmt = RKNN_TENSOR_NHWC;
  c->input_mem = rknn_create_mem(c->ctx, c->input_attr.size_with_stride ? c->input_attr.size_with_stride
                                                                        : c->input_attr.size);
  if (!c->input_mem) goto fail;
  ret = rknn_set_io_mem(c->ctx, c->input_mem, &c->input_attr);
  if (ret != RKNN_SUCC) {
    log_error("face", "set_io_mem input failed %d", ret);
    goto fail;
  }

  for (i = 0; i < c->io_num.n_output; i++) {
    uint32_t sz;
    rknn_tensor_attr logical;
    c->out_attr[i].index = i;
    ret = rknn_query(c->ctx, RKNN_QUERY_NATIVE_OUTPUT_ATTR, &c->out_attr[i], sizeof(c->out_attr[i]));
    if (ret != RKNN_SUCC) goto fail;
    sz = c->out_attr[i].size_with_stride ? c->out_attr[i].size_with_stride : c->out_attr[i].size;
    c->out_mem[i] = rknn_create_mem(c->ctx, sz);
    if (!c->out_mem[i]) goto fail;
    ret = rknn_set_io_mem(c->ctx, c->out_mem[i], &c->out_attr[i]);
    if (ret != RKNN_SUCC) {
      log_error("face", "set_io_mem out%u failed %d", i, ret);
      goto fail;
    }
    memset(&logical, 0, sizeof(logical));
    logical.index = i;
    if (rknn_query(c->ctx, RKNN_QUERY_OUTPUT_ATTR, &logical, sizeof(logical)) == RKNN_SUCC) {
      if (logical.n_elems > 0) {
        c->out_elems[i] = (int)logical.n_elems;
      } else {
        int e = 1, d;
        for (d = 0; d < 4; d++) {
          if (logical.dims[d] > 0) e *= (int)logical.dims[d];
        }
        c->out_elems[i] = e > 0 ? e : (int)(logical.size);
      }
    } else {
      c->out_elems[i] = (int)c->out_attr[i].n_elems;
    }
    if (c->out_elems[i] <= 0) c->out_elems[i] = 1600;
    c->out_f[i] = (float *)malloc((size_t)c->out_elems[i] * sizeof(float));
    if (!c->out_f[i]) goto fail;
  }

  c->ready = 1;
  log_info("face", "rknn ready model=%s outs=%u", model_path, c->io_num.n_output);
  return c;

fail:
  rknn_face_destroy(c);
  return NULL;
}

void rknn_face_destroy(RknnFaceCtx *ctx)
{
  uint32_t i;
  if (!ctx) return;
  for (i = 0; i < FACE_OUT_MAX; i++) {
    if (ctx->out_mem[i] && ctx->ctx) rknn_destroy_mem(ctx->ctx, ctx->out_mem[i]);
    free(ctx->out_f[i]);
  }
  if (ctx->input_mem && ctx->ctx) rknn_destroy_mem(ctx->ctx, ctx->input_mem);
  if (ctx->ctx) rknn_destroy(ctx->ctx);
  free(ctx);
}

/* yunet_n_320_320 already ends in sigmoid; only squash if a tensor holds logits. */
static int tensor_is_prob(const float *v, int n)
{
  int i;
  for (i = 0; i < n; i++) {
    if (v[i] < -0.01f || v[i] > 1.01f) return 0;
  }
  return 1;
}

static float to_prob(float v, int is_prob)
{
  return is_prob ? clampf(v, 0.f, 1.f) : sigmoidf_safe(v);
}

/* OpenCV FaceDetectorYN decode: bbox = (dx, dy, log w, log h) per grid cell. */
static void decode_stride(const float *cls, const float *obj, const float *bbox, int rows, int cols,
                          int stride, float score_thr, RknnFaceBox *out, int *n, int max_n,
                          float *max_score_io)
{
  int r, c;
  int cls_prob = tensor_is_prob(cls, rows * cols);
  int obj_prob = tensor_is_prob(obj, rows * cols);
  for (r = 0; r < rows && *n < max_n; r++) {
    for (c = 0; c < cols && *n < max_n; c++) {
      int idx = r * cols + c;
      float cs, os, score, cx, cy, bw, bh, x1, y1, x2, y2;
      cs = to_prob(cls[idx], cls_prob);
      os = to_prob(obj[idx], obj_prob);
      score = sqrtf(cs * os);
      if (max_score_io && score > *max_score_io) *max_score_io = score;
      if (score < score_thr) continue;
      cx = ((float)c + bbox[idx * 4 + 0]) * (float)stride;
      cy = ((float)r + bbox[idx * 4 + 1]) * (float)stride;
      bw = expf(bbox[idx * 4 + 2]) * (float)stride;
      bh = expf(bbox[idx * 4 + 3]) * (float)stride;
      x1 = cx - bw * 0.5f;
      y1 = cy - bh * 0.5f;
      x2 = cx + bw * 0.5f;
      y2 = cy + bh * 0.5f;
      x1 = clampf(x1, 0.f, (float)RKNN_FACE_INPUT - 1.f);
      y1 = clampf(y1, 0.f, (float)RKNN_FACE_INPUT - 1.f);
      x2 = clampf(x2, 0.f, (float)RKNN_FACE_INPUT - 1.f);
      y2 = clampf(y2, 0.f, (float)RKNN_FACE_INPUT - 1.f);
      /* Drop letterbox-pad / noise strips (~20x10 at y=0). */
      if (x2 - x1 < 16.f || y2 - y1 < 16.f) continue;
      out[*n].x1 = x1;
      out[*n].y1 = y1;
      out[*n].x2 = x2;
      out[*n].y2 = y2;
      out[*n].score = score;
      (*n)++;
    }
  }
}

int rknn_face_infer(RknnFaceCtx *ctx, const uint8_t *bgr320, float score_thr, float nms_iou,
                    RknnFaceBox *out, int max_out, int *out_n)
{
  int ret;
  int n = 0;
  uint32_t i;
  const int strides[3] = {8, 16, 32};
  const int grids[3] = {40, 20, 10};
  float max_score = 0.f;

  if (!ctx || !ctx->ready || !bgr320 || !out || !out_n || max_out <= 0) return -1;
  *out_n = 0;

  memcpy(ctx->input_mem->virt_addr, bgr320, (size_t)RKNN_FACE_INPUT * RKNN_FACE_INPUT * 3);
  ret = rknn_run(ctx->ctx, NULL);
  if (ret != RKNN_SUCC) {
    log_warn("face", "rknn_run failed %d", ret);
    return -2;
  }

  for (i = 0; i < ctx->io_num.n_output; i++) {
    int elems = ctx->out_elems[i];
    if (dequant_to_float(&ctx->out_attr[i], ctx->out_mem[i]->virt_addr, ctx->out_f[i], elems) != 0) {
      return -3;
    }
  }

  if (ctx->io_num.n_output >= 9) {
    for (i = 0; i < 3; i++) {
      decode_stride(ctx->out_f[i], ctx->out_f[3 + i], ctx->out_f[6 + i], grids[i], grids[i],
                    strides[i], score_thr, out, &n, max_out, &max_score);
    }
  }

  if (n > 1) nms_inplace(out, &n, nms_iou);
  if (n > max_out) n = max_out;
  *out_n = n;
  if ((n == 0 && max_score > 0.f) || n > 0) {
    static int dbg;
    if (dbg < 8 || n > 0) {
      if (dbg < 8) dbg++;
      if (n > 0) {
        log_info("face", "det n=%d max=%.3f best=%.3f box=%.0f,%.0f,%.0fx%.0f", n, max_score,
                 out[0].score, out[0].x1, out[0].y1, out[0].x2 - out[0].x1, out[0].y2 - out[0].y1);
      } else if (dbg <= 8) {
        log_info("face", "det n=0 max=%.3f thr=%.3f", max_score, score_thr);
      }
    }
  }
  return 0;
}
