# ESP32-CAM 四轮摄像小车

这是一个面向嵌入式开发、电子调试和 Git 工程流程学习的四轮小车项目。ESP32-CAM 创建本地 Wi-Fi 热点，通过网页提供 OV3660 实时画面和四电机控制；两块 TC1508A 分别驱动左右两侧电机。

## 当前状态

第一阶段原型已于 2026-08-29 完成实机验收：最新版 WebSocket 固件连续测试 10 分钟、重复操作 11 次，热点、网页、视频、四种运动组合和停车保护均按预期工作，用户未观察到复位、异常发热、焦味、失控或接触不良。

- 实机验收版本：`90647f6`
- WebSocket 优化实现：`4240cf4`
- 开发环境：PlatformIO、Arduino、`esp32cam`
- 网页地址：连接 `ESP32-CAM-Car` 后访问 `http://192.168.4.1/`

详细结论和证据边界见[第 7 部分验收记录](docs/test-records/part7-final-firmware/README.md)。

## 系统结构

- ESP32-CAM 初始化 OV3660、创建 Wi-Fi 热点并提供网页。
- 端口 80 提供控制页面和 WebSocket 控制通道。
- 端口 81 提供 MJPEG 视频流。
- GPIO12、GPIO13、GPIO14、GPIO15 控制两块 TC1508A。
- 浏览器按住按钮时每约 200 ms 发送控制心跳。
- 车端在约 500 ms 未收到心跳或单次动作达到 5 s 时独立停车。

系统职责和控制流程见[架构说明](docs/architecture.md)，实际引脚与供电边界见[接线说明](docs/hardware/wiring.md)。

## 第一阶段结果

| 能力 | 状态 | 证据 |
| --- | --- | --- |
| 最小系统、编译、烧录、串口 | 已验证 | [调试记录](docs/debug-log.md) |
| OV3660、PSRAM、热点与实时视频 | 已验证 | Part 2 提交 `2a17b08` |
| 单电机双向动作与停止 | 已验证 | [Part 3](docs/test-records/part3-single-motor/README.md) |
| 四电机装配与断电检查 | 已验证 | [Part 5](docs/test-records/part5-electrical-assembly/README.md) |
| 四电机空载动作 | 已验证 | [Part 6](docs/test-records/part6-four-motor-validation/README.md) |
| 视频、网页控制与停车保护 | 已验证 | [Part 7](docs/test-records/part7-final-firmware/README.md) |

完整范围与验收标准见[第一阶段需求](docs/requirements.md)。

## 构建

安装 PlatformIO 后，在仓库根目录运行：

```sh
pio run
```

编译成功只代表固件能够生成，不等同于烧录成功或实机验证通过。烧录前必须确认实际串口、供电状态和下载模式。

## 关键安全边界

- 接线或改线前同时断开 ESP32-CAM 和电机电源。
- ESP32-CAM 与电机驱动使用两套独立正极供电，只连接公共地。
- 电机电流不得经过 ESP32-CAM、面包板或普通杜邦线。
- GPIO12 和 GPIO15 是启动配置脚；ESP32-CAM 启动完成前必须保持电机电源关闭。
- 驱动板供电期间不得重启 ESP32-CAM，禁止堵转测试。

## 已知限制

- 当前是开环控制，没有编码器反馈或左右轮速度闭环，不能保证直线精度。
- 临时杜邦连接曾在 Part 6 出现无法定位的接触异常；第一阶段最终验收未复现，但正式移动平台仍应改进连接和应力释放。
- 软件提供视频自动重连和诊断，但不能代替对电源波动、电机干扰和接线可靠性的测量。
- 当前未实现自动驾驶、计算机视觉、PID 调速、正式 PCB 或长期续航验证。

## 文档索引

- [项目规则](AGENTS.md)
- [第一阶段需求](docs/requirements.md)
- [系统架构](docs/architecture.md)
- [接线与供电](docs/hardware/wiring.md)
- [各部分测试记录](docs/test-records/)
- [版本变化](CHANGELOG.md)
