#!/usr/bin/env python3
from __future__ import annotations

import json
import os


def main() -> int:
    payload = {
        "service": os.getenv("URACIL_SERVICE_NAME", "unknown"),
        "function": os.getenv("URACIL_FUNCTION_NAME", "health"),
        "runner_kind": os.getenv("URACIL_RUNNER_KIND", ""),
        "runner_protocol": os.getenv("URACIL_RUNNER_PROTOCOL", "http"),
        "runner_bind": os.getenv("URACIL_RUNNER_BIND", "127.0.0.1:8091"),
        "runner_model_ref": os.getenv("URACIL_RUNNER_MODEL_REF", ""),
        "runner_target": os.getenv("URACIL_RUNNER_TARGET", ""),
        "runner_accelerator": os.getenv("URACIL_RUNNER_ACCELERATOR", ""),
    }
    print(json.dumps(payload))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
