#!/usr/bin/env python3
"""
APS TTS function for LFM2-Audio via Athena Neuron sidecar.

Usage:
    tts.py <output.wav> <prompt text...> [--system <system_prompt>]

The sidecar URL and model ID are resolved via URACIL_RUNNER_* env vars
(injected by the Uracil runtime).  The shim handles model-load checks
automatically.
"""
from __future__ import annotations

import base64
import json
import sys
from pathlib import Path

# The shim lives one level up in uracil/tools/ and is importable from there
# when running under the Uracil host_runtime or any correctly-configured env.
import os

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools"))
from neuron_gguf_shim import (  # noqa: E402
    default_model_id,
    ensure_model_loaded,
    resolve_sidecar_url,
    tts_inference,
)


def main() -> int:
    if len(sys.argv) < 3:
        print(
            "usage: tts.py <output.wav> <prompt text> [--system <system_prompt>]",
            file=sys.stderr,
        )
        return 2

    output_path = Path(sys.argv[1]).expanduser().resolve()

    # Parse remaining args: collect prompt words; extract optional --system
    remaining = sys.argv[2:]
    system_prompt = ""
    prompt_words: list[str] = []
    i = 0
    while i < len(remaining):
        if remaining[i] == "--system" and i + 1 < len(remaining):
            system_prompt = remaining[i + 1]
            i += 2
        else:
            prompt_words.append(remaining[i])
            i += 1

    prompt = " ".join(prompt_words).strip()
    if not prompt:
        print("error: prompt text is required", file=sys.stderr)
        return 2

    sidecar_url = resolve_sidecar_url()
    model_id = default_model_id()

    ensure_model_loaded(model_id, sidecar_url)

    audio_b64, text = tts_inference(prompt, system_prompt, model_id, sidecar_url)

    if not audio_b64:
        print("error: sidecar returned no audio", file=sys.stderr)
        return 1

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(base64.b64decode(audio_b64))

    print(
        json.dumps(
            {
                "model_id": model_id,
                "output_audio": str(output_path),
                "text": text,
            }
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
