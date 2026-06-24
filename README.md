# Uracil

A hardware-agnostic, rootless microkernel runtime for native services.

Uracil provides a minimal, capability-scoped execution environment for service bundles. Each bundle declares the permissions it needs; users grant only what they approve. The runtime refuses to execute any function whose requirements haven't been satisfied.

```
                        ┌──────────────────────────────────────┐
                        │           uracil runtime             │
                        ├──────────────┬───────────────────────┤
                        │  permission  │     bundle loader     │
                        │    engine    │  (JSON manifest → C)  │
                        ├──────────────┼───────────────────────┤
                        │  grant store │  subprocess executor  │
                        │ ~/.uracil/   │  (fork+exec python3)  │
                        ├──────────────┴───────────────────────┤
                        │       host platform adapter          │
                        │         (arch/host/)                 │
                        └──────────────────────────────────────┘
```

## Features

- **Permission model** — services declare capabilities; users grant them explicitly
- **Bundle manifest** — JSON contract describing service identity, functions, and permissions
- **External bundle loader** — load any APS-installed service directory at runtime
- **Subprocess executor** — run function snippets as isolated child processes
- **Built-in JSON parser** — zero-dependency manifest loading
- **Hot-swap metadata** — versioned payload groups for safe service updates
- **Runner metadata passthrough** — `.ura` bundles can carry GGUF/API runner metadata for Athena-managed model services
- **Campaign kernel** — persistent campaign-scoped state, artifact ownership, and cleanup under `~/.uracil/campaigns/`
- **Host-first** — works on Linux x86_64 now, designed for additional ISAs later
- **C11, zero external dependencies** — only libc and POSIX

## Repository Layout

```
uracil/
├── CMakeLists.txt
├── include/uracil/
│   ├── bundle.h          Service bundle descriptor
│   ├── campaign.h        Campaign-kernel lifecycle API
│   ├── common.h          Status codes, utility macros
│   ├── json.h            JSON parser API
│   ├── loader.h          External bundle loader API
│   ├── permissions.h     Permission model API
│   ├── platform.h        Platform abstraction layer
│   ├── process.h         Subprocess function executor
│   └── runtime.h         Runtime context and operations
├── kernel/
│   ├── bundle.c          Function lookup
│   ├── campaign.c        Campaign state, artifacts, cleanup
│   ├── json.c            Recursive-descent JSON parser
│   ├── loader.c          Manifest → ur_service_bundle
│   ├── permissions.c     Grant store (load / save / format)
│   ├── process.c         fork+exec function snippets
│   └── runtime.c         Print, grant, revoke, run
├── arch/host/
│   └── platform.c        Linux/POSIX platform adapter
├── app/
│   ├── main.c            CLI entry point
│   ├── demo_bundle.c     Compiled-in demo service
│   └── demo_bundle.h
├── tests/
│   ├── test_campaign.c   Campaign kernel tests
│   ├── test_json.c       JSON parser tests
│   ├── test_loader.c     Bundle loader tests
│   └── test_permissions.c Permission model tests
├── tools/
│   └── host_runtime.py   Python host runner for APS packages
└── examples/
    └── aps-service.manifest.json
```

## Build

Requires CMake 3.20+ and a C11 compiler (GCC or Clang).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Run the test suite:

```bash
ctest --test-dir build --output-on-failure
```

Enable strict warnings:

```bash
cmake -S . -B build -DURACIL_WARNINGS_AS_ERRORS=ON
cmake --build build
```

## Usage

### Built-in demo bundle

The binary ships with a compiled-in sample service so the runtime is usable immediately:

```bash
$ ./build/uracil describe
service:         aps.sample.echo-service
version:         0.1.0
description:     Sample APS-native service bundle running inside Uracil.
runtime abi:     uracil.service.v1
hot-swap group:  aps.sample.echo-service
payload rev:     demo-001
rootless:        true
requested perms: fs.read, net.client, clock.read
functions:
  - health
    description: Report runtime health and service metadata.
    requires:    (none)
  - inspect-fs
    description: List the current working directory through the filesystem adapter.
    requires:    fs.read
  - net-probe
    description: Demonstrate an outbound network client capability.
    requires:    net.client
  - clock-now
    description: Read the virtual clock exposed by the host adapter.
    requires:    clock.read

$ ./build/uracil run health
service=aps.sample.echo-service status=ready rootless=true abi=uracil.service.v1

$ ./build/uracil grant fs.read clock.read
updated grant manifest: fs.read, clock.read

$ ./build/uracil run clock-now
virtual clock reading: 2026-04-17 12:00:00 UTC
```

### External bundles (APS packages)

Load any service directory that contains a `manifest.json`:

```bash
# Describe an APS-installed service
./build/uracil --bundle ~/.aps/packages/weather-check-service describe

# Grant permissions
./build/uracil --bundle ~/.aps/packages/weather-check-service grant net.client

# Run a function (executes functions/current_weather.py as a subprocess)
./build/uracil --bundle ~/.aps/packages/weather-check-service run current-weather London

# Check permission state
./build/uracil --bundle ~/.aps/packages/weather-check-service permissions
```

The `--bundle` flag switches from the compiled demo to an external service. The runtime:

1. Reads `manifest.json` from the directory
2. Validates the bundle contract (name, version, ABI, functions)
3. Applies the same permission model as compiled bundles
4. Executes function snippets via `python3 functions/<name>.py`
5. Sets `URACIL_*` environment variables for the subprocess

### Campaign kernel

Uracil now exposes a persistent campaign-kernel surface for multi-step coordinators and agent harnesses. Each campaign gets its own runtime root under `~/.uracil/campaigns/<campaign-id>/`, including an `artifacts/` directory for owned binaries and payloads that should be cleaned up when the campaign finishes.

```bash
# Bootstrap a campaign runtime root
./build/uracil campaign init demo-campaign keryx keryx-ami adeno-cauth

# Attach a generated artifact for later cleanup
./build/uracil campaign attach-artifact demo-campaign artifacts/writer.ura

# Transition to running
./build/uracil campaign start demo-campaign

# Complete the campaign and clean registered artifacts
./build/uracil campaign complete demo-campaign

# Remove the campaign kernel state entirely
./build/uracil campaign destroy demo-campaign
```

The public API in `include/uracil/campaign.h` gives other services the same lifecycle primitives directly in C: boot/load/store, phase transitions, artifact registration, artifact cleanup, and full campaign teardown.

### Version

```bash
$ ./build/uracil --version
uracil 0.2.0
```

## Permission Model

Uracil separates **declared capabilities** from **user-granted permissions**.

A service bundle's manifest lists every permission the service might need:

```json
{
  "permissions": {
    "requested": ["fs.read", "net.client", "clock.read"]
  }
}
```

Each function declares which subset it actually requires:

```json
{
  "name": "current-weather",
  "description": "Fetch current weather for a city",
  "required_permissions": ["net.client"]
}
```

The runtime will only execute a function if **every** permission in its `required_permissions` has been granted by the user. Grants are stored locally:

```
~/.uracil/grants/<service-name>.perm
```

### Available permissions

| Permission     | Capability                        |
|----------------|-----------------------------------|
| `fs.read`      | Read files from the host          |
| `fs.write`     | Write files to the host           |
| `net.client`   | Make outbound network connections |
| `net.server`   | Listen for inbound connections    |
| `proc.spawn`   | Spawn child processes             |
| `clock.read`   | Read the system clock             |

### Grant lifecycle

```bash
# View what's requested vs. granted
uracil --bundle <dir> permissions

# Grant specific permissions
uracil --bundle <dir> grant net.client clock.read

# Revoke a permission
uracil --bundle <dir> revoke net.client

# The runtime refuses to run functions with unmet requirements
uracil --bundle <dir> run current-weather   # → permission-denied
```

## Bundle Manifest

Every service bundle carries a `manifest.json` at its root:

```json
{
  "name": "aps.sample.echo-service",
  "version": "0.1.0",
  "description": "Sample APS-native service package that targets Uracil.",
  "runtime": {
    "name": "uracil",
    "abi": "uracil.service.v1",
    "mode": "rootless",
    "hotswap_group": "aps.sample.echo-service",
    "payload_revision": "demo-001",
    "target_env": "keryx-ami"
  },
  "security": {
    "cauth": {
      "enabled": true,
      "mode": "adeno-cauth",
      "base_url": "https://adeno.ltd/cauth",
      "issuer": "https://adeno.ltd/cauth",
      "audience": "keryx",
      "token_env": "KERYX_TRANSTOKEN",
      "secret_key_env": "CAUTH_SECRET_KEY",
      "encryption_key_env": "CAUTH_ENCRYPTION_KEY"
    }
  },
  "service": {
    "entry_function": "health",
    "functions": [
      {
        "name": "health",
        "description": "Report bundle health.",
        "snippet_path": "functions/health.py",
        "required_permissions": []
      },
      {
        "name": "inspect-fs-native",
        "description": "List the current working directory with a native C++ helper.",
        "snippet_path": "functions/inspect_fs.cpp",
        "language": "cpp",
        "build": {
          "standard": "c++20",
          "flags": ["-O3"]
        },
        "required_permissions": ["fs.read"]
      }
    ]
  },
  "permissions": {
    "requested": ["fs.read", "net.client", "clock.read"]
  }
}
```

### Manifest fields

| Field | Required | Description |
|-------|----------|-------------|
| `name` | Yes | Globally unique service identifier |
| `version` | Yes | Semantic version |
| `description` | No | Human-readable summary |
| `runtime.name` | No | Runtime target (`uracil`) |
| `runtime.abi` | No | ABI version (`uracil.service.v1`) |
| `runtime.mode` | No | Execution mode (`rootless`) |
| `runtime.hotswap_group` | No | Group ID for safe payload updates |
| `runtime.payload_revision` | No | Current payload version |
| `runtime.target_env` | No | Deployment profile such as `keryx-ami` |
| `security.cauth` | No | CAuth runtime metadata projected as `URACIL_CAUTH_*` |
| `service.entry_function` | No | Default function to run |
| `service.functions[]` | No | Array of function descriptors |
| `service.functions[].language` | No | `python` (default) or `cpp` for native functions |
| `service.functions[].build` | No | Optional native compile options for host-runtime and builder |
| `service.functions[].binary_path` | No | Prebuilt native binary path used by compiled `.ura` bundles |
| `permissions.requested` | No | Union of all permissions the service needs |

## Subprocess Execution

When running an external bundle, Uracil executes function payloads as child processes:

```
uracil --bundle <dir> run <function> [args...]
       │
       ├── fork()
       │
       └── child:
           chdir(<service_directory>)
           setenv(URACIL_SERVICE_NAME, ...)
           setenv(URACIL_GRANTED_PERMISSIONS, ...)
           setenv(...)
       exec("python3", "functions/<function>.py", args...)   # Python
       exec("compiled/<function>", args...)                    # Native C++
```

### Environment variables

| Variable | Description |
|----------|-------------|
| `URACIL_SERVICE_NAME` | Service identifier from manifest |
| `URACIL_SERVICE_VERSION` | Version string |
| `URACIL_FUNCTION_NAME` | Name of the function being executed |
| `URACIL_RUNTIME_ABI` | ABI version |
| `URACIL_HOTSWAP_GROUP` | Hot-swap group identifier |
| `URACIL_PAYLOAD_REVISION` | Current payload revision |
| `URACIL_TARGET_ENV` | Deployment profile such as `keryx-ami` |
| `URACIL_KERYX_AMI` | `true` when targeting the Keryx AMI runtime |
| `URACIL_SERVICE_DIR` | Absolute path to the service directory |
| `URACIL_STATE_DIR` | Path to `~/.uracil` |
| `URACIL_ROOTLESS` | Always `true` in rootless mode |
| `URACIL_REQUESTED_PERMISSIONS` | Comma-separated list |
| `URACIL_GRANTED_PERMISSIONS` | Comma-separated list |
| `URACIL_CAUTH_*` | Adeno CAuth runtime contract for token and envelope metadata |

## APS Integration

Uracil is designed as a runtime target for the [APS](https://aps.adeno.ltd) (Adeno Package Service) package ecosystem.

```
  ┌─────────────┐    codegen     ┌────────────────┐
  │    Keryx     │──────────────→│  .aps archive   │
  │  (Devstral)  │               │  manifest.json  │
  └─────────────┘               │  functions/*.py │
                                 └───────┬────────┘
                                         │ aps install service
                                         ▼
                                 ┌────────────────┐
                                 │  APS registry   │
                                 │  ~/.aps/pkgs/   │
                                 └───────┬────────┘
                                         │ uracil --bundle
                                         ▼
                                 ┌────────────────┐
                                 │    Uracil       │
                                 │  permission ──→ │ grant/deny
                                 │  subprocess ──→ │ execute
                                 └────────────────┘
```

### Typical workflow

```bash
# 1. Keryx generates a Uracil service package
#    (or build one manually)

# 2. Install via APS
aps install service weather-check-service

# 2b. Audit the package locally without exposing secrets
aps audit weather-check-service
aps audit weather-check-service --shell

# 3. Inspect the bundle
uracil --bundle ~/.aps/packages/weather-check-service describe

# 4. Grant required permissions
uracil --bundle ~/.aps/packages/weather-check-service grant net.client

# 5. Run a function
uracil --bundle ~/.aps/packages/weather-check-service run current-weather London
```

### GGUF service runners

Athena packages model-backed services as APS bundles first, then compiles them
into `.ura` binaries. For GGUF-backed services, include runner metadata in the
manifest so the URA artifact carries the model contract alongside the function
definitions:

```json
{
  "name": "mistral-chat-service",
  "version": "0.3.0",
  "runtime": {
    "name": "uracil",
    "abi": "uracil.service.v1",
    "mode": "rootless"
  },
  "service": {
    "entry_function": "chat",
    "functions": [
      {
        "name": "chat",
        "snippet_path": "functions/chat.py",
        "required_permissions": ["net.server"]
      }
    ]
  },
  "permissions": {
    "requested": ["net.server"]
  },
  "runner": {
    "kind": "gguf-http",
    "protocol": "http",
    "bind": "127.0.0.1:8088",
    "model": {
      "format": "gguf",
      "uri": "corpus://athena/models/mistral-7b-instruct.gguf"
    }
  }
}
```

`tools/ura_builder.py` now preserves this runner metadata into `.ura.meta` via
`runner`, `runner-protocol`, `runner-bind`, `runner-model-format`,
`runner-model-ref`, `runner-target`, `runner-accelerator`,
`runner-source-format`, `runner-runtime-format`, and `runner-config` lines.
Both Uracil execution paths consume that contract now:

- the APS-installed host runtime exposes the runner block to snippets through
  `URACIL_RUNNER_*` environment variables
- compiled `.ura` binaries parse the same metadata and expose the same
  `URACIL_RUNNER_*` variables inside the virtualized stub process

That is the control surface for GGUF-on-Neuron packaging. A bundle can keep the
source model in GGUF while declaring the virtualized runtime target separately:

```json
{
  "runner": {
    "kind": "gguf-neuron",
    "protocol": "http",
    "bind": "127.0.0.1:8091",
    "model": {
      "format": "gguf",
      "uri": "corpus://athena/models/mistral-7b-instruct.gguf"
    },
    "virtualization": {
      "target": "aws-neuron-inf2",
      "accelerator": "neuron",
      "source_format": "gguf",
      "runtime_format": "neff"
    }
  }
}
```

Athena can then register the model as `runner=gguf-neuron` while keeping the
original Corpus reference in GGUF form.

For a concrete audio-oriented bundle, see `uracil/examples/lfm2-audio-neuron-service`,
which forwards interleaved wav requests to the Athena Neuron sidecar via the
same `URACIL_RUNNER_*` contract.

## Architecture

### Two-tier design

Uracil implements the same runtime contract at two levels:

1. **C core** (`kernel/` + `arch/host/`) — compiled binary with embedded demo bundle. Demonstrates the permission engine, grant store, and function dispatch in native code.

2. **External loader** (`kernel/loader.c` + `kernel/process.c`) — loads service bundles from JSON manifests at runtime and executes function snippets as subprocesses. This is the path APS-installed packages take.

Both tiers share:
- The same permission model (requested → granted → enforced)
- The same grant store (`~/.uracil/grants/`)
- The same bundle descriptor structure
- The same CLI interface

### Design principles

- **Capability-scoped** — no ambient authority. Every function must declare what it needs.
- **Rootless** — no elevated privileges. Services run as the current user.
- **Host-first** — establish the contract on Linux, then port to other targets.
- **Zero dependencies** — the C runtime needs only libc and POSIX.
- **Hot-swap ready** — payload revisions within a group can be updated safely.

## Next Steps

- Add capability-scoped filesystem and network adapters
- Add non-host targets under `arch/<target>/`
- Add native payload loading (dlopen or embedded WASM)
- Add service isolation via namespaces or seccomp

## License

See LICENSE file.
