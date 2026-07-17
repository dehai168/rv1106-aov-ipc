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

## 鉴权

- 登录成功后返回 `data.token`，并设置 Cookie `ipc_token`（HttpOnly）。
- 后续请求可用 Cookie，或 `Authorization: Bearer <token>`。
- 默认账号：`admin` / `admin`；`must_change=true` 时前端应引导改密。

## 域与端点

### auth（T4.1）

| 方法 | 路径 | 鉴权 | 说明 |
|---|---|---|---|
| POST | `/api/v1/auth/login` | 否 | body: `{username,password}` → `{token,must_change,username}` |
| POST | `/api/v1/auth/logout` | 否* | 清除 token/cookie |
| POST | `/api/v1/auth/password` | 是 | body: `{old_password,new_password}`（new≥6） |
| GET | `/api/v1/auth/me` | 是 | `{username,must_change}` |

### preview（T4.3）

| 方法 | 路径 | 鉴权 | 说明 |
|---|---|---|---|
| GET | `/api/v1/preview/ws` | 是（Cookie / Bearer / `?token=`） | WebSocket：首包文本 `{"codec":"avc1.xxxxxx"}`，随后二进制 fMP4 init（ftyp+moov），再推 moof/mdat 分片（子码流 H.264） |

- 首个预览客户端连接时按需 `media_stream_start`（不启 detect 通道）；断开最后一个客户端后停止媒体。
- 前端 MSE（`video/mp4; codecs="…"`）播放；断线自动重连。

### network（T4.4）

| 方法 | 路径 | 鉴权 | 说明 |
|---|---|---|---|
| GET | `/api/v1/network/config` | 是 | 返回期望配置 + 运行时状态 |
| POST | `/api/v1/network/config` | 是 | 保存并可选立即应用 |

- body / data 字段：`iface`（仅 `eth0`）、`mode`（`dhcp`\|`static`）、`ip`、`netmask`、`gateway`、`dns1`、`dns2`、`apply`（bool，默认 true）
- 额外只读：`link`、`current_ip`、`current_netmask`、`current_gateway`、`usb0_ip`（usb0 不由此 API 改写）
- 错误：`1002` 参数非法；`2401` 保存失败；`2402` 应用失败
- 持久化：`ipc_config.json` + `/etc/network/interfaces` + `/etc/resolv.conf`

### video（T4.5）

| 方法 | 路径 | 鉴权 | 说明 |
|---|---|---|---|
| GET | `/api/v1/video/image` | 是 | 亮度/对比度/饱和度/镜像/翻转（0–100 或 bool） |
| POST | `/api/v1/video/image` | 是 | 保存并可选立即应用（RKAIQ） |
| GET | `/api/v1/video/encode` | 是 | 主/子码流参数 + `stream_up` |
| POST | `/api/v1/video/encode` | 是 | 保存并可选 `media_stream_apply` |

- image body：`brightness/contrast/saturation`（0–100）、`mirror/flip`（bool）、`apply`（默认 true）
- encode body：`main`/`sub` 各含 `w/h/fps/bitrate_kbps/gop`，`apply`（默认 true）
- 错误：`2101` 保存失败；`2102` 应用失败（media 未起流时 image 仅保存不报错）

### storage（T4.6）
- 已实现：
  - `GET /api/v1/storage/status`
  - `GET /api/v1/storage/records`（可选 `date=YYYYMMDD`，可选 `limit<=200`）
  - `GET /api/v1/storage/download?path=<YYYYMMDD>/<file>.mp4`（直接返回 MP4，可被浏览器 Range 读取）
  - `POST /api/v1/storage/format`：body `{"confirm":true}`，软清空 `records/`、`snapshots/`、`alarms/`（非 mkfs）；会先停 motion 自动化
- 错误：`2402` 状态/文件失败；`2403` format 失败

- `/records` 返回：
  - `data.records[]`：`{ path, name, mtime, size }`
- `/format` 返回：`data.deleted`（删除文件数）、`data.note`

### alarm（T4.7）
- 已实现：
  - `GET /api/v1/alarm/motion`：detect 配置（含 `region`/`schedule`）+ `running/motion_count/last_event`
  - `POST /api/v1/alarm/motion`：保存配置（可选 `apply` 立即重启 runtime）
  - `GET /api/v1/alarm/events?limit<=200`：读取 `/alarms/alarms.log` 最近 N 条（JSONL，含 `snapshot`）
  - `GET /api/v1/alarm/snapshot?file=YYYYMMDD_HHMMSS.jpg`：下载告警抓图（鉴权，白名单文件名）
- `region`：`{ enabled, x, y, w, h }`，坐标系为侦测帧像素（约 640×360）；关闭或 w/h≤0 表示全画面
- `schedule`：`{ enabled, start_min, end_min, days }`；`days` bit0=周一 … bit6=周日；`end_min` 可达 1440；结束&lt;开始表示跨夜
- 抓图：触发时灰度 JPEG 写入 `/mnt/sdcard/snapshots/`，事件字段 `snapshot` 为文件名
- 错误：`2401` 保存失败；`2402` apply/文件失败

### system（T4.8）
- 已实现：
  - `GET /api/v1/system/info`：设备名、主机名、型号、版本、运行时长、内存
  - `GET /api/v1/system/time`：当前 Unix 时间 + 时区/NTP 配置
  - `POST /api/v1/system/time`：保存时区/NTP；`unix_time` 手动校时；`apply_ntp: true` 且 `ntp_enabled` 时尝试 `ntpdate`/`ntpd -q`
- 进程启动（T5.1）：`system_time_init` 在 `config_init` 后执行 `hwclock -s`、应用时区；`system.ntp.enabled` 时后台线程按 `system.ntp.interval_min`（默认 60）同步并 `hwclock -w`
  - `POST /api/v1/system/reboot`：body `{"confirm":true}`，fork 后 1s 执行 reboot
  - `POST /api/v1/system/reset`：body `{"confirm":true}`，删除用户 `ipc_config.json` 并 `config_reload`（不清 TF 录像）；会话 token 失效
  - `GET /api/v1/system/log?file=ipc_app.log`：下载白名单日志（默认 `log.dir/ipc_app.log`）
- 用户管理：沿用 `POST /api/v1/auth/password`（`PasswordView`）
- 错误：`2501` time set / reset 失败；`2502` info/log/reboot/ntp sync 失败
