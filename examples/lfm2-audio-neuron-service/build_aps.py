#!/usr/bin/env python3
from __future__ import annotations

import argparse
import zipfile
from pathlib import Path


def build_archive(service_dir: Path, out_file: Path) -> Path:
    out_file.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(out_file, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.write(service_dir / "manifest.json", arcname="manifest.json")
        for file_path in sorted((service_dir / "functions").glob("*.py")):
            archive.write(file_path, arcname=f"functions/{file_path.name}")
    return out_file


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Build a .aps archive for the Athena LFM2 audio Neuron sidecar bundle"
    )
    parser.add_argument(
        "--output",
        default="dist/lfm2-audio-neuron-service.aps",
        help="Path to output .aps archive (default: dist/lfm2-audio-neuron-service.aps)",
    )
    args = parser.parse_args()

    service_dir = Path(__file__).resolve().parent
    output_path = (service_dir / args.output).resolve()
    archive = build_archive(service_dir, output_path)
    print(archive)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())