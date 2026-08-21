# VoiceEdgeMonitor

## 项目简介

VoiceEdgeMonitor 是一个基于 RK3588 的边缘语音采集、识别与 Web 可视化监控项目。

项目采用“双端架构”设计：

- **RK3588 板端**：负责配置化 ALSA 音频采集、基础语音处理、语音识别、状态管理，以及向主机发送实时音频与识别结果；当前验证基线为 Logitech C310 USB 麦克风。
- **主机 Web 端**：负责接收板端数据，并在浏览器中展示实时音频状态、波形、识别文本、运行状态，并提供音频监听能力。

本项目重点不是做完整的智能音箱，而是构建一个稳定、可观测、可扩展的边缘语音处理系统。

## 当前状态

这是一个可运行的 MVP/技术预览版本：

- Phase 1：C310 ALSA 采集已完成
- Phase 2：有界音频处理队列已完成
- Phase 3：RK3588 WebSocket Server 和 CLI Client 已完成
- Phase 4：能量 VAD 已接入
- Phase 5：离线 SenseVoice ASR 和 WebSocket ASR 事件已接入
- Phase 6：浏览器 Web UI 初版已完成
- Phase 7：长时间稳定性、慢客户端和断线恢复验收进行中

当前正式输入设备是 Logitech C310 USB 麦克风，3.5mm CTIA 输入保留为后续兼容项。

核心闭环已经在 RK3588 上验证：

```text
C310
  → ALSA
  → bounded audio queues
  → VAD
  → SenseVoice offline ASR
  → WebSocket Server
  → CLI / Browser Web Client
```

## 快速开始

### 1. RK3588 板端构建

在 RK3588 的项目目录中准备以下环境：

- ARM64 Linux
- C++17、CMake、pkg-config
- ALSA 开发库
- Boost.Beast 头文件
- sherpa-onnx 运行库和 SenseVoice 模型（放在仓库外部）

设置外部资源路径后构建：

```bash
export SHERPA_ONNX_ROOT=/path/to/sherpa-onnx-runtime
export SENSEVOICE_MODEL_DIR=/path/to/sensevoice-model

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTING=ON \
  -DVOICEEDGE_BUILD_ALSA=ON \
  -DVOICEEDGE_BUILD_WS=ON \
  -DVOICEEDGE_BUILD_ASR=ON \
  -DSHERPA_ONNX_ROOT="$SHERPA_ONNX_ROOT"
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### 2. 启动 RK3588 WebSocket Server

```bash
build/ws_server \
  --bind=0.0.0.0 \
  --port=8765 \
  --path=/voiceedge \
  --device=hw:CARD=U0x46d0x81b,DEV=0 \
  --period-frames=2000 \
  --buffer-frames=8000 \
  --asr-model="$SENSEVOICE_MODEL_DIR/model.int8.onnx" \
  --asr-tokens="$SENSEVOICE_MODEL_DIR/tokens.txt" \
  --asr-language=zh \
  --asr-provider=cpu \
  --asr-threads=2
```

默认音频参数：16 kHz、单声道、`S16_LE`。USB 声卡编号可能变化，优先使用稳定的
`CARD=` 设备名，不要固定使用 `hw:1,0` 或 `hw:2,0`。

### 3. 启动主机 Web 页面

在 WSL 或主机项目根目录执行：

```bash
python3 -m http.server 8080 --directory web
```

浏览器打开：

```text
http://127.0.0.1:8080/?ws=ws://<RK3588_IP>:8765/voiceedge
```

页面会显示设备状态、波形、VAD、ASR 文本和运行事件。音频监听必须由用户点击
启动；外放可能造成 C310 声学回授，建议使用耳机。

### 4. 可选本地保存

音频和识别文字默认不保存。需要保存时，在板端 Server 启动参数中显式增加：

```bash
--persist-dir=/data/voiceedge \
--persist-audio \
--persist-transcript
```

语音段会保存为 WAV，识别结果和元数据保存为 JSONL。当前 Web UI 没有保存按钮。

## 文档入口

- [板端构建、采集、Pipeline 和 Server 说明](board/README.md)
- [WebSocket 协议](docs/protocol.md)
- [VAD 设计与验收](docs/vad.md)
- [离线 ASR、模型和持久化](docs/asr.md)
- [浏览器 Web Client](web/README.md)

## 已知限制

- C310 可能受到 USB Hub、线缆或供电影响而自动 reset；Server 已支持 ALSA 设备退避重连，但仍需长时间验证。
- 尚未完成固定中文测试集上的 CER、噪声环境识别质量和完整链路 10～30 分钟验收。
- 尚未完成慢客户端压力测试、浏览器兼容性矩阵和 TLS/鉴权。
- 3.5mm CTIA 输入尚未作为当前第一阶段主输入验收。

---

## 一、项目目标

项目第一阶段目标是完成以下闭环：

1. RK3588 通过可配置的 ALSA 输入采集实时音频；当前第一阶段使用 Logitech C310 USB 麦克风。
2. 板端完成基础语音处理与语音识别。
3. 板端将实时音频推送到主机。
4. 板端将识别文本与处理状态同步推送到主机。
5. 主机通过 Web 页面实时展示音频波形、识别结果和系统状态。
6. Web 页面可以监听来自 RK3588 的实时音频。
7. Web 端异常、网络延迟或浏览器卡顿不得影响板端采集和识别主流程。

---

## 二、总体架构

项目由两个独立软件组成。

### 1. RK3588 板端服务

负责：

- 配置化 ALSA 麦克风音频采集（当前基线为 C310 USB 麦克风）
- ALSA 音频设备管理
- PCM 数据读取
- 音频缓冲
- VAD 语音活动检测
- ASR 语音识别
- 音频实时推送
- 识别结果推送
- 运行状态上报
- 网络断开处理
- 异常恢复

板端是整个系统的核心。

即使主机断网、Web 页面关闭或者主机处理速度较慢，板端音频采集和语音识别仍然必须正常运行。

### 2. 主机 Web 服务

负责：

- 接收 RK3588 推送的数据
- 实时音频播放
- 实时波形显示
- ASR 文本显示
- VAD 状态显示
- 系统运行状态显示
- 网络状态显示
- 历史识别结果展示

Web 页面主要用于监控和展示，不参与板端语音核心处理。

---

## 三、核心数据链路

### 音频主链路

```text
Configured ALSA Microphone (C310 baseline)
        ↓
      ALSA
        ↓
       PCM
        ↓
   Audio Buffer
        ↓
       VAD
        ↓
       ASR
        ↓
 Recognition Result
```

### Web 旁路链路

```text
PCM Audio
    ↓
Network Sender
    ↓
Host Web Server
    ↓
Browser Audio Playback
```

同时：

```text
ASR Result / VAD State / Device State
                  ↓
            Network Sender
                  ↓
           Host Web Server
                  ↓
              Browser UI
```

---

## 四、核心设计原则

### 1. 板端优先

板端音频采集和语音识别是主业务。

Web 展示只是旁路能力。

任何情况下都不允许：

- 网络发送阻塞音频采集
- Web 客户端速度影响 ASR
- 浏览器关闭导致板端退出
- 主机断开导致板端停止工作

---

### 2. 音频与网络解耦

音频采集、语音识别和网络发送需要通过缓冲机制解耦。

网络发送速度低于音频产生速度时，不允许无限堆积。

应优先保证实时性。

当缓存压力过大时，可以丢弃旧的监控音频数据，但不能影响当前语音识别链路。

---

### 3. 有界缓存

所有用于实时数据传输的队列必须设置容量上限。

禁止无限增长。

需要记录：

- 当前队列深度
- 最大队列深度
- 丢弃的数据量
- 网络拥塞次数

---

### 4. 可观测性

系统需要能够清晰看到当前状态。

包括：

- 音频设备是否正常
- 当前采样率
- 当前声道数
- PCM 格式
- 当前音频能量
- VAD 状态
- ASR 状态
- 当前识别文本
- 网络连接状态
- Web 客户端连接数量
- 缓冲区状态
- 错误次数
- 运行时间

---

## 五、板端功能需求

### 1. ALSA 音频采集

支持通过 RK3588 的可配置 ALSA 输入采集音频。第一阶段使用 Logitech C310 USB 麦克风；3.5mm CTIA 输入作为后续兼容设备。

要求：

- 支持稳定连续采集
- 支持常见 PCM 格式
- 支持设置采样率
- 支持设置声道数
- 支持读取 Period / Buffer 参数
- 支持错误恢复
- 支持设备异常检测
- 支持正常退出

第一阶段优先使用适合语音识别的参数，例如：

- 16 kHz
- Mono
- S16_LE

实际参数以 RK3588 音频设备能力为准。

---

### 2. 音频缓冲

增加独立的实时音频缓冲机制。

需要满足：

- 固定容量
- 线程安全
- 生产者 / 消费者模型
- 支持超时
- 支持停止
- 支持数据丢弃统计
- 不允许无限增长

音频识别链路优先保证连续性。

网络监控链路优先保证实时性。

---

### 3. VAD

增加语音活动检测。

用于判断：

- 是否开始说话
- 是否正在说话
- 是否结束说话

Web 页面需要能够实时显示 VAD 状态。

第一阶段可以采用轻量方案，不要求复杂模型。

---

### 4. ASR

增加离线语音识别。

要求：

- 中文优先
- 本地运行
- 不依赖云 API
- 输出最终识别文本
- 记录识别耗时
- 支持异常恢复

第一阶段主要目标是稳定，不追求复杂连续对话。

---

### 5. 网络数据发送

板端需要同时发送两类数据。

#### 音频数据

用于主机实时监听和波形展示。

#### 状态数据

至少包含：

- 时间戳
- VAD 状态
- ASR 状态
- ASR 文本
- 识别耗时
- 音频设备状态
- 网络状态
- 错误信息

第一阶段推荐采用适合双向实时通信的网络方式。

WebSocket 可以作为优先方案。

不要求第一阶段使用 RTMP。

---

## 六、主机 Web 功能需求

### 1. 实时状态页

Web 页面首页显示：

- RK3588 在线状态
- 音频设备状态
- 当前采样率
- 当前音量 / 音频能量
- VAD 状态
- ASR 状态
- 最近一次识别结果
- 当前延迟
- 网络状态

---

### 2. 实时音频波形

Web 页面显示实时音频波形。

目标是帮助观察：

- 是否有声音输入
- 音量是否过低
- 是否存在明显噪声
- VAD 是否和实际讲话一致

波形展示不能反向影响板端音频采集。

---

### 3. 实时音频播放

浏览器可以监听 RK3588 当前采集到的音频。

允许存在一定播放延迟。

播放延迟不会作为第一阶段核心性能指标。

重点要求是：

- 播放稳定
- 网络抖动时不影响板端
- 用户可以开始 / 停止监听

---

### 4. ASR 文本展示

Web 页面实时展示：

- 当前识别结果
- 历史识别结果
- 时间戳
- 单次识别耗时

历史记录第一阶段只需要保存在主机进程内存或简单本地文件中即可。

暂时不需要数据库。

---

### 5. 事件日志

显示关键事件，例如：

- RK3588 连接
- RK3588 断开
- 音频设备打开成功
- 音频设备异常
- VAD 开始
- VAD 结束
- ASR 开始
- ASR 完成
- 网络拥塞
- 队列丢弃数据

---

## 七、线程与并发要求

板端至少需要保证以下逻辑相互解耦：

- 音频采集
- 音频处理
- ASR
- 网络发送

原则：

- ALSA 采集线程不能等待网络
- 网络线程不能阻塞 ASR
- ASR 耗时不能导致音频采集停止
- 禁止 busy loop
- 禁止 detached thread
- 所有线程必须支持正常退出
- Ctrl+C 后必须正常释放设备和线程

---

## 八、异常处理

项目必须覆盖以下异常：

- 麦克风设备打开失败
- ALSA 读取失败
- XRUN
- 音频设备短暂异常
- 主机未启动
- WebSocket 连接失败
- 网络断开
- 主机重启
- 浏览器关闭
- ASR 异常
- 音频缓存溢出

需要明确区分：

- 可恢复错误
- 需要重新连接的错误
- 不可恢复错误

禁止无限快速重试。

---

## 九、性能与稳定性要求

### 第一阶段基础目标

连续运行至少 30 分钟。

要求：

- 无明显持续内存增长
- 无死锁
- 无线程泄漏
- 无无限重连
- 无无限缓存增长
- 板端采集稳定
- Web 关闭后板端继续正常运行
- Web 重新打开后可以重新连接

---

## 十、第一阶段验收标准

### 板端

完成：

```text
Configured ALSA Microphone
→ ALSA
→ PCM
→ VAD
→ ASR
```

能够持续运行。

### 网络

完成：

```text
RK3588
→ Network
→ Host
```

能够持续发送：

- 实时音频
- VAD 状态
- ASR 结果
- 设备状态

### Web

完成：

- RK3588 在线状态显示
- 实时波形
- 实时音频监听
- VAD 状态
- ASR 文本
- 历史识别记录
- 基础运行日志

### 解耦

以下操作不得影响 RK3588 主业务：

- 关闭浏览器
- 刷新浏览器
- Web 页面卡顿
- 主机程序重启
- 网络短暂断开

---

## 十一、本阶段暂不实现

为了控制项目规模，第一阶段不实现：

- TTS
- AEC
- AGC
- NS
- Beamforming
- 多麦克风阵列
- 智能音箱业务
- 大模型对话
- Qwen / LLM
- 摄像头联动
- 视频推流
- RTMP 音视频合流
- 数据库
- 用户系统
- 云服务
- OTA
- 复杂权限系统

这些能力后续根据实际需要再增加。

---

## 十二、推荐开发阶段

### Phase 1：音频输入

完成 RK3588 配置化 ALSA 麦克风采集；当前已验证 Logitech C310 USB 麦克风。

验收稳定 PCM 数据。

### Phase 2：音频处理管线

完成采集线程、处理线程、音频处理队列、监控音频队列和状态统计。

所有实时队列有容量上限。

### Phase 3：WebSocket 通信

完成：

- RK3588 WebSocket Server
- 主机 Web/CLI Client
- 状态 JSON
- 二进制 PCM 音频包
- 心跳、断开和有界客户端队列

### Phase 4：VAD

增加语音活动检测，并同步展示状态。

### Phase 5：ASR

增加离线 ASR，并通过 WebSocket 实时展示识别结果。

### Phase 6：浏览器 Web UI

完成：

- 状态卡片
- 实时波形
- 音频监听
- VAD/ASR 事件和文本
- 断线重连

页面入口见 [`web/README.md`](web/README.md)。

### Phase 7：稳定性与诊断

补充：

- 错误恢复
- 网络重连
- 队列监控
- 延迟统计
- 丢包 / 丢帧统计
- 30 分钟稳定性测试

---

## 十三、项目最终效果

项目完成后，用户在 RK3588 麦克风前说话：

```text
User Voice
    ↓
Configured ALSA Microphone
    ↓
RK3588 ALSA Capture
    ↓
VAD
    ↓
ASR
    ↓
Network
    ↓
Host Web Server
    ↓
Browser
```

浏览器能够同时看到：

- 实时音频波形
- 当前是否有人讲话
- 实时音频监听
- 语音识别文本
- 识别耗时
- RK3588 运行状态
- 网络状态
- 错误和事件日志

最终目标是形成一个：

**稳定、实时、可观测、板端与 Web 解耦的 RK3588 边缘语音监控系统。**

---

## 十四、开发原则

Agent 开发时必须遵守以下原则：

1. 先阅读 README，再开始设计。
2. 先完成最小闭环，再增加功能。
3. 不要一次实现所有 Phase。
4. 不要引入无关功能。
5. 不要为了架构而架构。
6. 优先保证稳定性和可调试性。
7. 所有实时队列必须有容量上限。
8. 网络不得阻塞板端核心业务。
9. 所有错误必须可定位。
10. 每完成一个 Phase，都需要给出实际测试结果和当前限制。
