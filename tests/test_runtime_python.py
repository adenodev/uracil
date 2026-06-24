from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

URACIL_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(URACIL_ROOT))

from tools.ura_builder import build_ura

HOST_RUNTIME = URACIL_ROOT / "tools" / "host_runtime.py"


def _write_native_service(service_dir: Path) -> None:
    (service_dir / "functions").mkdir(parents=True, exist_ok=True)
    (service_dir / "manifest.json").write_text(json.dumps({
        "name": "native-sum-service",
        "version": "0.1.0",
        "description": "Native C++ Uracil service for runtime regression tests.",
        "runtime": {
            "name": "uracil",
            "abi": "uracil.service.v1",
            "mode": "rootless",
            "hotswap_group": "native-sum-service",
            "payload_revision": "test-001",
            "target_env": "keryx-ami",
        },
        "security": {
            "cauth": {
                "enabled": True,
                "mode": "adeno-cauth",
                "base_url": "https://adeno.ltd/cauth",
                "issuer": "https://adeno.ltd/cauth",
                "audience": "keryx",
                "token_env": "KERYX_TRANSTOKEN",
                "secret_key_env": "CAUTH_SECRET_KEY",
                "encryption_key_env": "CAUTH_ENCRYPTION_KEY",
            }
        },
        "service": {
            "entry_function": "sum",
            "functions": [
                {
                    "name": "sum",
                    "description": "Add two integers and print runtime metadata.",
                    "snippet_path": "functions/sum.cpp",
                    "language": "cpp",
                    "required_permissions": [],
                    "build": {
                        "standard": "c++20",
                        "flags": ["-O2"],
                    },
                }
            ],
        },
        "permissions": {
            "requested": [],
        },
    }, indent=2))
    (service_dir / "functions" / "sum.cpp").write_text(
        "#include <cstdlib>\n"
        "#include <iostream>\n"
        "int main(int argc, char** argv) {\n"
        "    int a = argc > 1 ? std::atoi(argv[1]) : 0;\n"
        "    int b = argc > 2 ? std::atoi(argv[2]) : 0;\n"
        "    const char* target = std::getenv(\"URACIL_TARGET_ENV\");\n"
        "    const char* cauth = std::getenv(\"URACIL_CAUTH_ENABLED\");\n"
        "    std::cout << \"sum=\" << (a + b)\n"
        "              << \" env=\" << (target ? target : \"unset\")\n"
        "              << \" cauth=\" << (cauth ? cauth : \"unset\")\n"
        "              << std::endl;\n"
        "    return 0;\n"
        "}\n"
    )


def _runtime_env() -> dict[str, str]:
    env = os.environ.copy()
    env.setdefault("KERYX_TRANSTOKEN", "test-transtoken")
    env.setdefault("CAUTH_SECRET_KEY", "test-secret")
    env.setdefault("CAUTH_ENCRYPTION_KEY", "test-encryption-key")
    return env


class TestUracilNativeRuntime:
    def test_host_runtime_executes_native_cpp_function(self, tmp_path: Path):
        service_dir = tmp_path / "service"
        _write_native_service(service_dir)

        result = subprocess.run(
            [sys.executable, str(HOST_RUNTIME), "run", str(service_dir), "sum", "--", "2", "3"],
            capture_output=True,
            text=True,
            env=_runtime_env(),
        )

        assert result.returncode == 0, result.stderr
        assert "sum=5" in result.stdout
        assert "env=keryx-ami" in result.stdout
        assert "cauth=true" in result.stdout

    def test_built_ura_binary_executes_native_cpp_function(self, tmp_path: Path):
        service_dir = tmp_path / "service"
        _write_native_service(service_dir)

        output = tmp_path / "native-sum.ura"
        build_ura(package_dir=service_dir, target="x86_64_elf", output=output)

        describe = subprocess.run(
            [str(output), "--describe"],
            capture_output=True,
            text=True,
            env=_runtime_env(),
        )
        assert describe.returncode == 0, describe.stderr
        assert "Target:   keryx-ami" in describe.stdout
        assert "CAuth:    enabled" in describe.stdout
        assert "language: cpp" in describe.stdout

        result = subprocess.run(
            [str(output), "sum", "4", "5"],
            capture_output=True,
            text=True,
            env=_runtime_env(),
        )

        assert result.returncode == 0, result.stderr
        assert "sum=9" in result.stdout
        assert "env=keryx-ami" in result.stdout
        assert "cauth=true" in result.stdout