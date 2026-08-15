#!/usr/bin/env python3
"""Prepare the static ESP Web Tools installer for GitHub Pages."""

from __future__ import annotations

import argparse
import configparser
import json
import re
import shutil
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parent.parent
WEB_SOURCE = PROJECT_ROOT / "web"
DEFAULT_FIRMWARE = (
    PROJECT_ROOT / ".pio" / "build" / "m5stack_paper" / "firmware.factory.bin"
)
VERSION_PATTERN = re.compile(r"^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$")


def read_firmware_version() -> str:
    config = configparser.ConfigParser(interpolation=None)
    config.read(PROJECT_ROOT / "platformio.ini")

    version = config.get("env:m5stack_paper", "custom_app_version", fallback="").strip()
    if not VERSION_PATTERN.fullmatch(version):
        raise ValueError(
            "platformio.ini custom_app_version must look like 2.7.0 or 2.7.0-beta.1"
        )
    return version


def build_manifest(version: str) -> dict[str, object]:
    return {
        "name": "TRMNL for M5Paper",
        "version": version,
        "new_install_prompt_erase": False,
        "new_install_improv_wait_time": 0,
        "builds": [
            {
                "chipFamily": "ESP32",
                "improv": False,
                "parts": [{"path": "m5paper.factory.bin", "offset": 0}],
            }
        ],
    }


def prepare(output: Path, firmware: Path) -> None:
    if not firmware.is_file():
        raise FileNotFoundError(
            f"Factory image not found at {firmware}. Run `pio run` first."
        )

    output.mkdir(parents=True, exist_ok=True)
    for asset in ("index.html", "installer.js", "styles.css"):
        shutil.copy2(WEB_SOURCE / asset, output / asset)

    shutil.copy2(firmware, output / "m5paper.factory.bin")
    manifest = build_manifest(read_firmware_version())
    (output / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    (output / ".nojekyll").touch()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output",
        type=Path,
        default=PROJECT_ROOT / "dist" / "web-installer",
        help="Directory to populate (default: dist/web-installer)",
    )
    parser.add_argument(
        "--firmware",
        type=Path,
        default=DEFAULT_FIRMWARE,
        help="Path to the PlatformIO factory image",
    )
    return parser.parse_args()


if __name__ == "__main__":
    arguments = parse_args()
    prepare(arguments.output.resolve(), arguments.firmware.resolve())
