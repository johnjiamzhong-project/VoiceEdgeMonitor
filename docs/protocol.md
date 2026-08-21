# VoiceEdgeMonitor WebSocket 协议（Phase 3）

## 连接方向

```text
RK3588 WebSocket Server
        ↑
主机 Web Client / CLI Client
```

默认地址：

```text
ws://<RK3588_IP>:8765/voiceedge
```

当前版本只适合可信局域网，尚未加入 TLS、鉴权或用户系统。

## 文本消息

### Server hello

```json
{"type":"hello","version":1,"server":"VoiceEdgeMonitor"}
```

### 状态消息

状态消息是 UTF-8 JSON，当前示例：

```json
{
  "type": "state",
  "version": 1,
  "timestamp_ms": 2005,
  "device": "hw:CARD=U0x46d0x81b,DEV=0",
  "sample_rate": 16000,
  "channels": 1,
  "format": "S16_LE",
  "captured_frames": 31995,
  "xruns": 0,
  "recoveries": 0,
  "read_errors": 0,
  "clients": 1,
  "vad": "speaking",
  "vad_started": 2,
  "vad_ended": 1,
  "asr": "processing",
  "asr_segments": 3,
  "asr_drops": 0,
  "asr_failures": 0,
  "asr_processing_ms": 210,
  "audio_state": "online",
  "capture_reconnects": 0,
  "capture_reconnect_failures": 0
}
```

`timestamp_ms` 是本次 Server 运行以来的单调时间，不是墙上时钟。

VAD 事件使用文本 JSON：

```json
{"type":"vad_event","event":"vad_started","timestamp_ms":1875,"duration_ms":0,"rms":0.055710}
```

`event` 当前为 `vad_started` 或 `vad_ended`，状态值为 `idle`、`starting`、
`speaking` 或 `ending`。

启用外部 sherpa-onnx ASR 后，Server 还会发送：

```json
{"type":"asr_started","segment_id":1,"start_timestamp_ms":1875,"end_timestamp_ms":4375}
{"type":"asr_final","segment_id":1,"text":"测试文本","processing_ms":210,"audio_ms":2625.0,"rtf":0.0800}
{"type":"asr_failed","segment_id":1,"reason":"..."}
```

`asr` 状态为 `idle` 或 `processing`；ASR 模型未启用时保持 `idle`。

音频设备异常和恢复使用文本事件：

```json
{"type":"audio_event","event":"capture_reconnecting"}
{"type":"audio_event","event":"capture_reconnected"}
```

状态中的 `audio_state` 为 `online` 或 `reconnecting`。设备重连采用退避策略，
不会因为一次 ALSA/USB I/O 错误直接退出 WebSocket Server。

## 二进制音频消息

每个 WebSocket binary message 是一个 PCM 音频包，字节序为 little-endian。
包头固定 32 字节，后面紧跟 `S16_LE` 样本数据。

| 偏移 | 长度 | 字段 |
| ---: | ---: | --- |
| 0 | 4 | ASCII magic：`VEA1` |
| 4 | 1 | 协议版本，当前为 `1` |
| 5 | 1 | 编码，`0` 表示 PCM S16_LE |
| 6 | 1 | 声道数 |
| 7 | 1 | 保留，当前为 0 |
| 8 | 4 | 采样率 |
| 12 | 8 | 音频序列号 |
| 20 | 8 | 相对 Server 启动时间戳，毫秒 |
| 28 | 4 | 样本数量（所有声道合计） |
| 32 | N | `int16` PCM 样本 |

当前 C310 的音频参数为 16 kHz、单声道、S16_LE。序列号单调递增；客户端
可以用序列号和时间戳发现丢包或跳帧。

## 队列和断开策略

- Server 为每个 Client 使用独立的有界发送队列。
- Client 写入慢时只影响该 Client。
- 当前队列满时丢弃最旧消息，Server 主采集线程不等待网络写入。
- 浏览器或 CLI 断开不影响板端采集。
- Server 停止时关闭所有 Client；CLI 将正常结束 WebSocket EOF 视为结束，不视为协议错误。

## 当前未实现

- Client 控制命令尚未定义。
- TLS、鉴权和跨网部署尚未实现。
