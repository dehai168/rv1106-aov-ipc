# T0.3 公共基础件设计

## 目标
日志文件落盘、配置中心（默认+用户合并、原子写入）、事件总线（已有，保持同步发布）。

## 接口

### log
- `log_init_ex(level, log_dir)`：创建目录，打开 `/userdata/log/ipc_app.log`
- 同时写 stderr + 文件；文件失败不阻断启动

### config (`src/system/config_service.h`)
- 默认：`/oem/usr/share/ipc/default_config.json`（开发期也可 `/userdata/default_config.json`）
- 用户：`/userdata/ipc_config.json`
- `config_init(default_path, user_path)`：读默认 → 若用户存在则 deep merge 覆盖
- `config_get_string/int/bool(path, out, default)`：点分路径，如 `log.level`
- `config_set_string/int/bool` + `config_save()`：写临时文件再 rename

### 自测
`ipc_app --self-test`：写配置 → 重载 → 校验；写日志文件 → 检查存在；事件总线收发。
