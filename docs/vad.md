# VAD 设计与验收（Phase 4）

当前采用不依赖外部模型的能量 VAD，运行在音频处理线程中。它只读取已经进入
处理队列的 PCM，不访问网络，也不阻塞 ALSA 采集。

## 状态机

```text
idle
  → starting
  → speaking
  → ending
  → idle
```

- `start_rms`：进入语音候选的阈值。
- `end_rms`：低于该值后进入静音结束候选；应低于 `start_rms` 形成迟滞。
- `min_speech_ms`：语音候选持续达到该时长后才触发 `vad_started`。
- `silence_ms`：持续低于结束阈值达到该时长后触发 `vad_ended`。

当前默认候选值：

```text
start_rms=0.015
end_rms=0.012
min_speech_ms=125
silence_ms=1000
```

这些值需要使用实际 C310 的安静、噪声和讲话录音重新标定，不能视为最终声学参数。

## Pipeline 配置

```bash
build/pipeline_probe \
  --device=hw:CARD=U0x46d0x81b,DEV=0 \
  --vad-start-rms=0.015 \
  --vad-end-rms=0.012 \
  --vad-min-speech-ms=125 \
  --vad-silence-ms=1000
```

Pipeline 状态会输出当前 VAD 状态、开始/结束事件次数和最新 RMS；事件格式为：

```text
vad_event type=vad_started timestamp_ms=... duration_ms=0 rms=...
vad_event type=vad_ended timestamp_ms=... duration_ms=... rms=...
```

## 验收指标

测试至少覆盖安静环境、风扇/键盘等背景噪声、短语音、连续语音和停顿，并记录：

- 静音/噪声误触发次数与测试时长
- 真实语音漏检次数与语音段数量
- 开始和结束检测延迟的 p50/p95
- `starting`/`ending` 状态抖动次数
- 处理线程 CPU 占用

当前已完成合成 PCM 状态机测试；真实 C310 VAD 测试因 USB 设备暂时出现 I/O error
尚未完成。
