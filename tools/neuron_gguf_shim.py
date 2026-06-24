#!/usr/bin/env python3
"""
Uracil Neuron/GGUF compatibility shim.

Shared helper imported by all APS audio function snippets.  Abstracts the
Athena Neuron sidecar API so individual function scripts stay minimal.

All HTTP calls use stdlib ``urllib.request`` only — no external deps.
The sidecar URL and model configuration are read from URACIL_* env vars
injected by the Uracil runtime at execution time.

Public API
----------
resolve_sidecar_url()   -> str
ensure_model_loaded(model_id, sidecar_url, timeout=360) -> None
tts_inference(prompt, system, model_id, sidecar_url) -> tuple[str, str]
asr_inference(audio_b64, model_id, sidecar_url) -> str
interleaved_inference(audio_b64, prompt, system, model_id, sidecar_url) -> tuple[str, str]
"""
from __future__ import annotations

import json
import os
import urllib.error
import urllib.request
from typing import Any


# ── URL resolution ────────────────────────────────────────────────────────


def resolve_sidecar_url() -> str:
    """
    Build the sidecar base URL from URACIL_RUNNER_BIND / URACIL_RUNNER_PROTOCOL.
    If URACIL_RUNNER_BIND already starts with a scheme it is returned as-is.
    """
    bind = os.getenv("URACIL_RUNNER_BIND", "127.0.0.1:8091")
    protocol = os.getenv("URACIL_RUNNER_PROTOCOL", "http")
    if bind.startswith("http://") or bind.startswith("https://"):
        return bind.rstrip("/")
    return f"{protocol}://{bind}".rstrip("/")


def runner_config() -> dict[str, Any]:
    raw = os.getenv("URACIL_RUNNER_CONFIG_JSON", "{}")
    try:
        parsed = json.loads(raw)
    except json.JSONDecodeError:
        return {}
    return parsed if isinstance(parsed, dict) else {}


def default_model_id() -> str:
    cfg = runner_config()
    return str(
        cfg.get("model_id")
        or os.getenv("URACIL_MODEL_ID")
        or "lfm2-audio-1.5b"
    )


# ── Low-level HTTP helpers ────────────────────────────────────────────────


def _post_json(url: str, payload: dict[str, Any], timeout: float) -> dict[str, Any]:
    req = urllib.request.Request(
        url,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"HTTP {exc.code} from {url}: {body}") from exc
    except urllib.error.URLError as exc:
        raise RuntimeError(f"Request to {url} failed: {exc}") from exc


def _get_json(url: str, timeout: float) -> dict[str, Any]:
    req = urllib.request.Request(url, method="GET")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"HTTP {exc.code} from {url}: {body}") from exc
    except urllib.error.URLError as exc:
        raise RuntimeError(f"Request to {url} failed: {exc}") from exc


# ── Model lifecycle ───────────────────────────────────────────────────────


def ensure_model_loaded(model_id: str, sidecar_url: str, timeout: float = 360.0) -> None:
    """
    Check whether ``model_id`` is already loaded in the sidecar.
    If not, trigger a load and wait up to ``timeout`` seconds for it to
    complete.  Raises ``RuntimeError`` if loading fails.
    """
    models_url = f"{sidecar_url}/v1/models"
    try:
        body = _get_json(models_url, timeout=10.0)
        loaded = {m.get("model_id") for m in (body.get("data") or [])}
        if model_id in loaded:
            return
    except RuntimeError:
        pass  # Sidecar unreachable — let the load attempt surface the error.

    cfg = runner_config()
    load_payload: dict[str, Any] = {
        "model_id": model_id,
        "uri": str(cfg.get("uri") or os.getenv("URACIL_RUNNER_MODEL_REF") or ""),
        "runner": str(cfg.get("runner") or "gguf-neuron"),
    }
    _post_json(f"{sidecar_url}/v1/models/load", load_payload, timeout=timeout)


# ── Inference helpers ─────────────────────────────────────────────────────


def tts_inference(
    prompt: str,
    system: str,
    model_id: str,
    sidecar_url: str,
    *,
    timeout: float = 600.0,
) -> tuple[str, str]:
    """
    Run TTS inference on the sidecar.

    Returns ``(audio_b64, text)`` where ``audio_b64`` is a base-64-encoded
    WAV and ``text`` is any transcript produced by the runner.
    """
    payload = {
        "model_id": model_id,
        "prompt": prompt,
        "system": system or "Perform TTS.",
        "audio_b64": "",
    }
    body = _post_json(f"{sidecar_url}/v1/audio/tts", payload, timeout=timeout)
    return str(body.get("audio_b64") or ""), str(body.get("text") or "")


def asr_inference(
    audio_b64: str,
    model_id: str,
    sidecar_url: str,
    *,
    system: str = "Perform ASR.",
    timeout: float = 600.0,
) -> str:
    """
    Run ASR inference on the sidecar.  Returns the recognised text.
    """
    payload = {
        "model_id": model_id,
        "prompt": "",
        "system": system,
        "audio_b64": audio_b64,
    }
    body = _post_json(f"{sidecar_url}/v1/audio/asr", payload, timeout=timeout)
    return str(body.get("text") or "")


def interleaved_inference(
    audio_b64: str,
    prompt: str,
    system: str,
    model_id: str,
    sidecar_url: str,
    *,
    timeout: float = 600.0,
) -> tuple[str, str]:
    """
    Run interleaved audio+text inference on the sidecar.

    Returns ``(audio_b64, text)``.
    """
    payload = {
        "model_id": model_id,
        "prompt": prompt,
        "system": system or "Respond with interleaved text and audio.",
        "audio_b64": audio_b64,
    }
    body = _post_json(f"{sidecar_url}/v1/audio/interleaved", payload, timeout=timeout)
    return str(body.get("audio_b64") or ""), str(body.get("text") or "")
