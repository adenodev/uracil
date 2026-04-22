#!/usr/bin/env python3
from __future__ import annotations

import json
import os


def main() -> int:
    payload = {
        "service": os.getenv("URACIL_SERVICE_NAME", "unknown"),
        "version": os.getenv("URACIL_SERVICE_VERSION", "unknown"),
        "function": os.getenv("URACIL_FUNCTION_NAME", "health"),
        "status": "ok",
        "rootless": os.getenv("URACIL_ROOTLESS", "false"),
    }
    print(json.dumps(payload))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
