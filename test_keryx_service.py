#!/usr/bin/env python3
"""
Build a test Uracil service using the Keryx packager, then run it
through the Uracil C runtime.

This demonstrates the full Keryx → APS → Uracil pipeline:
  1. Define service files (manifest + function snippets)
  2. Package with Keryx's build_aps_archive()
  3. Extract to a service directory
  4. Run through `uracil --bundle`
"""

import io
import json
import os
import subprocess
import sys
import tempfile
import zipfile

# Add Keryx backend to path
KERYX_BACKEND = os.path.join(os.path.dirname(__file__), "..", "keryx", "backend")
sys.path.insert(0, KERYX_BACKEND)

from execution.codegen.packager import build_aps_archive, read_aps_manifest

URACIL_BIN = os.path.join(os.path.dirname(__file__), "build", "uracil")

# ── Service definition (what Keryx codegen would produce) ────────────────

SERVICE_FILES = {
    "manifest.json": json.dumps({
        "name": "sys-info-service",
        "version": "0.1.0",
        "description": "System information service built via Keryx codegen.",
        "runtime": {
            "name": "uracil",
            "abi": "uracil.service.v1",
            "mode": "rootless",
            "hotswap_group": "sys-info-service",
            "payload_revision": "test-001"
        },
        "service": {
            "entry_function": "health",
            "functions": [
                {
                    "name": "health",
                    "description": "Report service health and runtime metadata.",
                    "required_permissions": []
                },
                {
                    "name": "clock-now",
                    "description": "Read the current system time.",
                    "required_permissions": ["clock.read"]
                },
                {
                    "name": "disk-usage",
                    "description": "Show disk usage of the current directory.",
                    "required_permissions": ["fs.read"]
                },
                {
                    "name": "env-info",
                    "description": "Display Uracil runtime environment variables.",
                    "required_permissions": []
                }
            ]
        },
        "permissions": {
            "requested": ["fs.read", "clock.read"]
        }
    }, indent=2),

    "functions/health.py": '''\
#!/usr/bin/env python3
"""Report service health and runtime metadata."""
import os
import sys

service = os.environ.get("URACIL_SERVICE_NAME", "unknown")
version = os.environ.get("URACIL_SERVICE_VERSION", "unknown")
abi = os.environ.get("URACIL_RUNTIME_ABI", "unknown")
rootless = os.environ.get("URACIL_ROOTLESS", "unknown")

print(f"service={service} version={version} abi={abi} rootless={rootless} status=healthy")
sys.exit(0)
''',

    "functions/clock_now.py": '''\
#!/usr/bin/env python3
"""Read the current system time."""
import os
import sys
from datetime import datetime, timezone

granted = os.environ.get("URACIL_GRANTED_PERMISSIONS", "")
if "clock.read" not in granted:
    print("error: clock.read permission not granted", file=sys.stderr)
    sys.exit(1)

now = datetime.now(timezone.utc)
print(f"utc_time={now.strftime('%Y-%m-%d %H:%M:%S')} timezone=UTC")
print(f"unix_timestamp={int(now.timestamp())}")
sys.exit(0)
''',

    "functions/disk_usage.py": '''\
#!/usr/bin/env python3
"""Show disk usage of the service directory."""
import os
import sys

service_dir = os.environ.get("URACIL_SERVICE_DIR", ".")
total_size = 0
file_count = 0

for root, dirs, files in os.walk(service_dir):
    for f in files:
        path = os.path.join(root, f)
        try:
            total_size += os.path.getsize(path)
            file_count += 1
        except OSError:
            pass

print(f"directory={service_dir}")
print(f"files={file_count}")
print(f"total_bytes={total_size}")
print(f"total_kb={total_size / 1024:.1f}")
sys.exit(0)
''',

    "functions/env_info.py": '''\
#!/usr/bin/env python3
"""Display Uracil runtime environment variables."""
import os
import sys

prefix = "URACIL_"
env_vars = sorted(
    (k, v) for k, v in os.environ.items() if k.startswith(prefix)
)

if not env_vars:
    print("no URACIL_* environment variables found")
    sys.exit(1)

for key, value in env_vars:
    print(f"{key}={value}")
sys.exit(0)
''',
}

SERVICE_METADATA = {
    "name": "sys-info-service",
    "version": "0.1.0",
    "description": "System information service built via Keryx codegen.",
}


def main():
    print("=" * 60)
    print("Keryx → APS → Uracil Integration Test")
    print("=" * 60)

    if not os.path.isfile(URACIL_BIN):
        print(f"\nERROR: Uracil binary not found at {URACIL_BIN}")
        print("Run: cmake -S . -B build && cmake --build build")
        sys.exit(1)

    # 1. Build APS archive using Keryx packager
    print("\n[1/5] Building APS archive with Keryx packager...")
    archive_bytes = build_aps_archive(SERVICE_FILES, SERVICE_METADATA)
    manifest = read_aps_manifest(archive_bytes)
    print(f"      Package: {manifest['name']} v{manifest['version']}")
    print(f"      Archive: {len(archive_bytes)} bytes")
    print(f"      Files:   {list(SERVICE_FILES.keys())}")

    # 2. Extract to a temp service directory
    print("\n[2/5] Extracting service bundle...")
    service_dir = tempfile.mkdtemp(prefix="uracil-test-")
    with zipfile.ZipFile(io.BytesIO(archive_bytes), "r") as zf:
        zf.extractall(service_dir)

    print(f"      Extracted to: {service_dir}")
    for root, dirs, files in os.walk(service_dir):
        for f in files:
            rel = os.path.relpath(os.path.join(root, f), service_dir)
            print(f"        {rel}")

    # 3. Describe the bundle through Uracil
    print("\n[3/5] Uracil describe:")
    result = subprocess.run(
        [URACIL_BIN, "--bundle", service_dir, "describe"],
        capture_output=True, text=True
    )
    for line in result.stdout.strip().split("\n"):
        print(f"      {line}")
    assert result.returncode == 0, f"describe failed: {result.stderr}"

    # 4. Run health (no permissions needed)
    print("\n[4/5] Running functions...")

    print("\n  health (no permissions required):")
    result = subprocess.run(
        [URACIL_BIN, "--bundle", service_dir, "run", "health"],
        capture_output=True, text=True
    )
    for line in result.stdout.strip().split("\n"):
        print(f"      {line}")
    assert result.returncode == 0, f"health failed: {result.stderr}"

    print("\n  env-info (no permissions required):")
    result = subprocess.run(
        [URACIL_BIN, "--bundle", service_dir, "run", "env-info"],
        capture_output=True, text=True
    )
    for line in result.stdout.strip().split("\n"):
        print(f"      {line}")
    assert result.returncode == 0, f"env-info failed: {result.stderr}"

    print("\n  clock-now (requires clock.read — should be denied):")
    result = subprocess.run(
        [URACIL_BIN, "--bundle", service_dir, "run", "clock-now"],
        capture_output=True, text=True
    )
    denied = result.returncode != 0
    for line in result.stderr.strip().split("\n"):
        print(f"      {line}")
    assert denied, "clock-now should have been denied without grant"
    print("      → Correctly denied (no grant)")

    # 5. Grant permissions and re-run
    print("\n[5/5] Granting permissions and re-running...")

    print("\n  grant clock.read fs.read:")
    result = subprocess.run(
        [URACIL_BIN, "--bundle", service_dir, "grant", "clock.read", "fs.read"],
        capture_output=True, text=True
    )
    for line in result.stderr.strip().split("\n"):
        print(f"      {line}")
    assert result.returncode == 0, f"grant failed: {result.stderr}"

    print("\n  permissions:")
    result = subprocess.run(
        [URACIL_BIN, "--bundle", service_dir, "permissions"],
        capture_output=True, text=True
    )
    for line in result.stdout.strip().split("\n"):
        print(f"      {line}")

    print("\n  clock-now (now granted):")
    result = subprocess.run(
        [URACIL_BIN, "--bundle", service_dir, "run", "clock-now"],
        capture_output=True, text=True
    )
    for line in result.stdout.strip().split("\n"):
        print(f"      {line}")
    assert result.returncode == 0, f"clock-now failed after grant: {result.stderr}"

    print("\n  disk-usage (now granted):")
    result = subprocess.run(
        [URACIL_BIN, "--bundle", service_dir, "run", "disk-usage"],
        capture_output=True, text=True
    )
    for line in result.stdout.strip().split("\n"):
        print(f"      {line}")
    assert result.returncode == 0, f"disk-usage failed after grant: {result.stderr}"

    # Cleanup
    import shutil
    shutil.rmtree(service_dir)

    print("\n" + "=" * 60)
    print("All tests passed! Full Keryx → APS → Uracil pipeline working.")
    print("=" * 60)


if __name__ == "__main__":
    main()
