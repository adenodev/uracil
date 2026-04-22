#!/usr/bin/env python3
from __future__ import annotations

import json
import os
import sys


def main() -> int:
    payload = {
        "service": os.getenv("URACIL_SERVICE_NAME", "unknown"),
        "function": os.getenv("URACIL_FUNCTION_NAME", "echo"),
        "args": sys.argv[1:],
    }
    print(json.dumps(payload))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
