#ifndef IPC_FACE_RKNN_FACE_H
#define IPC_FACE_RKNN_FACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RKNN_FACE_INPUT 320
#define RKNN_FACE_MAX_DET 64

typedef struct {
  float x1, y1, x2, y2; /* pixels in model input 320x320 space */
  float score;
} RknnFaceBox;

typedef struct RknnFaceCtx RknnFaceCtx;

RknnFaceCtx *rknn_face_create(const char *model_path);
void rknn_face_destroy(RknnFaceCtx *ctx);

/* bgr: contiguous HWC uint8, size must be 320*320*3 */
int rknn_face_infer(RknnFaceCtx *ctx, const uint8_t *bgr320, float score_thr, float nms_iou,
                    RknnFaceBox *out, int max_out, int *out_n);

#ifdef __cplusplus
}
#endif

#endif
