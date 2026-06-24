#!/usr/bin/env python3
"""
Uracil CUDA→Neuron translation shim.

Injected into the Python environment before a function snippet runs when
runner.kind == "torch-neuron".  Patches PyTorch CUDA APIs so that existing
CUDA-targeting code (model.cuda(), tensor.to("cuda"), torch.device("cuda"),
torch.cuda.is_available()) works transparently on Amazon Inferentia2 via
torch_neuronx / XLA.

If torch_neuronx is not installed, all CUDA calls redirect to CPU with a
warning so the same service binary also runs in development environments.

Public API
----------
activate()
    Apply all CUDA→Neuron patches.  Safe to call multiple times.

compile_for_neuron(model, example_inputs, *, cache_dir=None) -> model
    Trace a nn.Module with torch_neuronx.trace(), with a persistent disk
    cache keyed on model weights + input shapes.

NeuronModelCache(cache_dir)
    Low-level cache class used by compile_for_neuron().

run_wrapped(snippet_path)
    Entry point called by ura_stub.c via ``python3 -c "...run_wrapped(...)"``
"""

from __future__ import annotations

import functools
import hashlib
import io
import logging
import os
import sys
import warnings
from pathlib import Path
from typing import Any, Callable, Optional, Sequence

_log = logging.getLogger("uracil.neuron_torch_shim")
logging.basicConfig(level=logging.INFO, format="[neuron_shim] %(levelname)s %(message)s")

_NEURON_AVAILABLE: bool = False
_ACTIVATED: bool = False

# XLA is the Neuron device name within torch_neuronx.
_NEURON_DEVICE_TARGET: str = "xla"
_NEURON_FALLBACK: str = "cpu"


# ── Neuron device counting ────────────────────────────────────────────────


def _count_neuron_devices() -> int:
    try:
        devs = list(Path("/dev").glob("neuron*"))
        return max(len(devs), 1)
    except OSError:
        return 1


# ── Device string rewriting ───────────────────────────────────────────────


def _rewrite_device_str(device: Any) -> Any:
    """Return an XLA/CPU device string if *device* names a CUDA device."""
    if not isinstance(device, str):
        return device
    lower = device.lower()
    if lower == "cuda" or lower.startswith("cuda:"):
        target = _NEURON_DEVICE_TARGET if _NEURON_AVAILABLE else _NEURON_FALLBACK
        if not _NEURON_AVAILABLE:
            warnings.warn(
                f"CUDA device '{device}' requested but torch_neuronx is not "
                f"available; redirecting to '{target}'.",
                stacklevel=3,
            )
        return target
    return device


# ── _NeuronDeviceFactory: callable replacement for torch.device ──────────


class _NeuronDeviceFactory:
    """
    Callable replacement for ``torch.device`` that silently rewrites
    'cuda' and 'cuda:N' to the Neuron XLA target before forwarding to the
    real ``torch.device`` constructor.

    ``torch.device`` is a C extension type and cannot be subclassed, so we
    replace the module-level name with this factory instance instead.  Python's
    ``isinstance(obj, torch.device)`` protocol works correctly because Python
    calls ``type(torch.device).__instancecheck__(torch.device, obj)`` — when
    ``torch.device`` is replaced by this factory instance, ``type(factory)``
    is ``_NeuronDeviceFactory``, so the method below is invoked.
    """

    def __init__(self, orig_cls: type) -> None:
        self._orig = orig_cls
        self.__name__ = "device"
        self.__qualname__ = "device"
        self.__module__ = getattr(orig_cls, "__module__", "torch")

    def __call__(self, type_or_str: Any, index: Optional[int] = None) -> Any:
        rewritten = _rewrite_device_str(type_or_str)
        if index is not None:
            return self._orig(rewritten, index)
        return self._orig(rewritten)

    def __instancecheck__(self, instance: Any) -> bool:
        return isinstance(instance, self._orig)

    def __subclasscheck__(self, subclass: Any) -> bool:
        try:
            return issubclass(subclass, self._orig)
        except TypeError:
            return False

    def __repr__(self) -> str:
        return repr(self._orig)


# ── .to() intercept ───────────────────────────────────────────────────────


def _wrap_to(original_to: Callable) -> Callable:
    """
    Return a wrapped version of .to() that rewrites CUDA device arguments
    to the Neuron target while passing all other .to() signatures unchanged
    (dtype, memory_format, copy, non_blocking, etc.).
    """

    @functools.wraps(original_to)
    def _patched_to(self: Any, *args: Any, **kwargs: Any) -> Any:
        new_args = list(args)
        if new_args:
            first = new_args[0]
            if isinstance(first, str):
                new_args[0] = _rewrite_device_str(first)
            elif hasattr(first, "type") and isinstance(getattr(first, "type", None), str):
                # torch.device-like object whose .type is 'cuda'
                if first.type == "cuda":
                    new_args[0] = _rewrite_device_str("cuda")
        if "device" in kwargs:
            d = kwargs["device"]
            if isinstance(d, str):
                kwargs["device"] = _rewrite_device_str(d)
            elif hasattr(d, "type") and getattr(d, "type", None) == "cuda":
                kwargs["device"] = _rewrite_device_str("cuda")
        return original_to(self, *new_args, **kwargs)

    return _patched_to


# ── Activation ────────────────────────────────────────────────────────────


def activate() -> None:
    """
    Apply all CUDA→Neuron patches to torch.  Safe to call multiple times;
    subsequent calls are no-ops.
    """
    global _NEURON_AVAILABLE, _ACTIVATED  # noqa: PLW0603
    if _ACTIVATED:
        return
    _ACTIVATED = True

    try:
        import torch  # noqa: PLC0415
    except ImportError as exc:
        raise ImportError(
            "neuron_torch_shim requires PyTorch. "
            "Install it in the service environment: pip install torch"
        ) from exc

    # Attempt to load torch_neuronx; degrade gracefully to CPU if absent.
    # OSError is raised when the Neuron runtime library (libnrt.so.1) is
    # missing even though the pip package is installed — catch it too.
    try:
        import torch_neuronx  # noqa: F401, PLC0415

        _NEURON_AVAILABLE = True
        _log.info("torch_neuronx loaded — CUDA calls will route to XLA/Neuron")
    except (ImportError, OSError) as _neuron_err:
        _NEURON_AVAILABLE = False
        _log.warning(
            "torch_neuronx not available (%s) — CUDA calls will route to CPU. "
            "On an Inf2 instance install: pip install torch-neuronx "
            "and ensure aws-neuronx-runtime-lib is installed via apt.",
            type(_neuron_err).__name__,
        )

    target = _NEURON_DEVICE_TARGET if _NEURON_AVAILABLE else _NEURON_FALLBACK

    # ── torch.cuda.* ─────────────────────────────────────────────────────
    torch.cuda.is_available = lambda: True  # type: ignore[assignment]
    torch.cuda.device_count = lambda: _count_neuron_devices()  # type: ignore[assignment]

    # ── torch.device replacement ──────────────────────────────────────────
    torch.device = _NeuronDeviceFactory(torch.device)  # type: ignore[misc]

    # ── torch.Tensor.cuda / .to ───────────────────────────────────────────
    def _tensor_cuda(self: "torch.Tensor", device: Any = None) -> "torch.Tensor":
        _ = device  # always route to Neuron target; device index is ignored
        return self.to(target)

    torch.Tensor.cuda = _tensor_cuda  # type: ignore[method-assign]
    torch.Tensor.to = _wrap_to(torch.Tensor.to)

    # ── torch.nn.Module.cuda / .to ────────────────────────────────────────
    def _module_cuda(
        self: "torch.nn.Module", device: Any = None
    ) -> "torch.nn.Module":
        _ = device
        return self.to(target)

    torch.nn.Module.cuda = _module_cuda  # type: ignore[assignment]
    torch.nn.Module.to = _wrap_to(torch.nn.Module.to)

    _log.info(
        "activated — CUDA→%s | neuron devices detected: %d",
        target,
        _count_neuron_devices(),
    )


# ── Persistent model cache ────────────────────────────────────────────────


class NeuronModelCache:
    """
    On-disk cache of torch_neuronx-traced models.

    Cache keys are SHA-256 over the model's serialised state_dict plus the
    shapes and dtypes of each example input tensor.  Serialisation uses
    torch.jit.save / torch.jit.load.
    """

    def __init__(self, cache_dir: "str | Path") -> None:
        self._dir = Path(cache_dir)
        self._dir.mkdir(parents=True, exist_ok=True)

    def _cache_key(
        self,
        model: Any,
        example_inputs: Sequence[Any],
    ) -> str:
        import torch  # noqa: PLC0415

        h = hashlib.sha256()
        try:
            # Move state dict to CPU before serialising — model may be on XLA.
            cpu_sd = {
                k: v.cpu() if hasattr(v, "cpu") else v
                for k, v in model.state_dict().items()
            }
            buf = io.BytesIO()
            torch.save(cpu_sd, buf)
            h.update(buf.getvalue())
        except Exception:  # pragma: no cover — non-module models
            h.update(repr(model).encode())
        for t in example_inputs:
            if hasattr(t, "shape") and hasattr(t, "dtype"):
                h.update(f"{tuple(t.shape)}:{t.dtype}".encode())
            else:
                h.update(repr(t).encode())
        return h.hexdigest()

    def get(
        self,
        model: Any,
        example_inputs: Sequence[Any],
    ) -> Any:
        import torch  # noqa: PLC0415

        key = self._cache_key(model, example_inputs)
        path = self._dir / f"{key}.pt"
        if path.exists():
            _log.info("Neuron model cache hit: %s", path.name)
            return torch.jit.load(str(path))
        return None

    def put(
        self,
        model: Any,
        example_inputs: Sequence[Any],
        compiled: Any,
    ) -> None:
        import torch  # noqa: PLC0415

        key = self._cache_key(model, example_inputs)
        path = self._dir / f"{key}.pt"
        torch.jit.save(compiled, str(path))
        _log.info("Neuron model cache saved: %s", path.name)


# ── compile_for_neuron ────────────────────────────────────────────────────


def compile_for_neuron(
    model: Any,
    example_inputs: Sequence[Any],
    *,
    cache_dir: "Optional[str | Path]" = None,
) -> Any:
    """
    Compile *model* for Neuron execution via ``torch_neuronx.trace()``.

    Checks a persistent disk cache first (keyed on model weights + input
    shapes/dtypes).  On a cache miss the model is traced and the result
    stored.  On subsequent calls the cached compiled model is returned
    without re-tracing.

    If ``torch_neuronx`` is not available the original model is returned
    on CPU with a warning — allowing the same service code to run in
    development environments without an Inf2 instance.

    Args:
        model: A ``torch.nn.Module`` to compile.
        example_inputs: Example tensors matching the shapes the model will
            receive during inference (same contract as ``torch.jit.trace``).
        cache_dir: Override for the trace cache directory.  Defaults to
            ``$URACIL_NEURON_CACHE_DIR`` or ``/tmp/uracil_neuron_cache``.

    Returns:
        A compiled ``torch.jit.ScriptModule`` (Neuron) or the original model
        (CPU fallback).
    """
    if not _ACTIVATED:
        activate()

    resolved_cache = Path(
        cache_dir
        if cache_dir is not None
        else os.environ.get("URACIL_NEURON_CACHE_DIR", "/tmp/uracil_neuron_cache")
    )
    cache = NeuronModelCache(resolved_cache)

    cached = cache.get(model, example_inputs)
    if cached is not None:
        return cached

    if not _NEURON_AVAILABLE:
        warnings.warn(
            "compile_for_neuron: torch_neuronx is not available; "
            "returning the original model on CPU.",
            stacklevel=2,
        )
        return model

    try:
        import torch_neuronx  # noqa: PLC0415

        # torch_neuronx.trace() requires the model and all example inputs to
        # be on CPU.  Our shim may have moved them to XLA already, so we
        # explicitly bring them back before tracing.
        cpu_model = model.to("cpu") if hasattr(model, "to") else model
        cpu_inputs = tuple(
            t.to("cpu") if hasattr(t, "to") else t for t in example_inputs
        )

        _log.info("Tracing model with torch_neuronx.trace() ...")
        traced = torch_neuronx.trace(cpu_model, cpu_inputs)
        cache.put(cpu_model, cpu_inputs, traced)
        return traced
    except Exception as exc:
        raise RuntimeError(f"torch_neuronx.trace() failed: {exc}") from exc


# ── run_wrapped ───────────────────────────────────────────────────────────


def run_wrapped(snippet_path: str) -> None:
    """
    Entry point invoked by ura_stub.c for ``runner.kind == "torch-neuron"``.

    1. Applies all CUDA→Neuron patches via ``activate()``.
    2. Executes the function snippet as ``__main__``.

    The snippet may import ``neuron_torch_shim`` directly to call
    ``compile_for_neuron()`` — since tmpdir is already on sys.path when
    this function is called, the import resolves to this same module.
    """
    activate()

    snippet = Path(snippet_path)
    if not snippet.exists():
        print(f"ura: snippet not found: {snippet_path}", file=sys.stderr)
        sys.exit(1)

    # Make the snippet's directory importable so relative imports work.
    snippet_dir = str(snippet.parent)
    if snippet_dir not in sys.path:
        sys.path.insert(0, snippet_dir)

    source = snippet.read_text(encoding="utf-8")
    code = compile(source, str(snippet), "exec")
    globs: dict[str, Any] = {
        "__file__": str(snippet),
        "__name__": "__main__",
        "__spec__": None,
        "__loader__": None,
        "__builtins__": __builtins__,
    }
    exec(code, globs)  # noqa: S102
