# Basic APS Echo Service

This folder contains a minimal Uracil service package for APS.

## Build archive

```bash
cd uracil/examples/basic-echo-service
python3 build_aps.py
```

The command outputs a local archive at `dist/basic-echo-service.aps`.

## Install with APS CLI

```bash
aps install service uracil/examples/basic-echo-service/dist/basic-echo-service.aps
aps info demo.basic.echo-service
```

## Run functions

```bash
aps run demo.basic.echo-service
aps run demo.basic.echo-service --function echo hello from aps
```
