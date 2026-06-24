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
        "cxx": ["c++", "g++", "clang++"],
        "cflags": ["-O2", "-s", "-Wall"],
        "ldflags_try": ["-static"],  # try static first, fall back to dynamic
        "ext": ".ura",
        "platform_str": "x86_64-elf",
    },
    "x86_64_win": {
        "cc": ["x86_64-w64-mingw32-gcc"],
        "cxx": ["x86_64-w64-mingw32-g++"],
        "cflags": ["-O2", "-s", "-Wall"],
        "ldflags_try": ["-static"],
        "ext": ".exe",
        "platform_str": "x86_64-win",
    },
    "riscv_elf": {
        "cc": ["riscv64-linux-gnu-gcc"],
        "cxx": ["riscv64-linux-gnu-g++"],
        "cflags": ["-O2", "-s", "-Wall"],
        "ldflags_try": ["-static"],
        "ext": ".ura",
        "platform_str": "riscv64-elf",
    },
    "wasm32": {
        "cc": ["emcc"],
        "cxx": ["em++"],
        "cflags": ["-O2", "-s", "-Wall", "-sWASM=1", "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap']", "-sEXPORTED_FUNCTIONS=['_malloc','_free']"],
        "ldflags_try": [],
        "ext": ".wasm",
        "platform_str": "wasm32",
    },
}

# Accept user-facing aliases
TARGET_ALIASES = {
    "x86_64_ELF": "x86_64_elf",
    "x86_64_WIN": "x86_64_win",
    "risc_v_ELF": "riscv_elf",
    "riscv_ELF":  "riscv_elf",
    "riscv64_elf": "riscv_elf",
    "wasm": "wasm32",
    "WASM": "wasm32",
    "wasm32": "wasm32",
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


def find_cpp_compiler(target: str) -> str:
    cfg = TARGETS[target]
    for compiler in cfg["cxx"]:
        if shutil.which(compiler):
            return compiler
    raise RuntimeError(
        f"No C++ compiler found for target '{target}'. "
        f"Install one of: {', '.join(cfg['cxx'])}"
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


def _runner_virtualization(runner: dict) -> dict:
    virtualization = runner.get("virtualization") or {}
    return virtualization if isinstance(virtualization, dict) else {}


def _function_language(function: dict) -> str:
    value = str(function.get("language") or function.get("runtime") or "python").strip().lower()
    if value in {"cpp", "c++", "cxx", "native"}:
        return "cpp"
    return "python"


def _security_cauth(manifest: dict) -> dict:
    security = manifest.get("security") or {}
    cauth = security.get("cauth") or {}
    return cauth if isinstance(cauth, dict) else {}


def _runner_config_json(runner: dict) -> str:
    return json.dumps(runner, sort_keys=True, separators=(",", ":"))


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
    if runtime.get("target_env"):
        lines.append(f"target-env {runtime['target_env']}")

    service = manifest.get("service") or {}
    entry = service.get("entry_function", "")
    functions = service.get("functions") or []
    if not entry and functions:
        entry = functions[0]["name"]
    lines.append(f"entry {entry}")

    # Function entries
    for fn in functions:
        entry_path = fn.get("binary_path") or fn.get("snippet_path") or fn.get("entry_point")
        lines.append(f"fn {fn['name']} {entry_path}")
        lines.append(f"fn-language {fn['name']} {_function_language(fn)}")
    # Descriptions
    for fn in functions:
        desc = fn.get("description", "")
        if desc:
            lines.append(f"desc {fn['name']} {desc}")
    # Per-function requirements
    for fn in functions:
        reqs = fn.get("required_permissions") or fn.get("requires") or []
        req_str = " ".join(reqs) if reqs else ""
        lines.append(f"req {fn['name']}{(' ' + req_str) if req_str else ''}")

    # Package-level permissions
    perms = (manifest.get("permissions") or {}).get("requested") or []
    if perms:
        lines.append(f"perm {' '.join(perms)}")

    cauth = _security_cauth(manifest)
    if cauth:
        lines.append(f"cauth-enabled {'true' if cauth.get('enabled', True) else 'false'}")
        for field, meta_key in (
            ("mode", "cauth-mode"),
            ("base_url", "cauth-base-url"),
            ("issuer", "cauth-issuer"),
            ("audience", "cauth-audience"),
            ("token_env", "cauth-token-env"),
            ("secret_key_env", "cauth-secret-key-env"),
            ("encryption_key_env", "cauth-encryption-key-env"),
        ):
            value = cauth.get(field)
            if value:
                lines.append(f"{meta_key} {value}")

    runner = manifest.get("runner") or {}
    if runner:
        kind = runner.get("kind") or runner.get("name") or ""
        if kind:
            lines.append(f"runner {kind}")

        protocol = runner.get("protocol") or ""
        if protocol:
            lines.append(f"runner-protocol {protocol}")

        bind = runner.get("bind") or ""
        if bind:
            lines.append(f"runner-bind {bind}")

        model = runner.get("model") or {}
        model_format = model.get("format") or runner.get("model_format") or ""
        if model_format:
            lines.append(f"runner-model-format {model_format}")

        model_ref = model.get("uri") or model.get("path") or runner.get("model_uri") or ""
        if model_ref:
            lines.append(f"runner-model-ref {model_ref}")

        virtualization = _runner_virtualization(runner)
        target_platform = runner.get("platform") or runner.get("target") or virtualization.get("target") or ""
        if target_platform:
            lines.append(f"runner-target {target_platform}")

        accelerator = runner.get("accelerator") or virtualization.get("accelerator") or ""
        if accelerator:
            lines.append(f"runner-accelerator {accelerator}")

        source_format = virtualization.get("source_format") or ""
        if source_format:
            lines.append(f"runner-source-format {source_format}")

        runtime_format = virtualization.get("runtime_format") or ""
        if runtime_format:
            lines.append(f"runner-runtime-format {runtime_format}")

        lines.append(f"runner-config {_runner_config_json(runner)}")

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
            if not p.is_file():
                continue
            rel = str(p.relative_to(package_dir))
            # Skip build output directories and compiled service binaries.
            if rel.startswith("dist/") or rel.startswith("build/"):
                continue
            if p.suffix == ".ura":
                continue
            files[rel] = p.read_bytes()
        return manifest, files

    raise ValueError("Either package_dir or aps_archive must be provided")


def _write_temp_package(root: Path, files: dict[str, bytes]) -> None:
    for relative_path, content in files.items():
        target = root / relative_path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content)


def _compile_native_function(target: str, package_root: Path, function: dict) -> str:
    function_name = str(function.get("name") or "native")
    source_rel = str(function.get("snippet_path") or function.get("entry_point") or "").strip()
    if not source_rel:
        raise RuntimeError(f"Native function '{function_name}' is missing snippet_path")

    source_path = (package_root / source_rel).resolve()
    source_path.relative_to(package_root.resolve())
    if not source_path.exists():
        raise RuntimeError(f"Native function source not found: {source_rel}")

    output_rel = str(function.get("binary_path") or f"native/{function_name}").strip()
    output_path = (package_root / output_rel).resolve()
    output_path.relative_to(package_root.resolve())
    output_path.parent.mkdir(parents=True, exist_ok=True)

    build = function.get("build") or {}
    std = str(build.get("standard") or build.get("std") or "c++20")
    flags = [str(value) for value in (build.get("flags") or ["-O3"])]
    include_dirs = [str(value) for value in (build.get("include_dirs") or [])]
    libraries = [str(value) for value in (build.get("libraries") or [])]

    command = [find_cpp_compiler(target), str(source_path), "-o", str(output_path), f"-std={std}", *flags]
    for include in include_dirs:
        include_path = Path(include)
        if not include_path.is_absolute():
            include_path = (package_root / include).resolve()
        command.extend(["-I", str(include_path)])
    command.extend(libraries)

    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip() or "native compile failed")

    output_path.chmod(0o755)
    return str(output_path.relative_to(package_root))


def _embed_native_functions(manifest: dict, files: dict[str, bytes], target: str) -> tuple[dict, dict[str, bytes]]:
    manifest_copy = json.loads(json.dumps(manifest))
    functions = (manifest_copy.get("service") or {}).get("functions") or []
    if not any(_function_language(function) == "cpp" for function in functions):
        return manifest_copy, files

    with tempfile.TemporaryDirectory() as tmp_dir:
        package_root = Path(tmp_dir)
        _write_temp_package(package_root, files)
        updated_files = dict(files)

        for function in functions:
            if _function_language(function) != "cpp":
                continue
            binary_rel = _compile_native_function(target, package_root, function)
            function["binary_path"] = binary_rel
            updated_files[binary_rel] = (package_root / binary_rel).read_bytes()

        updated_files["manifest.json"] = json.dumps(manifest_copy, indent=2).encode("utf-8")
        return manifest_copy, updated_files


def build_ura(
    package_dir: Path | None = None,
    aps_archive: Path | None = None,
    target: str = "x86_64_elf",
    output: Path | None = None,
) -> Path:
    """Build a .ura binary from an installed package or .aps archive."""
    target = resolve_target(target)
    manifest, files = load_package(package_dir, aps_archive)
    manifest, files = _embed_native_functions(manifest, files, target)

    # For torch-neuron runners, embed the CUDA→Neuron shim into the payload so
    # ura_stub.c can locate it at $tmpdir/neuron_torch_shim.py at runtime.
    runner_kind = (manifest.get("runner") or {}).get("kind", "")
    if runner_kind == "torch-neuron":
        shim_src = Path(__file__).resolve().parent / "neuron_torch_shim.py"
        if not shim_src.exists():
            raise FileNotFoundError(
                f"neuron_torch_shim.py not found at {shim_src}. "
                "It must live in the same directory as ura_builder.py."
            )
        files["neuron_torch_shim.py"] = shim_src.read_bytes()

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
