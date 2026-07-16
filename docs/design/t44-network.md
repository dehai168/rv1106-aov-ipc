# T4.4 网络参数配置

- 管理口：`eth0`（EMAC）。`usb0` 仅展示当前 IP，不由本页改写（避免断 adb）。
- 持久化：`ipc_config.json` → `network.*`；同时写 `/etc/network/interfaces` + `/etc/resolv.conf` 供开机 `ifup`。
- API：`GET/POST /api/v1/network/config`
- POST body 含 `apply:true` 时立即生效；前端改静态 IP 前二次确认。
