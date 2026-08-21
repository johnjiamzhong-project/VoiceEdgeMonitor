# ALSA 采集探针

`capture_probe` 是 Phase 1 硬件探针。它只负责打开配置的 ALSA 采集设备、读取 PCM、
报告信号统计信息，并按需写入 PCM WAV 文件。它不会加载 VAD/ASR 模型，也不使用网络。

## 在 RK3588 板端构建

目标环境需要 C++17 编译器、CMake、`pkg-config` 以及 ALSA 开发头文件和库：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON -DVOICEEDGE_BUILD_WS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## 使用已验证的 C310 输入运行

USB 设备枚举时声卡编号可能发生变化，建议优先使用稳定的声卡名称：

```bash
build/capture_probe \
  --device=hw:CARD=U0x46d0x81b,DEV=0 \
  --period-frames=2000 \
  --buffer-frames=8000 \
  --run-ms=30000 \
  --wav=/tmp/voiceedge-c310-30s.wav
```

当前已验证的基线参数为 16 kHz、单声道、`S16_LE`。不指定 `--run-ms` 时，探针会一直
运行到按下 Ctrl+C。收到 `SIGINT` 或 `SIGTERM` 后会停止采集并正常关闭 ALSA 句柄。

也可以通过 `VOICEEDGE_CAPTURE_DEVICE` 提供设备；显式指定的 `--device` 优先级更高：

```bash
VOICEEDGE_CAPTURE_DEVICE=hw:CARD=U0x46d0x81b,DEV=0 \
  build/capture_probe --run-ms=10000
```

每行统计信息包含运行时间、dBFS 单位的 RMS/峰值电平、帧数、XRUN 数量、恢复次数和读取
错误数。一次合格的 Phase 1 稳定性测试必须记录测试时长和运行环境，不能仅凭短时冒烟
测试就宣称已经具备长期稳定性。

## Phase 2 音频处理管线探针

`pipeline_probe` 增加两个相互独立且有界的队列：

```text
ALSA 采集
   ├─> 处理队列 -> 处理线程 -> 音频状态
   └─> 监控队列 -> 监控消费者
```

处理队列满时拒绝新的音频帧，并递增 `processing_drops`。监控队列满时丢弃最旧的音频帧，
并递增 `monitor_drops`。监控消费者只是未来网络发送器的本地替代实现；此阶段还不包含
WebSocket 或 ASR。

```bash
build/pipeline_probe \
  --device=hw:CARD=U0x46d0x81b,DEV=0 \
  --period-frames=2000 \
  --buffer-frames=8000 \
  --run-ms=10000 \
  --processing-capacity=32 \
  --monitor-capacity=64
```

可以使用 `--monitor-delay-ms` 模拟监控侧背压，同时不改变处理路径。正常退出时会等待并
回收所有线程。

## Phase 3 WebSocket 服务端

使用 `-DVOICEEDGE_BUILD_WS=ON` 构建后，在板端启动 WebSocket 服务端：

```bash
build/ws_server \
  --bind=0.0.0.0 \
  --port=8765 \
  --path=/voiceedge \
  --device=hw:CARD=U0x46d0x81b,DEV=0
```

在具备 Boost.Beast 头文件的主机上，可以使用 CLI 客户端连接：

```bash
build/ws_cli \
  --host=<RK3588_IP> \
  --port=8765 \
  --path=/voiceedge
```

服务端会发送状态 JSON 和二进制 PCM 数据包。数据包格式见
[`docs/WebSocket协议.md`](../docs/WebSocket协议.md)。

## Phase 4 能量 VAD

`pipeline_probe` 现在在处理线程中包含可配置的能量 VAD。初始候选参数为：

```text
start_rms=0.015
end_rms=0.012
min_speech_ms=125
silence_ms=1000
```

可以使用 `--vad-start-rms`、`--vad-end-rms`、`--vad-min-speech-ms` 和
`--vad-silence-ms` 覆盖这些参数。状态机产生 `vad_started` 和 `vad_ended` 事件。
状态机和验收指标见 [`docs/VAD设计与验收.md`](../docs/VAD设计与验收.md)。

## Phase 5 离线 ASR

ASR 是可选构建目标，可通过以下参数启用：

```bash
cmake -S . -B build \
  -DVOICEEDGE_BUILD_ASR=ON \
  -DSHERPA_ONNX_ROOT="$SHERPA_ONNX_ROOT"
```

`pipeline_probe` 使用独立的有界队列和工作线程执行 ASR 推理。模型路径、运行选项和验收
指标见 [`docs/离线ASR设计与验收.md`](../docs/离线ASR设计与验收.md)。

如需在本地保存最终 ASR 记录，必须显式启用以下一个或两个输出：

```bash
build/pipeline_probe \
  --asr-model="$SENSEVOICE_MODEL_DIR/model.int8.onnx" \
  --asr-tokens="$SENSEVOICE_MODEL_DIR/tokens.txt" \
  --persist-dir=/data/voiceedge \
  --persist-audio \
  --persist-transcript
```

音频会保存为语音片段 WAV 文件，文本和元数据会保存为 JSONL。两个输出默认均关闭。
