# LFM2 Audio Neuron Service

This example APS bundle targets the Athena Neuron sidecar for LiquidAI
LFM2-Audio interleaved inference.

The bundle uses the existing Uracil runner metadata passthrough:

- `URACIL_RUNNER_PROTOCOL`
- `URACIL_RUNNER_BIND`
- `URACIL_RUNNER_MODEL_REF`
- `URACIL_RUNNER_CONFIG_JSON`

The `interleaved` function reads a local `input.wav`, sends it to the sidecar,
and writes the returned `output.wav` to disk.

## Build archive

```bash
cd uracil/examples/lfm2-audio-neuron-service
python3 build_aps.py
```

## Install with APS CLI

```bash
aps install service uracil/examples/lfm2-audio-neuron-service/dist/lfm2-audio-neuron-service.aps
aps info demo.lfm2.audio-neuron-service
```

## Grant network permission

```bash
uracil --bundle ~/.aps/packages/demo.lfm2.audio-neuron-service grant net.client
```

## Run health

```bash
aps run demo.lfm2.audio-neuron-service
```

## Run interleaved inference

```bash
aps run demo.lfm2.audio-neuron-service --function interleaved input.wav output.wav "Transcribe and answer with speech"
```

The manifest defaults to `127.0.0.1:8091`, so this bundle is intended to run on
the same host as `athena/neuron_sidecar.py`.