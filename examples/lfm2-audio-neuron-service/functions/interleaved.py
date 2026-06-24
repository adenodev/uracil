#!/usr/bin/env python3
from __future__ import annotations

import base64
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools"))
from neuron_gguf_shim import (  # noqa: E402
    default_model_id,
    ensure_model_loaded,
    interleaved_inference,
    resolve_sidecar_url,
)


def main() -> int:
    if len(sys.argv) < 3:
        print(
            "usage: interleaved.py <input.wav> <output.wav> [prompt ...]",
            file=sys.stderr,
        )
        return 2

    input_path = Path(sys.argv[1]).expanduser().resolve()
    output_path = Path(sys.argv[2]).expanduser().resolve()
    prompt = " ".join(sys.argv[3:]).strip()

    if not input_path.exists() or not input_path.is_file():
        print(f"input wav not found: {input_path}", file=sys.stderr)
        return 2

    sidecar_url = resolve_sidecar_url()
    model_id = default_model_id()

    ensure_model_loaded(model_id, sidecar_url)

    audio_b64_in = base64.b64encode(input_path.read_bytes()).decode()
    audio_b64_out, text = interleaved_inference(
        audio_b64_in,
        prompt,
        "Respond with interleaved text and audio.",
        model_id,
        sidecar_url,
    )

    if audio_b64_out:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_bytes(base64.b64decode(audio_b64_out))

    print(
        json.dumps(
            {
                "model_id": model_id,
                "output_audio": str(output_path),
                "text": text,
                "warnings": [],
            }
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())