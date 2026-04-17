# Uracil

Uracil is a hardware-agnostic microkernel runtime scaffold for native services.

This repository is intentionally host-first. It gives you a working runtime contract, permission model, CLI, and service-bundle shape now, without locking the project to a specific ISA or board.

## Current Scope

- Rootless service runtime contract
- Bundle manifest model with hot-swap metadata
- Per-function permission requirements
- Local permission grant store
- Native `uracil` demo CLI
- Host-side APS service runner for dynamic package bundles
- Host platform adapter under `arch/host/`

## Repository Layout

```text
uracil/
├── CMakeLists.txt
├── README.md
├── app/
│   ├── demo_bundle.c
│   ├── demo_bundle.h
│   └── main.c
├── arch/host/
│   └── platform.c
├── examples/
│   └── aps-service.manifest.json
├── include/uracil/
│   ├── bundle.h
│   ├── common.h
│   ├── permissions.h
│   ├── platform.h
│   └── runtime.h
└── kernel/
    ├── bundle.c
    ├── permissions.c
    └── runtime.c
```

## Build

```bash
cd uracil
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

## CLI

The demo binary ships with an embedded sample service bundle so the repo is usable immediately.

```bash
./build/uracil describe
./build/uracil permissions
./build/uracil --grant fs.read clock.read
./build/uracil run inspect-fs
./build/uracil run clock-now
./build/uracil --revoke fs.read
```

For APS-installed dynamic service bundles, use the host runner:

```bash
python3 tools/host_runtime.py describe ~/.aps/packages/aps.sample.echo-service
python3 tools/host_runtime.py permissions ~/.aps/packages/aps.sample.echo-service
python3 tools/host_runtime.py grant ~/.aps/packages/aps.sample.echo-service fs.read
python3 tools/host_runtime.py run ~/.aps/packages/aps.sample.echo-service health
```

## Permission Model

Uracil separates two things:

- The bundle's requested capability manifest, compiled into the binary
- The user-granted permission overlay, stored locally

That avoids self-modifying binaries while still giving the runtime an internal permission manifest concept. Grants are stored under:

```text
~/.uracil/grants/<service>.perm
```

Each function declares the permissions it needs. Uracil refuses to run a function if those permissions have not been granted.

## Bundle Model

Each service bundle carries:

- `service_name`
- `service_version`
- `runtime_abi`
- `hotswap_group`
- `payload_revision`
- `requested_permissions`
- `functions[]`

That lets APS install a bundle, inspect its enclosed functions, show requested permissions, and later swap payload revisions within a stable hot-swap group.

See [examples/aps-service.manifest.json](/mnt/E/Projects/AI-ML/uracil/examples/aps-service.manifest.json) for the APS-facing manifest shape this repo is designed around.

## APS Direction

The intended APS flow is:

1. APS downloads a service package that targets the `uracil.service.v1` ABI.
2. APS reads the package manifest and shows enclosed functions plus requested permissions.
3. APS installs the package as a Uracil service instead of expanding it into a Python venv.
4. The user grants only the permissions that service should receive.5. Later payload revisions can be hot-swapped as long as they stay within the same `hotswap_group` and ABI.

Until the C runtime grows a real external payload loader, APS can execute service bundles through `tools/host_runtime.py`. That host runner enforces the same manifest, function, and grant model for dynamic packages while the compiled `uracil` demo binary remains the low-level reference scaffold.

## Next Steps

The scaffold is ready for the next layer of work:

- Add a real payload loader instead of the compiled demo bundle
- Add capability-scoped filesystem and network adapters
- Add an APS installer path that targets `runtime.name = "uracil"`
- Add non-host targets under `arch/<target>/`
