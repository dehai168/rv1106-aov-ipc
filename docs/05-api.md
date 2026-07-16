# REST API 契约

> 本文档是前后端唯一契约。API 实现或修改必须同步更新本文档（同一 commit）。
> 通用约定见 `02-dev-process.md` §3.3：前缀 `/api/v1`，响应 `{"code":0,"msg":"ok","data":{...}}`，除 auth/login 外均需鉴权。

## 错误码约定

| code | 含义 |
|---|---|
| 0 | 成功 |
| 1001 | 未登录/鉴权失败 |
| 1002 | 参数错误 |
| 1003 | 权限不足 |
| 2xxx | 各业务域错误（media 21xx / record 22xx / detect 23xx / network 24xx / system 25xx） |

## 域与端点（随开发登记）

### auth（T4.1）
- 待实现登记：`POST /api/v1/auth/login`、`POST /api/v1/auth/logout`、`POST /api/v1/auth/password`

### preview（T4.3）
- 待实现登记：`GET /api/v1/preview/ws`（WebSocket，fMP4 分片）

### network（T4.4）
- 待实现登记：`GET/POST /api/v1/network/config`

### video（T4.5）
- 待实现登记：`GET/POST /api/v1/video/image`、`GET/POST /api/v1/video/encode`

### storage（T4.6）
- 待实现登记：`GET /api/v1/storage/status`、`POST /api/v1/storage/format`、`GET /api/v1/storage/records`、`GET /api/v1/storage/download`

### alarm（T4.7）
- 待实现登记：`GET/POST /api/v1/alarm/motion`、`GET /api/v1/alarm/events`

### system（T4.8）
- 待实现登记：`GET /api/v1/system/info`、`GET/POST /api/v1/system/time`、`POST /api/v1/system/reboot`、`POST /api/v1/system/reset`、`GET /api/v1/system/log`
