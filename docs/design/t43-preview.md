# T4.3 实时预览

```
VENC1 H.264 Annex-B → fmp4_mux → 有界队列 → MG_EV 轮询 mg_ws_send
浏览器 MediaSource / SourceBuffer，断线重连
```

- 路径：`GET /api/v1/preview/ws`（Cookie / Bearer / `?token=`）
- 首包文本：`{"codec":"avc1.xxxxxx"}`，随后二进制 init，再推 fragment
- 禁止在 VENC 回调里直接 `mg_ws_send`；`fmp4_mux` 产出进队列，web 线程 poll 发送
- 首个 WS 客户端时按需 `media_stream_start`（detect 关闭）；`media_stream_request_idr(1)`
- 实现：`src/web/fmp4_mux.c`、`web_service.c` preview_*；前端 `PreviewView.vue`
