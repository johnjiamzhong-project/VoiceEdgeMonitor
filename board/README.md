# ALSA capture probe

`capture_probe` is the Phase 1 hardware probe. It only opens the configured
ALSA capture device, reads PCM, reports signal statistics and optionally writes
a PCM WAV file. It does not load VAD/ASR models and does not use the network.

## Build on the RK3588 board

The target needs a C++17 compiler, CMake, `pkg-config` and ALSA development
headers/library:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON -DVOICEEDGE_BUILD_WS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Run with the validated C310 input

The card number can change when USB devices are enumerated. Prefer the stable
card name:

```bash
build/capture_probe \
  --device=hw:CARD=U0x46d0x81b,DEV=0 \
  --period-frames=2000 \
  --buffer-frames=8000 \
  --run-ms=30000 \
  --wav=/tmp/voiceedge-c310-30s.wav
```

The current validated baseline is 16 kHz, mono, `S16_LE`. Without `--run-ms`,
the probe runs until Ctrl+C. `SIGINT` and `SIGTERM` stop the capture and close
the ALSA handle cleanly.

The device can also be supplied through `VOICEEDGE_CAPTURE_DEVICE`; an
explicit `--device` takes precedence:

```bash
VOICEEDGE_CAPTURE_DEVICE=hw:CARD=U0x46d0x81b,DEV=0 \
  build/capture_probe --run-ms=10000
```

Each statistics line reports elapsed time, RMS/peak level in dBFS, frames,
XRUNs, recoveries and read errors. A successful Phase 1 stability run must
record its duration and environment and must not claim long-run stability from
the short smoke test alone.

## Phase 2 pipeline probe

`pipeline_probe` adds two independent bounded queues:

```text
ALSA capture
   ├─> processing queue -> processing thread -> audio state
   └─> monitor queue   -> monitor consumer
```

The processing queue rejects new frames when full and increments
`processing_drops`. The monitor queue drops its oldest frame when full and
increments `monitor_drops`. The monitor consumer is a local stand-in for the
future network sender; no WebSocket or ASR is included yet.

```bash
build/pipeline_probe \
  --device=hw:CARD=U0x46d0x81b,DEV=0 \
  --period-frames=2000 \
  --buffer-frames=8000 \
  --run-ms=10000 \
  --processing-capacity=32 \
  --monitor-capacity=64
```

Use `--monitor-delay-ms` to exercise monitor backpressure without changing the
processing path. All threads are joined during normal shutdown.

## Phase 3 WebSocket Server

Build with `-DVOICEEDGE_BUILD_WS=ON`, then start the board Server:

```bash
build/ws_server \
  --bind=0.0.0.0 \
  --port=8765 \
  --path=/voiceedge \
  --device=hw:CARD=U0x46d0x81b,DEV=0
```

The CLI Client can connect from a host with the Boost.Beast headers available:

```bash
build/ws_cli \
  --host=<RK3588_IP> \
  --port=8765 \
  --path=/voiceedge
```

The Server sends state JSON and binary PCM packets. The packet format is
documented in [`docs/protocol.md`](../docs/protocol.md).

## Phase 4 energy VAD

`pipeline_probe` now includes a configurable energy VAD in the processing
thread. The initial candidate values are:

```text
start_rms=0.015
end_rms=0.012
min_speech_ms=125
silence_ms=1000
```

Override them with `--vad-start-rms`, `--vad-end-rms`,
`--vad-min-speech-ms` and `--vad-silence-ms`. The state machine emits
`vad_started` and `vad_ended` events. See [`docs/vad.md`](../docs/vad.md) for
the state machine and acceptance metrics.

## Phase 5 offline ASR

ASR is an optional build target. Enable it with:

```bash
cmake -S . -B build \
  -DVOICEEDGE_BUILD_ASR=ON \
  -DSHERPA_ONNX_ROOT="$SHERPA_ONNX_ROOT"
```

`pipeline_probe` keeps ASR inference in a separate bounded queue and worker.
See [`docs/asr.md`](../docs/asr.md) for model paths, options and acceptance
metrics.
