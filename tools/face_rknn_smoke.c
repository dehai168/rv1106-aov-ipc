/* face_rknn_smoke: load .rknn, zero-copy IO (RV1106), timed inferences (F0). */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "rknn_api.h"

static long long now_us(void)
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (long long)tv.tv_sec * 1000000LL + tv.tv_usec;
}

static unsigned char *read_file(const char *path, int *size_out)
{
  FILE *fp = fopen(path, "rb");
  long sz;
  unsigned char *buf;
  if (!fp) {
    fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
    return NULL;
  }
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

int main(int argc, char **argv)
{
  const char *model_path = "/userdata/models/yunet_320.rknn";
  int loops = 50;
  int model_len = 0;
  unsigned char *model = NULL;
  rknn_context ctx = 0;
  int ret;
  rknn_input_output_num io_num;
  rknn_tensor_attr *input_attrs = NULL;
  rknn_tensor_attr *output_attrs = NULL;
  rknn_tensor_mem **input_mems = NULL;
  rknn_tensor_mem **output_mems = NULL;
  uint32_t i, n;
  long long t0, t1, t_load0, t_load1;
  double sum_ms = 0.0, min_ms = 1e9, max_ms = 0.0;
  int ok_loops = 0;

  if (argc >= 2) model_path = argv[1];
  if (argc >= 3) loops = atoi(argv[2]);
  if (loops < 1) loops = 1;

  printf("face_rknn_smoke model=%s loops=%d\n", model_path, loops);

  t_load0 = now_us();
  model = read_file(model_path, &model_len);
  if (!model) return 2;
  printf("model_bytes=%d\n", model_len);

  ret = rknn_init(&ctx, model, model_len, 0, NULL);
  free(model);
  if (ret != RKNN_SUCC) {
    fprintf(stderr, "rknn_init failed ret=%d\n", ret);
    return 3;
  }
  t_load1 = now_us();
  printf("rknn_init_ok load_ms=%.2f\n", (t_load1 - t_load0) / 1000.0);

  memset(&io_num, 0, sizeof(io_num));
  ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
  if (ret != RKNN_SUCC || io_num.n_input < 1 || io_num.n_output < 1) {
    fprintf(stderr, "query io_num failed ret=%d\n", ret);
    rknn_destroy(ctx);
    return 4;
  }
  printf("n_input=%u n_output=%u\n", io_num.n_input, io_num.n_output);

  input_attrs = (rknn_tensor_attr *)calloc(io_num.n_input, sizeof(rknn_tensor_attr));
  output_attrs = (rknn_tensor_attr *)calloc(io_num.n_output, sizeof(rknn_tensor_attr));
  input_mems = (rknn_tensor_mem **)calloc(io_num.n_input, sizeof(rknn_tensor_mem *));
  output_mems = (rknn_tensor_mem **)calloc(io_num.n_output, sizeof(rknn_tensor_mem *));
  if (!input_attrs || !output_attrs || !input_mems || !output_mems) {
    ret = 6;
    goto cleanup;
  }

  for (i = 0; i < io_num.n_input; i++) {
    input_attrs[i].index = i;
    ret = rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &input_attrs[i], sizeof(rknn_tensor_attr));
    if (ret != RKNN_SUCC) {
      fprintf(stderr, "query input %u failed %d\n", i, ret);
      goto cleanup;
    }
    printf("input%u name=%s dims=%u,%u,%u,%u fmt=%d type=%d size=%u stride_size=%u\n", i,
           input_attrs[i].name, input_attrs[i].dims[0], input_attrs[i].dims[1],
           input_attrs[i].dims[2], input_attrs[i].dims[3], input_attrs[i].fmt,
           input_attrs[i].type, input_attrs[i].size, input_attrs[i].size_with_stride);
    /* Host feeds UINT8 NHWC; runtime converts for NPU. */
    input_attrs[i].type = RKNN_TENSOR_UINT8;
    input_attrs[i].fmt = RKNN_TENSOR_NHWC;
    input_mems[i] = rknn_create_mem(ctx, input_attrs[i].size_with_stride
                                            ? input_attrs[i].size_with_stride
                                            : input_attrs[i].size);
    if (!input_mems[i]) {
      fprintf(stderr, "create input mem %u failed\n", i);
      ret = 7;
      goto cleanup;
    }
    memset(input_mems[i]->virt_addr, 0x5a, input_mems[i]->size);
    ret = rknn_set_io_mem(ctx, input_mems[i], &input_attrs[i]);
    if (ret != RKNN_SUCC) {
      fprintf(stderr, "set_io_mem input %u failed %d\n", i, ret);
      goto cleanup;
    }
  }

  for (i = 0; i < io_num.n_output; i++) {
    output_attrs[i].index = i;
    ret = rknn_query(ctx, RKNN_QUERY_NATIVE_OUTPUT_ATTR, &output_attrs[i], sizeof(rknn_tensor_attr));
    if (ret != RKNN_SUCC) {
      fprintf(stderr, "query native output %u failed %d\n", i, ret);
      goto cleanup;
    }
    printf("output%u name=%s dims=%u,%u,%u,%u fmt=%d type=%d size=%u stride_size=%u\n", i,
           output_attrs[i].name, output_attrs[i].dims[0], output_attrs[i].dims[1],
           output_attrs[i].dims[2], output_attrs[i].dims[3], output_attrs[i].fmt,
           output_attrs[i].type, output_attrs[i].size, output_attrs[i].size_with_stride);
    output_mems[i] = rknn_create_mem(ctx, output_attrs[i].size_with_stride
                                              ? output_attrs[i].size_with_stride
                                              : output_attrs[i].size);
    if (!output_mems[i]) {
      fprintf(stderr, "create output mem %u failed\n", i);
      ret = 8;
      goto cleanup;
    }
    ret = rknn_set_io_mem(ctx, output_mems[i], &output_attrs[i]);
    if (ret != RKNN_SUCC) {
      fprintf(stderr, "set_io_mem output %u failed %d\n", i, ret);
      goto cleanup;
    }
  }

  for (n = 0; n < (uint32_t)loops; n++) {
    t0 = now_us();
    ret = rknn_run(ctx, NULL);
    t1 = now_us();
    if (ret != RKNN_SUCC) {
      fprintf(stderr, "run failed %d at iter %u\n", ret, n);
      break;
    }
    {
      double ms = (t1 - t0) / 1000.0;
      sum_ms += ms;
      if (ms < min_ms) min_ms = ms;
      if (ms > max_ms) max_ms = ms;
      if (n < 3 || n == (uint32_t)loops - 1) printf("iter=%u infer_ms=%.2f\n", n, ms);
    }
    ok_loops++;
  }

  if (ok_loops == loops) {
    printf("PASS loops=%d avg_ms=%.2f min_ms=%.2f max_ms=%.2f\n", loops, sum_ms / loops, min_ms,
           max_ms);
    ret = 0;
  } else {
    printf("FAIL after %d/%d\n", ok_loops, loops);
    if (ret == RKNN_SUCC) ret = 9;
  }

cleanup:
  if (input_mems) {
    for (i = 0; i < io_num.n_input; i++) {
      if (input_mems[i]) rknn_destroy_mem(ctx, input_mems[i]);
    }
    free(input_mems);
  }
  if (output_mems) {
    for (i = 0; i < io_num.n_output; i++) {
      if (output_mems[i]) rknn_destroy_mem(ctx, output_mems[i]);
    }
    free(output_mems);
  }
  free(input_attrs);
  free(output_attrs);
  if (ctx) rknn_destroy(ctx);
  return ret == RKNN_SUCC ? 0 : (ret > 0 ? ret : 10);
}
