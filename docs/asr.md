# 离线 ASR 设计与验收（Phase 5）

当前使用 sherpa-onnx C API 和 SenseVoiceSmall int8 模型。模型、tokens 和运行库
位于仓库外部，不提交到项目目录。

## 独立探针

在 RK3588 上启用 ASR 目标：

```bash
cmake -S . -B build \
  -DVOICEEDGE_BUILD_ALSA=ON \
  -DVOICEEDGE_BUILD_WS=ON \
  -DVOICEEDGE_BUILD_ASR=ON \
  -DSHERPA_ONNX_ROOT="$SHERPA_ONNX_ROOT"
cmake --build build --parallel
```

独立 WAV 测试：

```bash
build/asr_probe \
  --model="$SENSEVOICE_MODEL_DIR/model.int8.onnx" \
  --tokens="$SENSEVOICE_MODEL_DIR/tokens.txt" \
  --wave="$SENSEVOICE_MODEL_DIR/test_wavs/zh.wav" \
  --language=zh --provider=cpu --threads=2
```

输出必须包含最终文本、模型加载耗时、处理耗时、音频时长和 RTF。

## Pipeline ASR 队列

VAD 结束后生成一个有界 `SpeechSegment`，进入独立 ASR 队列：

```text
采集线程 → 处理队列 → VAD → 有界 ASR 队列 → ASR 线程
       └──────────────→ 监控音频队列
```

- ASR 推理不在采集线程和处理线程中执行。
- ASR 队列满时增加 `asr_drops`，不无限缓存。
- 单段语音超过 `max_segment_ms` 时增加 `asr_overflows` 并丢弃该段。
- ASR 失败增加 `asr_failures`，不导致采集线程退出。

Pipeline 启用 ASR 的关键参数：

```bash
--asr-model <path>
--asr-tokens <path>
--asr-language zh
--asr-provider cpu
--asr-threads 2
--asr-queue-capacity 4
--max-segment-ms 30000
```

## 首轮目标板结果

C310 连续 30 秒测试中已完成 8 个 ASR 语音段，观测到：

- 推理耗时约 99～291 ms
- RTF 约 0.058～0.081
- `asr_drops=0`
- `asr_failures=0`
- `asr_overflows=0`

当前还需要补充固定测试集上的中文 CER、不同噪声环境和更长语音段测试。
