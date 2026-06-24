#!/usr/bin/env python3
"""Host-side Uracil runtime for APS-installed dynamic service bundles."""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

URACIL_ABI = "uracil.service.v1"


def _error(message: str) -> int:
    print(f"uracil: error: {message}", file=sys.stderr)
    return 1


def _sanitize_service_name(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9_-]+", "-", name).strip("-") or "service"


def _service_dir(path_value: str) -> Path:
    return Path(path_value).expanduser().resolve()


def _manifest_path(service_dir: Path) -> Path:
    return service_dir / "manifest.json"


def _load_manifest(service_dir: Path) -> dict[str, Any]:
    manifest_file = _manifest_path(service_dir)
    if not manifest_file.exists():
        raise ValueError(f"missing manifest.json in {service_dir}")

    manifest = json.loads(manifest_file.read_text())
    runtime = manifest.get("runtime") or {}
    service = manifest.get("service") or {}

    if runtime.get("name") != "uracil":
        raise ValueError("manifest does not target runtime.name='uracil'")
    if runtime.get("abi") != URACIL_ABI:
        raise ValueError(f"manifest ABI must be {URACIL_ABI}")
    if not isinstance(service.get("functions"), list) or not service["functions"]:
        raise ValueError("manifest.service.functions must contain at least one function")

    return manifest


def _requested_permissions(manifest: dict[str, Any]) -> list[str]:
    permissions = manifest.get("permissions") or {}
    values = permissions.get("requested") or []
    return [str(value) for value in values]


def _function_list(manifest: dict[str, Any]) -> list[dict[str, Any]]:
    service = manifest.get("service") or {}
    return [value for value in service.get("functions") or [] if isinstance(value, dict)]


def _function_map(manifest: dict[str, Any]) -> dict[str, dict[str, Any]]:
    mapping: dict[str, dict[str, Any]] = {}
    for function in _function_list(manifest):
        name = str(function.get("name") or "").strip()
        if name:
            mapping[name] = function
    return mapping


def _function_permissions(function: dict[str, Any]) -> list[str]:
    values = function.get("required_permissions") or []
    return [str(value) for value in values]


def _function_language(function: dict[str, Any]) -> str:
    value = str(function.get("language") or function.get("runtime") or "python").strip().lower()
    if value in {"cpp", "c++", "cxx", "native"}:
        return "cpp"
    return "python"


def _function_binary_path(function: dict[str, Any]) -> str:
    return str(function.get("binary_path") or function.get("compiled_path") or "").strip()


def _service_path(service_dir: Path, relative: str) -> Path:
    candidate = (service_dir / relative).resolve()
    candidate.relative_to(service_dir)
    return candidate


def _security_cauth(manifest: dict[str, Any]) -> dict[str, Any]:
    security = manifest.get("security") or {}
    cauth = security.get("cauth") or {}
    return cauth if isinstance(cauth, dict) else {}


def _target_env(manifest: dict[str, Any]) -> str:
    runtime = manifest.get("runtime") or {}
    return str(runtime.get("target_env") or os.getenv("URACIL_TARGET_ENV") or "")


def _runner_config(manifest: dict[str, Any]) -> dict[str, Any]:
    runner = manifest.get("runner") or {}
    return runner if isinstance(runner, dict) else {}


def _runner_virtualization(runner: dict[str, Any]) -> dict[str, Any]:
    virtualization = runner.get("virtualization") or {}
    return virtualization if isinstance(virtualization, dict) else {}


def _runner_model_ref(runner: dict[str, Any]) -> str:
    model = runner.get("model") or {}
    if not isinstance(model, dict):
        model = {}
    return str(model.get("uri") or model.get("path") or runner.get("model_uri") or "")


def _runner_model_format(runner: dict[str, Any]) -> str:
    model = runner.get("model") or {}
    if not isinstance(model, dict):
        model = {}
    return str(model.get("format") or runner.get("model_format") or "")


def _runner_summary_lines(runner: dict[str, Any]) -> list[str]:
    if not runner:
        return []

    virtualization = _runner_virtualization(runner)
    lines: list[str] = []

    kind = str(runner.get("kind") or runner.get("name") or "")
    if kind:
        lines.append(f"runner kind:    {kind}")

    protocol = str(runner.get("protocol") or "")
    if protocol:
        lines.append(f"runner proto:   {protocol}")

    bind = str(runner.get("bind") or "")
    if bind:
        lines.append(f"runner bind:    {bind}")

    model_format = _runner_model_format(runner)
    if model_format:
        lines.append(f"runner format:  {model_format}")

    model_ref = _runner_model_ref(runner)
    if model_ref:
        lines.append(f"runner model:   {model_ref}")

    target = str(runner.get("platform") or runner.get("target") or virtualization.get("target") or "")
    if target:
        lines.append(f"runner target:  {target}")

    accelerator = str(runner.get("accelerator") or virtualization.get("accelerator") or "")
    if accelerator:
        lines.append(f"runner accel:   {accelerator}")

    source_format = str(virtualization.get("source_format") or "")
    if source_format:
        lines.append(f"runner source:  {source_format}")

    runtime_format = str(virtualization.get("runtime_format") or "")
    if runtime_format:
        lines.append(f"runner runtime: {runtime_format}")

    lines.append(f"runner config:  {json.dumps(runner, sort_keys=True)}")
    return lines


def _apply_runner_env(env: dict[str, str], runner: dict[str, Any]) -> None:
    if not runner:
        return

    virtualization = _runner_virtualization(runner)
    env["URACIL_RUNNER_CONFIG_JSON"] = json.dumps(runner, sort_keys=True)

    kind = str(runner.get("kind") or runner.get("name") or "")
    if kind:
        env["URACIL_RUNNER_KIND"] = kind

    protocol = str(runner.get("protocol") or "")
    if protocol:
        env["URACIL_RUNNER_PROTOCOL"] = protocol

    bind = str(runner.get("bind") or "")
    if bind:
        env["URACIL_RUNNER_BIND"] = bind

    model_format = _runner_model_format(runner)
    if model_format:
        env["URACIL_RUNNER_MODEL_FORMAT"] = model_format

    model_ref = _runner_model_ref(runner)
    if model_ref:
        env["URACIL_RUNNER_MODEL_REF"] = model_ref

    target = str(runner.get("platform") or runner.get("target") or virtualization.get("target") or "")
    if target:
        env["URACIL_RUNNER_TARGET"] = target

    accelerator = str(runner.get("accelerator") or virtualization.get("accelerator") or "")
    if accelerator:
        env["URACIL_RUNNER_ACCELERATOR"] = accelerator

    source_format = str(virtualization.get("source_format") or "")
    if source_format:
        env["URACIL_RUNNER_SOURCE_FORMAT"] = source_format

    runtime_format = str(virtualization.get("runtime_format") or "")
    if runtime_format:
        env["URACIL_RUNNER_RUNTIME_FORMAT"] = runtime_format


def _grant_store_path(service_name: str) -> Path:
    grants_dir = Path.home() / ".uracil" / "grants"
    grants_dir.mkdir(parents=True, exist_ok=True)
    return grants_dir / f"{_sanitize_service_name(service_name)}.perm"


def _load_grants(service_name: str, requested: list[str]) -> list[str]:
    grant_file = _grant_store_path(service_name)
    if not grant_file.exists():
        return []

    requested_set = set(requested)
    granted: list[str] = []
    for line in grant_file.read_text().splitlines():
        permission = line.strip()
        if permission and permission in requested_set and permission not in granted:
            granted.append(permission)
    return granted


def _save_grants(service_name: str, requested: list[str], granted: list[str]) -> None:
    requested_set = set(requested)
    filtered = [permission for permission in granted if permission in requested_set]
    _grant_store_path(service_name).write_text("\n".join(filtered) + ("\n" if filtered else ""))


def _format_permissions(values: list[str]) -> str:
    return ", ".join(values) if values else "(none)"


def _resolve_function(manifest: dict[str, Any], function_name: str | None) -> tuple[str, dict[str, Any]]:
    service = manifest.get("service") or {}
    functions = _function_map(manifest)
    target_name = function_name or str(service.get("entry_function") or "").strip()

    if not target_name:
        if len(functions) == 1:
            target_name = next(iter(functions))
        else:
            raise ValueError("function name is required because the service has no entry_function")

    function = functions.get(target_name)
    if function is None:
        raise ValueError(f"unknown function: {target_name}")
    return target_name, function


def _snippet_path(service_dir: Path, function_name: str, function: dict[str, Any]) -> Path:
    relative = str(function.get("snippet_path") or function.get("entry_point") or "").strip()
    if not relative:
        relative = f"functions/{function_name.replace('-', '_')}.py"

    candidate = _service_path(service_dir, relative)
    if not candidate.exists():
        raise ValueError(f"snippet file not found: {relative}")
    return candidate


def _native_cache_dir(state_dir: Path, manifest: dict[str, Any]) -> Path:
    service_name = _sanitize_service_name(str(manifest.get("name") or "service"))
    revision = str((manifest.get("runtime") or {}).get("payload_revision") or manifest.get("version") or "dev")
    root = Path(os.getenv("URACIL_BINARY_CACHE_DIR") or (state_dir / "compiled"))
    cache_dir = root / service_name / revision
    cache_dir.mkdir(parents=True, exist_ok=True)
    return cache_dir


def _native_compiler(function: dict[str, Any]) -> str:
    build = function.get("build") or {}
    candidates: list[str] = []
    compiler = str(build.get("compiler") or "").strip()
    if compiler:
        candidates.append(compiler)
    candidates.extend(["c++", "g++", "clang++"])
    for candidate in candidates:
        if candidate and shutil.which(candidate):
            return candidate
    raise ValueError("no C++ compiler found for native Uracil function")


def _native_entry_path(
    service_dir: Path,
    state_dir: Path,
    manifest: dict[str, Any],
    function_name: str,
    function: dict[str, Any],
) -> Path:
    binary_rel = _function_binary_path(function)
    if binary_rel:
        candidate = _service_path(service_dir, binary_rel)
        if candidate.exists():
            candidate.chmod(candidate.stat().st_mode | 0o111)
            return candidate

    source_path = _snippet_path(service_dir, function_name, function)
    if source_path.suffix.lower() not in {".cc", ".cpp", ".cxx", ".c++"}:
        if os.access(source_path, os.X_OK):
            return source_path
        raise ValueError(f"native function source must be C++ or executable: {source_path.name}")

    build = function.get("build") or {}
    cache_dir = _native_cache_dir(state_dir, manifest)
    output_name = re.sub(r"[^A-Za-z0-9_.-]+", "-", Path(binary_rel or function_name).name).strip("-") or "native-fn"
    if sys.platform == "win32" and not output_name.endswith(".exe"):
        output_name += ".exe"
    output_path = cache_dir / output_name
    if not output_path.exists() or output_path.stat().st_mtime < source_path.stat().st_mtime:
        compiler = _native_compiler(function)
        std = str(build.get("standard") or build.get("std") or "c++20")
        flags = [str(value) for value in (build.get("flags") or ["-O3"])]
        include_dirs = [str(value) for value in (build.get("include_dirs") or [])]
        libraries = [str(value) for value in (build.get("libraries") or [])]
        command = [compiler, str(source_path), "-o", str(output_path), f"-std={std}", *flags]
        for include in include_dirs:
            include_path = Path(include)
            if not include_path.is_absolute():
                include_path = _service_path(service_dir, include)
            command.extend(["-I", str(include_path)])
        command.extend(libraries)
        result = subprocess.run(command, cwd=str(service_dir), capture_output=True, text=True)
        if result.returncode != 0:
            detail = result.stderr.strip() or result.stdout.strip() or "native compile failed"
            raise ValueError(detail)
        output_path.chmod(0o755)
    return output_path


def _apply_security_env(env: dict[str, str], manifest: dict[str, Any]) -> None:
    target_env = _target_env(manifest)
    if target_env:
        env["URACIL_TARGET_ENV"] = target_env
        env["URACIL_KERYX_AMI"] = "true" if target_env == "keryx-ami" else "false"

    cauth = _security_cauth(manifest)
    if not cauth:
        return

    env["URACIL_CAUTH_ENABLED"] = "true" if cauth.get("enabled", True) else "false"
    field_map = {
        "mode": "URACIL_CAUTH_MODE",
        "base_url": "URACIL_CAUTH_BASE_URL",
        "issuer": "URACIL_CAUTH_ISSUER",
        "audience": "URACIL_CAUTH_AUDIENCE",
        "token_env": "URACIL_CAUTH_TOKEN_ENV",
        "secret_key_env": "URACIL_CAUTH_SECRET_KEY_ENV",
        "encryption_key_env": "URACIL_CAUTH_ENCRYPTION_KEY_ENV",
    }
    for field, env_key in field_map.items():
        value = str(cauth.get(field) or "")
        if value:
            env[env_key] = value


def cmd_describe(args: argparse.Namespace) -> int:
    service_dir = _service_dir(args.service_dir)
    try:
        manifest = _load_manifest(service_dir)
    except ValueError as exc:
        return _error(str(exc))

    runtime = manifest.get("runtime") or {}
    requested = _requested_permissions(manifest)
    runner = _runner_config(manifest)

    print(f"service:         {manifest.get('name', '(unknown)')}")
    print(f"version:         {manifest.get('version', '(unknown)')}")
    print(f"description:     {manifest.get('description', '')}")
    print(f"runtime abi:     {runtime.get('abi', '')}")
    print(f"hot-swap group:  {runtime.get('hotswap_group', '')}")
    print(f"payload rev:     {runtime.get('payload_revision', '')}")
    print(f"rootless:        {runtime.get('mode', 'rootless') == 'rootless'}")
    if _target_env(manifest):
        print(f"target env:      {_target_env(manifest)}")
    print(f"requested perms: {_format_permissions(requested)}")
    cauth = _security_cauth(manifest)
    if cauth:
        print(
            "cauth:           "
            f"enabled={cauth.get('enabled', True)} "
            f"mode={cauth.get('mode', '')} "
            f"audience={cauth.get('audience', '')}"
        )
    for line in _runner_summary_lines(runner):
        print(line)
    print("functions:")

    for function in _function_list(manifest):
        name = str(function.get("name") or "")
        language = _function_language(function)
        entry_path = _function_binary_path(function) or function.get("snippet_path", f"functions/{name.replace('-', '_')}.py")
        print(f"  - {name}")
        print(f"    description: {function.get('description', '')}")
        print(f"    language:    {language}")
        print(f"    requires:    {_format_permissions(_function_permissions(function))}")
        print(f"    entry:       {entry_path}")
    return 0


def cmd_permissions(args: argparse.Namespace) -> int:
    service_dir = _service_dir(args.service_dir)
    try:
        manifest = _load_manifest(service_dir)
    except ValueError as exc:
        return _error(str(exc))

    requested = _requested_permissions(manifest)
    granted = _load_grants(str(manifest.get("name") or "service"), requested)
    withheld = [permission for permission in requested if permission not in set(granted)]

    print(f"service:   {manifest.get('name', '(unknown)')}")
    print(f"requested: {_format_permissions(requested)}")
    print(f"granted:   {_format_permissions(granted)}")
    print(f"withheld:  {_format_permissions(withheld)}")
    return 0


def _update_grants(args: argparse.Namespace, revoke: bool) -> int:
    service_dir = _service_dir(args.service_dir)
    try:
        manifest = _load_manifest(service_dir)
    except ValueError as exc:
        return _error(str(exc))

    requested = _requested_permissions(manifest)
    requested_set = set(requested)
    current = _load_grants(str(manifest.get("name") or "service"), requested)
    current_set = set(current)

    for permission in args.permissions:
        if permission not in requested_set:
            return _error(
                f"permission '{permission}' is not declared by service '{manifest.get('name', '(unknown)')}'"
            )

    if revoke:
        updated = [permission for permission in current if permission not in set(args.permissions)]
    else:
        updated = current + [permission for permission in args.permissions if permission not in current_set]

    _save_grants(str(manifest.get("name") or "service"), requested, updated)
    print(f"updated grant manifest: {_format_permissions(updated)}")
    return 0


def cmd_grant(args: argparse.Namespace) -> int:
    return _update_grants(args, revoke=False)


def cmd_revoke(args: argparse.Namespace) -> int:
    return _update_grants(args, revoke=True)


def cmd_run(args: argparse.Namespace) -> int:
    service_dir = _service_dir(args.service_dir)
    try:
        manifest = _load_manifest(service_dir)
        function_name, function = _resolve_function(manifest, args.function)
    except ValueError as exc:
        return _error(str(exc))

    requested = _requested_permissions(manifest)
    granted = _load_grants(str(manifest.get("name") or "service"), requested)
    granted_set = set(granted)
    required = _function_permissions(function)
    missing = [permission for permission in required if permission not in granted_set]
    if missing:
        return _error(f"refusing to run '{function_name}'; missing permissions: {_format_permissions(missing)}")

    runtime = manifest.get("runtime") or {}
    runner = _runner_config(manifest)
    state_dir = Path.home() / ".uracil"
    language = _function_language(function)
    try:
        if language == "cpp":
            entry_path = _native_entry_path(service_dir, state_dir, manifest, function_name, function)
        else:
            entry_path = _snippet_path(service_dir, function_name, function)
    except ValueError as exc:
        return _error(str(exc))

    env = os.environ.copy()
    env["URACIL_SERVICE_NAME"] = str(manifest.get("name") or "")
    env["URACIL_SERVICE_VERSION"] = str(manifest.get("version") or "")
    env["URACIL_FUNCTION_NAME"] = function_name
    env["URACIL_RUNTIME_ABI"] = str(runtime.get("abi") or "")
    env["URACIL_HOTSWAP_GROUP"] = str(runtime.get("hotswap_group") or "")
    env["URACIL_PAYLOAD_REVISION"] = str(runtime.get("payload_revision") or "")
    env["URACIL_STATE_DIR"] = str(state_dir)
    env["URACIL_SERVICE_DIR"] = str(service_dir)
    env["URACIL_ROOTLESS"] = "true"
    env["URACIL_REQUESTED_PERMISSIONS"] = ",".join(requested)
    env["URACIL_GRANTED_PERMISSIONS"] = ",".join(granted)
    _apply_runner_env(env, runner)
    _apply_security_env(env, manifest)

    if language == "cpp":
        print(f"running '{function_name}' inside rootless Uracil host runtime [native]")
        result = subprocess.run([str(entry_path), *args.extra_args], cwd=str(service_dir), env=env)
    else:
        env["PYTHONPATH"] = (
            str(service_dir)
            if not env.get("PYTHONPATH")
            else f"{service_dir}{os.pathsep}{env['PYTHONPATH']}"
        )
        print(f"running '{function_name}' inside rootless Uracil host runtime")
        result = subprocess.run([sys.executable, str(entry_path), *args.extra_args], cwd=str(service_dir), env=env)
    return result.returncode


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="uracil-host", description="Run APS Uracil service bundles")
    sub = parser.add_subparsers(dest="command", required=True)

    p_describe = sub.add_parser("describe", help="Describe a service bundle")
    p_describe.add_argument("service_dir", help="Installed service directory")
    p_describe.set_defaults(func=cmd_describe)

    p_permissions = sub.add_parser("permissions", help="Show current permission grants")
    p_permissions.add_argument("service_dir", help="Installed service directory")
    p_permissions.set_defaults(func=cmd_permissions)

    p_grant = sub.add_parser("grant", help="Grant permissions to a service")
    p_grant.add_argument("service_dir", help="Installed service directory")
    p_grant.add_argument("permissions", nargs="+", help="Permissions to grant")
    p_grant.set_defaults(func=cmd_grant)

    p_revoke = sub.add_parser("revoke", help="Revoke permissions from a service")
    p_revoke.add_argument("service_dir", help="Installed service directory")
    p_revoke.add_argument("permissions", nargs="+", help="Permissions to revoke")
    p_revoke.set_defaults(func=cmd_revoke)

    p_run = sub.add_parser("run", help="Run a service function")
    p_run.add_argument("service_dir", help="Installed service directory")
    p_run.add_argument("function", nargs="?", help="Function name (defaults to entry_function)")
    p_run.add_argument("extra_args", nargs=argparse.REMAINDER, help="Arguments passed to the function snippet")
    p_run.set_defaults(func=cmd_run)

    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    if getattr(args, "extra_args", None) and args.extra_args[0] == "--":
        args.extra_args = args.extra_args[1:]
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())