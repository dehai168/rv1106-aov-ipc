# 开发流程、开发规范与 Agent 持续迭代方法

> 所有开发 agent 在开始任何工作前必须先读：`docs/01-architecture.md`（做什么）、本文档（怎么做）、`docs/03-todolist.md`（当前进度与下一步）。

## 1. 环境与工具链

### 1.1 三个环境的分工

| 环境 | 用途 |
|---|---|
| Windows（工作区 `D:\CodeProject\rv1106-aov-ipc`） | 文档、前端开发、git 主仓库、adb 操控开发板 |
| WSL Ubuntu（用户 `user`，密码 `12345678`） | SDK 交叉编译环境；从 GitHub clone 本仓库进行 C/C++ 编译 |
| 开发板（adb 直连） | 部署与真机验证 |

- Windows 侧进入 WSL：`wsl -u user`（或 `wsl -d <发行版名> -u user`）。
- WSL 内代码路径约定：`~/work/rv1106-aov-ipc`（本仓库 clone）、`~/work/luckfox-pico`（官方 SDK）。
- 两侧代码**只通过 git 同步**（Windows 改完 push，WSL pull 后编译；或反之），禁止手工拷贝造成分叉。

### 1.2 首次环境搭建（一次性）

1. WSL 内安装依赖并 clone 官方 SDK `LuckfoxTECH/luckfox-pico`，按 wiki 完成 buildroot 环境与交叉编译链 `arm-rockchip830-linux-uclibcgnueabihf` 的准备；
2. 编译 SDK 例程验证工具链可用；
3. 在本仓库 `scripts/` 固化：`build.sh`（WSL 内交叉编译 ipc_app）、`deploy.ps1`/`deploy.sh`（adb push 到板 + 重启 app）、`env.md` 记录实际路径与版本。

### 1.3 真机操作约定（adb）

- 部署：`adb push build/ipc_app /userdata/` → `adb shell chmod +x ...` → 运行；
- 看日志：`adb shell` 后跑前台，或 `logread`/应用日志文件 `/userdata/log/`；
- **高危操作必须先征求用户同意**，包括：烧录/更新固件与内核、修改分区、格式化 TF 卡、改 uboot/env、删除系统文件、修改板上网络配置导致可能失联的操作。普通的 push 应用、重启 app、重启系统属于常规操作。

## 2. 开发流程（每个任务的标准循环）

```
读进度(03-todolist.md + 04-worklog.md)
  → 领取一个任务（标记 in_progress）
  → 设计（大任务先在 docs/design/ 写简短设计，列接口与验收标准）
  → 编码（Windows 或 WSL，见 1.1 分工）
  → 编译（WSL 交叉编译通过，零警告为目标）
  → 真机验证（adb 部署，按任务的验收标准逐条测）
  → 自 review（对照 §3 规范 + 验收标准检查 diff）
  → 提交 push（见 §4）
  → 更新 03-todolist.md（done + 一句话结果）和 04-worklog.md
  → 下一个任务
```

硬性规则：

- **先设计再开发**：涉及新模块或跨模块接口的任务，先写/更新设计文档再动代码；
- **小步提交**：一个任务一个或几个 commit，禁止巨型混合提交；
- **验收标准前置**：任务开始时如果 todolist 中的验收标准不明确，先补充明确再开发；
- **不跳任务依赖**：todolist 中标注的依赖未完成不得开始。

## 3. 开发规范

### 3.1 C/C++（板端）

- C11 / C++17，CMake 构建，交叉编译工具链文件 `cmake/toolchain-rv1106.cmake`；
- 命名：文件与函数 `snake_case`，类型 `PascalCase`，宏与常量 `UPPER_CASE`，模块 API 前缀（如 `media_`, `rec_`, `det_`, `web_`, `sys_`）；
- 每个模块对外只暴露一个头文件（`src/<mod>/<mod>_service.h`），内部实现不跨模块 include；
- 错误处理：API 返回 `0/负错误码`，禁止吞错误；资源获取失败必须走统一清理路径（goto cleanup 模式可用）；
- 日志用统一 `log_xxx()` 宏（DEBUG/INFO/WARN/ERROR），禁止裸 printf；
- 线程：模块内自管理线程，跨模块通信只走事件总线或模块 API，禁止跨模块摸内部数据；
- 注释解释"为什么"，不写复述代码的注释；
- 内存：长期运行零泄漏是验收项，新模块需通过至少 30 分钟运行观察 RSS 稳定。

### 3.2 Web 前端

- Vue3 + Vite + TypeScript；组件 `PascalCase.vue`；API 封装统一在 `web-ui/src/api/`；
- 构建产物必须检查体积（gzip 总量 < 2MB），不引入大型 UI 库全量包（按需引入或用轻量库）。

### 3.3 REST API

- 路径 `/api/v1/<domain>/<action>`，域：`auth/preview/network/video/storage/alarm/system`；
- 响应统一 `{"code":0,"msg":"ok","data":{...}}`，非 0 为错误码；
- 所有 API（除登录）需 token 鉴权；API 一旦在 `docs/05-api.md` 中登记即视为契约，改动需同步更新文档与前端。

### 3.4 文档

- 全部在 `docs/`：`01-architecture.md`（架构）、`02-dev-process.md`（本文）、`03-todolist.md`（任务与进度）、`04-worklog.md`(工作日志)、`05-api.md`（API 契约）、`design/`（各模块设计）、`env.md`（环境实况）；
- 文档跟随代码同 commit 更新，不允许"代码先行文档欠账"。

## 4. Git 规范

- 分支：直接在 `main` 上小步提交（单 agent 串行开发）；如并行多 agent，各开 `feat/<module>` 分支，完成后合入 main；
- Commit message：`<type>(<scope>): <说明>`，type ∈ feat/fix/docs/refactor/build/test/chore，scope 为模块名，说明用中文或英文均可但要说清"为什么/做了什么"；
- 每完成一个 todolist 任务至少 push 一次到 GitHub（这是进度持久化的一部分）；
- 禁止提交：编译产物、SDK 拷贝、密钥、超大二进制（模型文件放 `models/` 且单个 < 20MB，更大的记录下载方式）。

## 5. Agent 持续迭代方法（进度持久化）

Agent 会话随时可能中断，**仓库本身就是记忆**。约定如下：

### 5.1 单一事实源

- `docs/03-todolist.md`：任务清单 + 状态（`[ ]` 待办 / `[~]` 进行中 / `[x]` 完成 / `[!]` 阻塞），每个任务带验收标准；
- `docs/04-worklog.md`：追加式工作日志，每个工作会话结束前追加一条：

```markdown
## 2026-07-16 session N
- 完成：T2.1 VI+VENC 双码流出流（commit abc1234）
- 进行中：T2.2 已写完 muxer 初始化，卡在 moov 写入时机，下一步看 minimp4 的 fragmented 模式
- 阻塞/待用户：无
- 板上状态：ipc_app 已部署 /userdata，跑主码流正常
```

### 5.2 会话启动协议（每个新 agent 会话必做）

1. `git pull`，读 `03-todolist.md` 找当前 `[~]`/下一个 `[ ]`；
2. 读 `04-worklog.md` 最后 1–2 条了解上下文与坑；
3. 若涉及板子，先 `adb devices` + `adb shell ps | grep ipc` 确认板上实际状态；
4. 继续任务；任何时候发现文档与代码不符，以代码为准并顺手修文档。

### 5.3 会话结束协议

1. 代码可编译（不留编译不过的状态在 main）；
2. commit + push；
3. 更新 todolist 状态 + 追加 worklog；
4. 未完成的思路、失败的尝试也要写进 worklog（避免下个会话重蹈覆辙）。

### 5.4 遇到阻塞

- 技术阻塞：在 worklog 记录已尝试方案，任务标 `[!]` 并写明阻塞原因，跳到无依赖的其他任务；
- 需要用户决策/高危操作：明确向用户提问，等待期间可做其他任务。

## 6. 测试与验收

- 每个任务的验收标准写在 todolist 里，验收必须在真机完成（编译通过 ≠ 完成）；
- 里程碑级验收（见 todolist 的 M1–M5）由 agent 跑完整场景并在 worklog 记录证据（日志片段、文件列表、截图路径）；
- 回归习惯：动了 media/record 链路后，至少重测"侦测→录像→PC 读卡播放"主链路一次。
