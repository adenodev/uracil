#!/usr/bin/env python3
"""
Uracil APS model packaging CLI.

Scaffolds a complete APS bundle directory for any GGUF model and builds the
.aps archive.  Generated function stubs import the neuron_gguf_shim so the
model is immediately usable by agents with no additional boilerplate.

Usage
-----
    python3 package_model.py \\
        --model-id  my-llm-1b \\
        --hf-repo   SomeOrg/SomeModel-GGUF \\
        --quantization Q8_0 \\
        --capabilities tts,asr,interleaved \\
        [--platform ubuntu-x64] \\
        [--output-dir ./dist]

Outputs
-------
    <output-dir>/<model-id>-service/         Bundle directory
        manifest.json
        functions/health.py
        functions/{tts,asr,interleaved}.py   (per capability)
        build_aps.py
    <output-dir>/<model-id>-service.aps      Built archive
"""
from __future__ import annotations

import argparse
import json
import textwrap
import zipfile
from pathlib import Path

# ── Templates ─────────────────────────────────────────────────────────────

_MANIFEST_TEMPLATE = {
    "version": "0.1.0",
    "runtime": {
        "name": "uracil",
        "abi": "uracil.service.v1",
        "mode": "rootless",
    },
    "permissions": {"requested": ["net.client"]},
}

_HEALTH_PY = """\
#!/usr/bin/env python3
from __future__ import annotations
import json, os

def main() -> int:
    print(json.dumps({{
        "service": os.getenv("URACIL_SERVICE_NAME", "unknown"),
        "function": "health",
        "runner_bind": os.getenv("URACIL_RUNNER_BIND", ""),
        "runner_model_ref": os.getenv("URACIL_RUNNER_MODEL_REF", ""),
    }}))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
"""

_TTS_PY = """\
#!/usr/bin/env python3
\"\"\"TTS function for {model_id}.  Usage: tts.py <output.wav> <prompt...>\"\"\"
from __future__ import annotations
import base64, json, sys
from pathlib import Path

_SHIM = Path(__file__).resolve().parents[3] / "tools"
sys.path.insert(0, str(_SHIM))
from neuron_gguf_shim import resolve_sidecar_url, default_model_id, ensure_model_loaded, tts_inference  # noqa: E402

def main() -> int:
    if len(sys.argv) < 3:
        print("usage: tts.py <output.wav> <prompt...>", file=sys.stderr)
        return 2
    output_path = Path(sys.argv[1]).expanduser().resolve()
    prompt = " ".join(sys.argv[2:]).strip()
    url = resolve_sidecar_url()
    model_id = default_model_id()
    ensure_model_loaded(model_id, url)
    audio_b64, text = tts_inference(prompt, "", model_id, url)
    if not audio_b64:
        print("error: no audio returned", file=sys.stderr); return 1
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(base64.b64decode(audio_b64))
    print(json.dumps({{"model_id": model_id, "output_audio": str(output_path), "text": text}}))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
"""

_ASR_PY = """\
#!/usr/bin/env python3
\"\"\"ASR function for {model_id}.  Usage: asr.py <input.wav>\"\"\"
from __future__ import annotations
import base64, json, sys
from pathlib import Path

_SHIM = Path(__file__).resolve().parents[3] / "tools"
sys.path.insert(0, str(_SHIM))
from neuron_gguf_shim import resolve_sidecar_url, default_model_id, ensure_model_loaded, asr_inference  # noqa: E402

def main() -> int:
    if len(sys.argv) < 2:
        print("usage: asr.py <input.wav>", file=sys.stderr); return 2
    input_path = Path(sys.argv[1]).expanduser().resolve()
    if not input_path.exists():
        print(f"error: input wav not found: {{input_path}}", file=sys.stderr); return 2
    url = resolve_sidecar_url()
    model_id = default_model_id()
    ensure_model_loaded(model_id, url)
    audio_b64 = base64.b64encode(input_path.read_bytes()).decode()
    text = asr_inference(audio_b64, model_id, url)
    print(json.dumps({{"model_id": model_id, "text": text}}))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
"""

_INTERLEAVED_PY = """\
#!/usr/bin/env python3
\"\"\"Interleaved function for {model_id}.  Usage: interleaved.py <input.wav> <output.wav> [prompt...]\"\"\"
from __future__ import annotations
import base64, json, sys
from pathlib import Path

_SHIM = Path(__file__).resolve().parents[3] / "tools"
sys.path.insert(0, str(_SHIM))
from neuron_gguf_shim import resolve_sidecar_url, default_model_id, ensure_model_loaded, interleaved_inference  # noqa: E402

def main() -> int:
    if len(sys.argv) < 3:
        print("usage: interleaved.py <input.wav> <output.wav> [prompt...]", file=sys.stderr); return 2
    input_path  = Path(sys.argv[1]).expanduser().resolve()
    output_path = Path(sys.argv[2]).expanduser().resolve()
    prompt = " ".join(sys.argv[3:]).strip()
    if not input_path.exists():
        print(f"error: input wav not found: {{input_path}}", file=sys.stderr); return 2
    url = resolve_sidecar_url()
    model_id = default_model_id()
    ensure_model_loaded(model_id, url)
    audio_b64_in = base64.b64encode(input_path.read_bytes()).decode()
    audio_b64_out, text = interleaved_inference(audio_b64_in, prompt, "", model_id, url)
    if audio_b64_out:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_bytes(base64.b64decode(audio_b64_out))
    print(json.dumps({{"model_id": model_id, "output_audio": str(output_path), "text": text}}))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
"""

_BUILD_APS_PY = """\
#!/usr/bin/env python3
\"\"\"Build the {service_name} APS archive.\"\"\"
from __future__ import annotations
import argparse, zipfile
from pathlib import Path

def build_archive(service_dir: Path, out_file: Path) -> Path:
    out_file.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(out_file, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        zf.write(service_dir / "manifest.json", arcname="manifest.json")
        for fp in sorted((service_dir / "functions").glob("*.py")):
            zf.write(fp, arcname=f"functions/{{fp.name}}")
    return out_file

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", default="dist/{service_name}.aps")
    args = parser.parse_args()
    service_dir = Path(__file__).resolve().parent
    out = (service_dir / args.output).resolve()
    print(build_archive(service_dir, out))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
"""

_CAP_TEMPLATES = {
    "tts": _TTS_PY,
    "asr": _ASR_PY,
    "interleaved": _INTERLEAVED_PY,
}

_CAP_DESCRIPTIONS = {
    "tts": "Convert text to speech and write WAV to disk.",
    "asr": "Recognise speech from an input WAV file.",
    "interleaved": "Send input audio to the model and receive interleaved audio+text.",
}

# ── Manifest builder ──────────────────────────────────────────────────────


def _build_manifest(
    model_id: str,
    hf_repo: str,
    quantization: str,
    platform: str,
    capabilities: list[str],
) -> dict:
    service_name = f"{model_id}-service"
    functions = [
        {
            "name": "health",
            "description": "Emit runner wiring and current sidecar target.",
            "snippet_path": "functions/health.py",
            "required_permissions": [],
        }
    ]
    for cap in capabilities:
        functions.append(
            {
                "name": cap,
                "description": _CAP_DESCRIPTIONS.get(cap, f"{cap} inference."),
                "snippet_path": f"functions/{cap}.py",
                "required_permissions": ["net.client"],
            }
        )

    main_gguf = f"{Path(hf_repo).name}-{quantization}.gguf"
    artifacts = {
        "mmproj": f"corpus://athena/models/mmproj-audioencoder-{Path(hf_repo).name}-{quantization}.gguf",
        "audio_decoder": f"corpus://athena/models/audiodecoder-{Path(hf_repo).name}-{quantization}.gguf",
        "runner_archive": f"corpus://athena/models/lfm2-audio-{platform}.zip",
    }

    return {
        "name": f"service.{model_id}",
        "version": "0.1.0",
        "description": f"Uracil APS bundle for {model_id} ({quantization}) via Athena Neuron sidecar.",
        "runtime": {
            "name": "uracil",
            "abi": "uracil.service.v1",
            "mode": "rootless",
            "hotswap_group": service_name,
            "payload_revision": "0.1.0",
        },
        "service": {
            "entry_function": "health",
            "functions": functions,
        },
        "permissions": {"requested": ["net.client"]},
        "runner": {
            "kind": "gguf-neuron",
            "protocol": "http",
            "bind": "127.0.0.1:8091",
            "model": {
                "format": "gguf",
                "uri": f"corpus://athena/models/{main_gguf}",
            },
            "platform": "aws-neuron-inf2",
            "accelerator": "neuron",
            "virtualization": {
                "target": "aws-neuron-inf2",
                "accelerator": "neuron",
                "source_format": "gguf",
                "runtime_format": platform,
            },
            "model_id": model_id,
            "hf_repo": hf_repo,
            "quantization": quantization,
            "platform_hint": platform,
            "artifacts": artifacts,
        },
    }


# ── Archive builder ───────────────────────────────────────────────────────


def _build_archive(service_dir: Path, out_file: Path) -> Path:
    out_file.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(out_file, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        zf.write(service_dir / "manifest.json", arcname="manifest.json")
        for fp in sorted((service_dir / "functions").glob("*.py")):
            zf.write(fp, arcname=f"functions/{fp.name}")
    return out_file


# ── CLI ───────────────────────────────────────────────────────────────────


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Scaffold and build an APS function package for a GGUF model.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=textwrap.dedent(
            """\
            Example:
              python3 package_model.py \\
                  --model-id lfm2-audio-1.5b \\
                  --hf-repo LiquidAI/LFM2-Audio-1.5B-GGUF \\
                  --quantization Q8_0 \\
                  --capabilities tts,asr,interleaved
            """
        ),
    )
    parser.add_argument("--model-id", required=True, help="Unique model identifier (e.g. lfm2-audio-1.5b)")
    parser.add_argument("--hf-repo", required=True, help="HuggingFace repository (e.g. LiquidAI/LFM2-Audio-1.5B-GGUF)")
    parser.add_argument("--quantization", default="Q8_0", help="GGUF quantization level (default: Q8_0)")
    parser.add_argument("--platform", default="ubuntu-x64", help="Runner platform (default: ubuntu-x64)")
    parser.add_argument(
        "--capabilities",
        default="tts,asr,interleaved",
        help="Comma-separated list of capabilities to scaffold: tts, asr, interleaved (default: all)",
    )
    parser.add_argument(
        "--output-dir",
        default=".",
        help="Directory to write the bundle directory and .aps archive (default: .)",
    )
    args = parser.parse_args()

    capabilities = [c.strip() for c in args.capabilities.split(",") if c.strip()]
    unknown = [c for c in capabilities if c not in _CAP_TEMPLATES]
    if unknown:
        print(f"error: unknown capabilities: {unknown}. Valid: {list(_CAP_TEMPLATES)}", file=sys.stderr)
        return 2

    service_name = f"{args.model_id}-service"
    output_dir = Path(args.output_dir).resolve()
    service_dir = output_dir / service_name
    functions_dir = service_dir / "functions"
    functions_dir.mkdir(parents=True, exist_ok=True)

    # Write manifest
    manifest = _build_manifest(
        args.model_id,
        args.hf_repo,
        args.quantization,
        args.platform,
        capabilities,
    )
    (service_dir / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=False))
    print(f"  manifest.json")

    # Write health function
    (functions_dir / "health.py").write_text(_HEALTH_PY)
    print(f"  functions/health.py")

    # Write capability functions
    for cap in capabilities:
        src = _CAP_TEMPLATES[cap].format(model_id=args.model_id)
        (functions_dir / f"{cap}.py").write_text(src)
        print(f"  functions/{cap}.py")

    # Write build_aps.py
    build_script = _BUILD_APS_PY.format(service_name=service_name)
    (service_dir / "build_aps.py").write_text(build_script)
    print(f"  build_aps.py")

    # Build the archive
    out_aps = output_dir / f"{service_name}.aps"
    _build_archive(service_dir, out_aps)
    print(f"\nBuilt: {out_aps}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
