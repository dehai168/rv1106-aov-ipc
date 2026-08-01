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
| POST | `/api/v1/auth/login` | 否 | body: `{username,password}` → `{token,must_change,username}`；失败 `msg`：`请输入用户名和密码` / `用户名或密码错误` |
| POST | `/api/v1/auth/logout` | 否* | 清除 token/cookie |
| POST | `/api/v1/auth/password` | 是 | body: `{old_password,new_password}`（new≥6） |
| GET | `/api/v1/auth/me` | 是 | `{username,must_change}` |

### record（T7.6 录像计划）

- `GET /api/v1/record`：`mode`（`motion|continuous|schedule`）、`segment_sec`、`pre_record_sec`、`quiet_sec`、`recycle_free_percent`、`schedule{enabled,grid}`，以及运行态 `runtime_mode/running/policy_hold`
- `POST /api/v1/record`：保存同上字段；`apply` 默认 true
  - `motion`：预览门控事件录像（现网）
  - `continuous`：媒体常驻 + 连续分段
  - `schedule`：7×24 `grid`（168 字符）控制连续录像开/关；`schedule.enabled=0` 视为全天
- AOV 激活时 POST → `2601`；离开 AOV 后重新 apply 已保存策略
- 错误：`2401` 保存失败；`2402` apply 失败

### preview（T4.3 / T7.2）

| 方法 | 路径 | 鉴权 | 说明 |
|---|---|---|---|
| GET | `/api/v1/preview/ws` | 是（Cookie / Bearer / `?token=`） | WebSocket：`?stream=sub\|main`（默认 sub）。首包文本 `{"codec":"avc1.…","stream":"sub\|main"}`，随后 fMP4。sub=VENC1；main=VI2→VENC3 H.264（≤1280×720，仅有 main 客户端时启编码） |

| POST | `/api/v1/preview/snapshot` | 是 | 手动抓图 → `{file,url}`；JPEG 落盘 `snapshots/YYYYMMDD_HHMMSS.jpg`；下载用 `GET /api/v1/alarm/snapshot?file=<file>` |

- 首个预览客户端连接时按需 `media_stream_start`；断开最后一个客户端后停止媒体（录像 policy_hold 时除外）。
- 只给当前 `stream` 做 fMP4；主预览默认 idle，避免与连续录像抢 ISP/编码。
- 前端 MSE（`video/mp4; codecs="…"`）播放；断线自动重连；积压时丢旧分片追直播。
- 抓图需媒体管道就绪（会按需 `preview_ensure_pipeline`）；AOV 激活时返回 `2601`。

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
| GET | `/api/v1/video/osd` | 是 | OSD：`enabled/show_time/show_name/name/pos_x/pos_y` |
| POST | `/api/v1/video/osd` | 是 | 保存并可选 RGN 烧录 apply（主/子/主预览 VENC） |

- image body：`brightness/contrast/saturation`（0–100）、`mirror/flip`（bool）、`apply`（默认 true）
  - 应用：AIQ `setMirrorFlip(..., skip=4)` + VI 通道 `bMirror/bFlip`（0/1/2）
- encode body：`main`/`sub` 各含 `w/h/fps/bitrate_kbps/gop`，`apply`（默认 true）
- osd：`name` 支持中文（FreeType + `/userdata/fonts/simsun_cn_3000.ttf`）；`pos_x/pos_y` 为画面左上角像素，**须 16 对齐**（Rockchip RGN）；`name` 空则用 `device.name`；未起流时仅保存
- 错误：`2101` 保存失败；`2102` 应用失败（media 未起流时 image/osd 仅保存不报错）

### storage（T4.6）
- 已实现：
  - `GET /api/v1/storage/status`
  - `GET /api/v1/storage/records`（可选 `date=YYYYMMDD`，`limit<=200`，`offset` 分页）
  - `GET /api/v1/storage/download?path=<YYYYMMDD>/<file>.mp4`（直接返回 MP4，可被浏览器 Range 读取）
  - `POST /api/v1/storage/format`：body `{"confirm":true}`，软清空 `records/`、`snapshots/`、`alarms/`（非 mkfs）；会先停 motion 自动化
- 错误：`2402` 状态/文件失败；`2403` format 失败

- `/records` 返回：
  - `data.records[]`：`{ path, name, mtime, size }`
  - `data.total` / `data.count` / `data.offset` / `data.limit`
- `/format` 返回：`data.deleted`（删除文件数）、`data.note`
- **录像落盘**：mux 先写 `records/.stage/` 再 rename/promote 到日目录；长分段（如 300s）勿依赖 `/tmp`（tmpfs 易写满导致无 `moov`）

### alarm（T4.7）
- 已实现：
  - `GET /api/v1/alarm/motion`：detect 配置 + `running/motion_count/last_event`
  - `POST /api/v1/alarm/motion`：保存配置（可选 `apply` 立即重启 runtime）
  - `GET /api/v1/alarm/events?limit<=200`：读取 `/alarms/alarms.log` 最近 N 条（JSONL，含 `snapshot`）
  - `GET /api/v1/alarm/snapshot?file=YYYYMMDD_HHMMSS.jpg`：下载告警抓图（鉴权，白名单文件名）
- **侦测 runtime**：`detect.enabled` 且媒体已起时，**continuous / schedule / motion** 策略下都会跑 IVS（VI2）。`link.record` 仅在录像策略为 `motion` 时触发 `record_on_motion`（不会把连续录像切成事件模式）。
- `regions`：最多 4 个 `{ enabled, x, y, w, h }`（侦测帧像素，当前与 VI2 一致，约 ≤1280×720）；任一启用且与事件框相交即通过；全部未启用 → 全画面
- `region`：兼容字段，等于 `regions[0]`
- `schedule`：`{ enabled, start_min, end_min, days, grid }`
  - `grid`：长度 168 的 `"0"|"1"` 字符串，行优先 周一..周日 × hour0..23；合法时优先于 start/end/days
  - `days` bit0=周一 … bit6=周日；`enabled=0` 表示全天
- `link`：`{ record, snapshot }`，运动触发时是否联动录像/抓图（默认均 true）
- 抓图：硬件 JPEG 写入 `/mnt/sdcard/snapshots/`，事件字段 `snapshot` 为文件名
- 错误：`2401` 保存失败；`2402` apply/文件失败

### system（T4.8）
- 已实现：
  - `GET /api/v1/system/device_name`：**无需鉴权**，仅返回 `{device_name}`（登录页/品牌展示）
  - `GET /api/v1/system/info`：设备名、主机名、型号、版本、运行时长、内存
  - `POST /api/v1/system/info`：body `{device_name}`（1–32，无控制字符）
  - `GET /api/v1/system/time`：当前 Unix 时间 + 时区/NTP 配置
  - `POST /api/v1/system/time`：保存时区/NTP；`unix_time` 手动校时；`apply_ntp: true` 且 `ntp_enabled` 时尝试 `ntpdate`/`ntpd -q`
- 进程启动（T5.1）：`system_time_init` 在 `config_init` 后执行 `hwclock -s`、应用时区；`system.ntp.enabled` 时后台线程按 `system.ntp.interval_min`（默认 60）同步并 `hwclock -w`
  - `POST /api/v1/system/reboot`：body `{"confirm":true}`，fork 后 1s 执行 reboot
  - `GET|POST /api/v1/system/reboot_schedule`：`{enabled,hour,minute,days}`；`days` 长度 7（日..六，`0/1`）；本地时刻匹配则每天最多重启一次
  - `POST /api/v1/system/reset`：body `{"confirm":true}`，删除用户 `ipc_config.json` 并 `config_reload`（不清 TF 录像）；会话 token 失效
  - `GET /api/v1/system/log?file=ipc_app.log`：下载白名单日志
  - `GET /api/v1/system/log?file=ipc_app.log&tail=N`：JSON `{file,tail,lines[]}`（N≤500）
- 用户管理：沿用 `POST /api/v1/auth/password`（`PasswordView`）
- 错误：`2501` time set / reset 失败；`2502` info/log/reboot/ntp sync 失败

### aov（变帧率 AOV，T6.4）

- `GET /api/v1/aov/status`：模式、状态机、循环计数、上次唤醒原因/耗时、配置摘要
- `GET|POST /api/v1/aov/config`：`enabled/suspend_ms/event_fps/quiet_sec/event_record_sec/down_eth/allow_mem_suspend`
- `POST /api/v1/aov/enter`：body `{"confirm":true}`（断开预览）
- `POST /api/v1/aov/leave`：回常电
- 预览：AOV 激活时 `GET /api/v1/preview/ws` → `2601`
- CLI：`ipc_app --aov status|enter|leave|probe|loop N [--media]`；`--aov-probe`
- 错误：`2601` 预览不可用；`2602` 配置保存失败；`2603/2604` enter/leave 失败
- **注意**：`allow_mem_suspend` 默认 `false`（dry-run）；板上 resume 打通后再开
