"""
health.py — Report torch-neuron shim state.

Imported via neuron_torch_shim.run_wrapped(), so the shim is already active
when this snippet runs.  Import the shim module to read its state flags.
"""

import importlib
import json
import os
import sys
from pathlib import Path

# neuron_torch_shim is on sys.path (injected by the stub)
shim = importlib.import_module("neuron_torch_shim")

neuron_devices = sorted(str(p) for p in Path("/dev").glob("neuron*")) if Path("/dev").exists() else []

report = {
    "service": os.environ.get("URACIL_SERVICE_NAME", "unknown"),
    "function": "health",
    "runner_kind": os.environ.get("URACIL_RUNNER_KIND", ""),
    "cuda_translate_env": os.environ.get("URACIL_CUDA_TRANSLATE", "0"),
    "shim_activated": shim._ACTIVATED,
    "neuron_available": shim._NEURON_AVAILABLE,
    "neuron_device_target": shim._NEURON_DEVICE_TARGET,
    "neuron_fallback": shim._NEURON_FALLBACK,
    "neuron_devices": neuron_devices,
    "neuron_cache_dir": os.environ.get("URACIL_NEURON_CACHE_DIR", ""),
}

print(json.dumps(report, indent=2))
