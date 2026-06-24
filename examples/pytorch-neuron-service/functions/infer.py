"""
infer.py — Demonstrate CUDA→Neuron transparent translation.

Written as normal CUDA-targeting PyTorch code.  When run inside Uracil with
runner.kind == "torch-neuron", neuron_torch_shim rewrites all CUDA references
to XLA (Neuron) at the Python level before this snippet executes.

On a dev machine without torch_neuronx the shim falls back to CPU so the
same binary is testable locally.
"""

import json
import os
import sys

import torch
import torch.nn as nn

# neuron_torch_shim is importable from tmpdir (added to sys.path by run_wrapped)
import neuron_torch_shim

# ── Model definition ──────────────────────────────────────────────────────

class TwoLayerNet(nn.Module):
    """Minimal feed-forward net: input_dim=4 → hidden=8 → output=2."""

    def __init__(self) -> None:
        super().__init__()
        self.fc1 = nn.Linear(4, 8)
        self.fc2 = nn.Linear(8, 2)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return self.fc2(torch.relu(self.fc1(x)))


# ── Device placement using standard CUDA idioms ───────────────────────────
#
#   All three forms below are intercepted and redirected by the shim:
#     • torch.device("cuda")     → _NeuronDevice → resolves to 'xla' or 'cpu'
#     • model.cuda()             → model.to('xla' | 'cpu')
#     • tensor.to("cuda")        → tensor.to('xla' | 'cpu')

device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
print(f"[infer] effective device: {device}", file=sys.stderr)

model = TwoLayerNet()
model.cuda()  # redirected by shim
model.eval()

example = torch.randn(1, 4)
example_on_device = example.to("cuda")  # redirected by shim

# ── Compile / trace for Neuron ────────────────────────────────────────────
#
# compile_for_neuron() uses torch_neuronx.trace() on Inf2 and returns a
# cached ScriptModule on repeated calls.  Falls back to the original model
# on CPU when torch_neuronx is absent.

compiled_model = neuron_torch_shim.compile_for_neuron(
    model,
    [example_on_device],
)

# ── Inference ─────────────────────────────────────────────────────────────

with torch.no_grad():
    output = compiled_model(example_on_device)

result = {
    "service": os.environ.get("URACIL_SERVICE_NAME", "unknown"),
    "function": "infer",
    "runner_kind": os.environ.get("URACIL_RUNNER_KIND", ""),
    "cuda_translate": os.environ.get("URACIL_CUDA_TRANSLATE", "0") == "1",
    "neuron_available": neuron_torch_shim._NEURON_AVAILABLE,
    "effective_device": str(device),
    "input_shape": list(example.shape),
    "output": output.tolist(),
}

print(json.dumps(result, indent=2))
