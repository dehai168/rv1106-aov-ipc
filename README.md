# rv1106-aov-ipc

RV1106 AOV IPC project.

### 开发文档（agent 必读，按顺序）
1. [docs/01-architecture.md](docs/01-architecture.md) — 系统架构与技术方案
2. [docs/02-dev-process.md](docs/02-dev-process.md) — 开发流程、规范与持续迭代方法
3. [docs/03-todolist.md](docs/03-todolist.md) — 任务清单与进度（单一事实源）
4. [docs/04-worklog.md](docs/04-worklog.md) — 工作日志
5. [docs/05-api.md](docs/05-api.md) — REST API 契约

### 硬件环境
1. luckfox pico Max 开发板
    芯片	RV1106G3
    处理器	Cortex A7 1.2GHz
    NPU	1TOPS,支持int4、in8、int16
    ISP	最大输入5M @30fps
    内存  256MB DDR3L
    Wi-Fi+蓝牙	无
    摄像头接口	MIPI CSI 2-lane
    DPI接口	无
    POE接口	无
    喇叭接口 无
    USB	USB 2.0 Host/Device
    GPIO	26 个 GPIO 引脚
    网口	10/100M Ethernet controller and embedded PHY
    默认存储介质	SPI NAND FLASH(256MB)
2. 该开发板已经连接 MIS5001 5MP Camera
3. 该开发板已经插上 tf 卡64GB
4. 该开发板带了纽扣电池且已连接
5. 该开发板通过 adb 已经与电脑连接,可以直接通过adb进行操控
### 软件环境
1. 开发板的开发资料可以参考：https://wiki.luckfox.com/Luckfox-Pico-RV1106 
2. 开发板的下载参考资料可以参考: https://github.com/LuckfoxTECH/Luckfox-Pico-docs
3. 当前开发电脑是windows，已经安装了ubuntu 的子系统，可以进入该进行搭建编译环境，登录用户名 user 密码 12345678
4. 如果当前系统不满足开发，可以把代码clone到 ubuntu的子系统，边修改边编译边review再提交
### 功能目标
1. 该ipc具备移动物体检测并自动快起进行录像
2. 文件系统使用用户插上tf卡就能够从pc访问录像文件，录像文件格式是mp4格式，按照录像时长参数进行分段录制和按照存储空间进行周期覆盖
3. 该ipc具备一个web管理功能，如果通过网口访问网关以后能够打开ipc的web管理界面，该管理界面具备以下基本功能
   3.1 基本的登录和注销
   3.2 实时画面预览
   3.3 网络参数配置
   3.4 图像和视频流参数配置
   3.5 存储与回放配置
   3.6 告警与智能分析
   3.7 系统管理和安全
4.具备rtc校时功能
### 开发要求
1. 所有的文档资料都放到docs目录
2. 先设计再开发，开发过程持久化自身进度，开发完成以后要review代码功能
3. 涉及到高危操作要征求我的同意