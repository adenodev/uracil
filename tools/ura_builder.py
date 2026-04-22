#!/usr/bin/env python3
"""
Uracil .ura binary builder.

Takes an .aps package (installed directory or archive) and produces a native
executable for a target platform by:
  1. Compiling the C stub for the target
  2. Packing all package files into a flat payload archive
  3. Appending: payload + trailer to the compiled stub
"""

import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

STUB_SRC = Path(__file__).resolve().parent.parent / "stub" / "ura_stub.c"

# Trailer magic — must match ura_stub.c exactly
URA_MAGIC = b"\x55\x52\x41\x01\x53\x56\x43\x00"  # 8 bytes: URA\x01SVC\x00
assert len(URA_MAGIC) == 8
TRAILER_LEN = 16  # 8 B payload size + 8 B magic

TARGETS = {
    "x86_64_elf": {
        "cc": ["gcc", "cc"],
        "cflags": ["-O2", "-s", "-Wall"],
        "ldflags_try": ["-static"],  # try static first, fall back to dynamic
        "ext": ".ura",
        "platform_str": "x86_64-elf",
    },
    "x86_64_win": {
        "cc": ["x86_64-w64-mingw32-gcc"],
        "cflags": ["-O2", "-s", "-Wall"],
        "ldflags_try": ["-static"],
        "ext": ".exe",
        "platform_str": "x86_64-win",
    },
    "riscv_elf": {
        "cc": ["riscv64-linux-gnu-gcc"],
        "cflags": ["-O2", "-s", "-Wall"],
        "ldflags_try": ["-static"],
        "ext": ".ura",
        "platform_str": "riscv64-elf",
    },
}

# Accept user-facing aliases
TARGET_ALIASES = {
    "x86_64_ELF": "x86_64_elf",
    "x86_64_WIN": "x86_64_win",
    "risc_v_ELF": "riscv_elf",
    "riscv_ELF":  "riscv_elf",
    "riscv64_elf": "riscv_elf",
}


def resolve_target(name: str) -> str:
    """Normalize target name to internal key."""
    canonical = TARGET_ALIASES.get(name, name.lower())
    if canonical not in TARGETS:
        raise ValueError(
            f"Unknown target '{name}'. Available: "
            + ", ".join(sorted(TARGETS) + sorted(TARGET_ALIASES))
        )
    return canonical


def find_compiler(target: str) -> str:
    cfg = TARGETS[target]
    for cc in cfg["cc"]:
        if shutil.which(cc):
            return cc
    raise RuntimeError(
        f"No compiler found for target '{target}'. "
        f"Install one of: {', '.join(cfg['cc'])}"
    )


def compile_stub(target: str, output: Path) -> None:
    """Compile ura_stub.c for the given target platform."""
    cc = find_compiler(target)
    cfg = TARGETS[target]
    base = [cc] + cfg["cflags"]

    # Try with static linking first
    if cfg.get("ldflags_try"):
        cmd = base + cfg["ldflags_try"] + ["-o", str(output), str(STUB_SRC)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode == 0:
            return

    # Fall back to dynamic linking
    cmd = base + ["-o", str(output), str(STUB_SRC)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"Compilation failed:\n{r.stderr}")


def build_meta(manifest: dict, target: str) -> str:
    """Convert an APS manifest.json into the .ura.meta line format."""
    cfg = TARGETS[target]
    lines = []

    lines.append(f"name {manifest['name']}")
    lines.append(f"version {manifest['version']}")

    runtime = manifest.get("runtime") or {}
    lines.append(f"abi {runtime.get('abi', 'uracil.service.v1')}")
    lines.append(f"mode {runtime.get('mode', 'rootless')}")
    lines.append(f"platform {cfg['platform_str']}")

    service = manifest.get("service") or {}
    entry = service.get("entry_function", "")
    functions = service.get("functions") or []
    if not entry and functions:
        entry = functions[0]["name"]
    lines.append(f"entry {entry}")

    # Function entries
    for fn in functions:
        lines.append(f"fn {fn['name']} {fn['snippet_path']}")
    # Descriptions
    for fn in functions:
        desc = fn.get("description", "")
        if desc:
            lines.append(f"desc {fn['name']} {desc}")
    # Per-function requirements
    for fn in functions:
        reqs = fn.get("requires") or []
        req_str = " ".join(reqs) if reqs else ""
        lines.append(f"req {fn['name']}{(' ' + req_str) if req_str else ''}")

    # Package-level permissions
    perms = (manifest.get("permissions") or {}).get("requested") or []
    if perms:
        lines.append(f"perm {' '.join(perms)}")

    return "\n".join(lines) + "\n"


def pack_payload(files: dict, meta_content: str) -> bytes:
    """
    Pack files into the flat archive format:
      [uint16_le path_len] [path bytes] [uint32_le data_len] [data bytes]
    .ura.meta is written first.
    """
    buf = io.BytesIO()

    def _write_entry(name: str, data: bytes):
        name_bytes = name.encode("utf-8")
        buf.write(struct.pack("<H", len(name_bytes)))
        buf.write(name_bytes)
        buf.write(struct.pack("<I", len(data)))
        buf.write(data)

    # Meta first
    _write_entry(".ura.meta", meta_content.encode("utf-8"))

    # All other files
    for path in sorted(files):
        _write_entry(path, files[path])

    return buf.getvalue()


def load_package(
    package_dir: Path | None = None,
    aps_archive: Path | None = None,
) -> tuple[dict, dict]:
    """
    Load manifest and file map from an installed package directory
    or an .aps archive.  Returns (manifest_dict, {relative_path: bytes}).
    """
    if aps_archive:
        with zipfile.ZipFile(aps_archive, "r") as zf:
            manifest = json.loads(zf.read("manifest.json"))
            files = {}
            for name in zf.namelist():
                files[name] = zf.read(name)
            return manifest, files

    if package_dir:
        mf = package_dir / "manifest.json"
        if not mf.exists():
            raise FileNotFoundError(f"No manifest.json in {package_dir}")
        manifest = json.loads(mf.read_text())
        files = {}
        for p in package_dir.rglob("*"):
            if p.is_file():
                rel = str(p.relative_to(package_dir))
                files[rel] = p.read_bytes()
        return manifest, files

    raise ValueError("Either package_dir or aps_archive must be provided")


def build_ura(
    package_dir: Path | None = None,
    aps_archive: Path | None = None,
    target: str = "x86_64_elf",
    output: Path | None = None,
) -> Path:
    """Build a .ura binary from an installed package or .aps archive."""
    target = resolve_target(target)
    manifest, files = load_package(package_dir, aps_archive)

    # Default output name
    if not output:
        cfg = TARGETS[target]
        name = manifest["name"].replace(".", "-")
        output = Path(f"{name}{cfg['ext']}")

    # Build meta
    meta = build_meta(manifest, target)

    # Pack payload
    payload = pack_payload(files, meta)

    # Compile stub to a temp file
    ext = TARGETS[target]["ext"]
    fd, tmp = tempfile.mkstemp(suffix=ext)
    os.close(fd)
    tmp_path = Path(tmp)
    try:
        compile_stub(target, tmp_path)

        # Concatenate: stub + payload + trailer
        stub_bytes = tmp_path.read_bytes()
        trailer = struct.pack("<Q", len(payload)) + URA_MAGIC
        output.write_bytes(stub_bytes + payload + trailer)
        output.chmod(0o755)
    finally:
        tmp_path.unlink(missing_ok=True)

    return output


def main():
    import argparse

    parser = argparse.ArgumentParser(
        prog="ura-builder",
        description="Build a .ura service binary from an APS package",
    )
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument(
        "--package-dir", type=Path,
        help="Path to an installed package directory",
    )
    group.add_argument(
        "--aps-archive", type=Path,
        help="Path to an .aps archive file",
    )
    parser.add_argument(
        "--target", default="x86_64_elf",
        help=f"Target platform ({', '.join(sorted(TARGETS))})",
    )
    parser.add_argument(
        "-o", "--output", type=Path,
        help="Output file path",
    )

    args = parser.parse_args()
    try:
        out = build_ura(
            package_dir=args.package_dir,
            aps_archive=args.aps_archive,
            target=args.target,
            output=args.output,
        )
        size = out.stat().st_size
        print(f"Built: {out}  ({size:,} bytes)")
    except Exception as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
