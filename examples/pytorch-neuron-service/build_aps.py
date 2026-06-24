#!/usr/bin/env python3
"""Build the pytorch-neuron-service .ura binary."""

import sys
from pathlib import Path

# Resolve paths relative to this script
THIS_DIR = Path(__file__).resolve().parent
TOOLS_DIR = THIS_DIR.parent.parent / "tools"

sys.path.insert(0, str(TOOLS_DIR))

from ura_builder import build_ura  # noqa: E402

out = build_ura(
    package_dir=THIS_DIR,
    target="x86_64_elf",
    output=THIS_DIR / "dist" / "pytorch-neuron-service.ura",
)
print(f"Built: {out}  ({out.stat().st_size:,} bytes)")
