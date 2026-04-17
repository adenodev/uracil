#!/usr/bin/env python3
"""Host-side Uracil runtime for APS-installed dynamic service bundles."""

from __future__ import annotations

import argparse
import json
import os
import re
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

    candidate = (service_dir / relative).resolve()
    candidate.relative_to(service_dir)
    if not candidate.exists():
        raise ValueError(f"snippet file not found: {relative}")
    return candidate


def cmd_describe(args: argparse.Namespace) -> int:
    service_dir = _service_dir(args.service_dir)
    try:
        manifest = _load_manifest(service_dir)
    except ValueError as exc:
        return _error(str(exc))

    runtime = manifest.get("runtime") or {}
    requested = _requested_permissions(manifest)

    print(f"service:         {manifest.get('name', '(unknown)')}")
    print(f"version:         {manifest.get('version', '(unknown)')}")
    print(f"description:     {manifest.get('description', '')}")
    print(f"runtime abi:     {runtime.get('abi', '')}")
    print(f"hot-swap group:  {runtime.get('hotswap_group', '')}")
    print(f"payload rev:     {runtime.get('payload_revision', '')}")
    print(f"rootless:        {runtime.get('mode', 'rootless') == 'rootless'}")
    print(f"requested perms: {_format_permissions(requested)}")
    print("functions:")

    for function in _function_list(manifest):
        name = str(function.get("name") or "")
        print(f"  - {name}")
        print(f"    description: {function.get('description', '')}")
        print(f"    requires:    {_format_permissions(_function_permissions(function))}")
        print(f"    snippet:     {function.get('snippet_path', f'functions/{name.replace('-', '_')}.py')}")
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
        snippet_path = _snippet_path(service_dir, function_name, function)
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
    state_dir = Path.home() / ".uracil"
    env = os.environ.copy()
    env["PYTHONPATH"] = (
        str(service_dir)
        if not env.get("PYTHONPATH")
        else f"{service_dir}{os.pathsep}{env['PYTHONPATH']}"
    )
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

    print(f"running '{function_name}' inside rootless Uracil host runtime")
    result = subprocess.run([sys.executable, str(snippet_path), *args.extra_args], cwd=str(service_dir), env=env)
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