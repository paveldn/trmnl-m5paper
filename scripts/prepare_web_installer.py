#!/usr/bin/env python3
"""Prepare the static ESP Web Tools installer for GitHub Pages."""

from __future__ import annotations

import argparse
import configparser
import json
import re
import shutil
from pathlib import Path
from typing import Any


PROJECT_ROOT = Path(__file__).resolve().parent.parent
WEB_SOURCE = PROJECT_ROOT / "web"
DEFAULT_FIRMWARE = (
    PROJECT_ROOT / ".pio" / "build" / "m5stack_paper" / "firmware.factory.bin"
)
DEFAULT_PAPERDIY_FIRMWARE = (
    PROJECT_ROOT / ".pio" / "build" / "paperdiy" / "firmware.factory.bin"
)
VERSION_PATTERN = re.compile(r"^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$")
TAG_PATTERN = re.compile(r"^v(?P<version>\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?)$")


def read_firmware_version() -> str:
    config = configparser.ConfigParser(interpolation=None)
    config.read(PROJECT_ROOT / "platformio.ini")

    version = config.get("env:m5stack_paper", "custom_app_version", fallback="").strip()
    if not VERSION_PATTERN.fullmatch(version):
        raise ValueError(
            "platformio.ini custom_app_version must look like 2.7.0 or 2.7.0-beta.1"
        )
    return version


def build_manifest(version: str, device: str = "m5paper") -> dict[str, object]:
    if device == "paperdiy":
        name, chip, asset = "TRMNL for PaperDIY", "ESP32-S3", "paperdiy.factory.bin"
    else:
        name, chip, asset = "TRMNL for M5Paper", "ESP32", "m5paper.factory.bin"
    return {
        "name": name,
        "version": version,
        "new_install_prompt_erase": False,
        "new_install_improv_wait_time": 0,
        "builds": [
            {
                "chipFamily": chip,
                "improv": False,
                "parts": [{"path": asset, "offset": 0}],
            }
        ],
    }


def load_releases(path: Path) -> list[dict[str, Any]]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, list):
        raise ValueError("GitHub releases JSON must contain an array")

    # `gh api --paginate --slurp` returns one array per API page.
    if payload and all(isinstance(page, list) for page in payload):
        return [release for page in payload for release in page]
    return payload


def prepare_release_catalog(output: Path, releases_json: Path, firmware_dir: Path) -> None:
    shutil.copytree(WEB_SOURCE, output, dirs_exist_ok=True)
    catalog: list[dict[str, object]] = []

    for release in load_releases(releases_json):
        if not isinstance(release, dict) or release.get("draft", False):
            continue

        tag = release.get("tag_name", "")
        match = TAG_PATTERN.fullmatch(tag) if isinstance(tag, str) else None
        if not match:
            continue

        source = firmware_dir / tag / "m5paper.factory.bin"
        if not source.is_file():
            continue

        version = match.group("version")
        release_dir = output / "firmware" / tag
        release_dir.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, release_dir / "m5paper.factory.bin")
        (release_dir / "manifest.json").write_text(
            json.dumps(build_manifest(version), indent=2) + "\n", encoding="utf-8"
        )
        manifests = {"m5paper": f"firmware/{tag}/manifest.json"}
        paperdiy_source = firmware_dir / tag / "paperdiy.factory.bin"
        if paperdiy_source.is_file():
            shutil.copy2(paperdiy_source, release_dir / "paperdiy.factory.bin")
            (release_dir / "paperdiy-manifest.json").write_text(
                json.dumps(build_manifest(version, "paperdiy"), indent=2) + "\n",
                encoding="utf-8",
            )
            manifests["paperdiy"] = f"firmware/{tag}/paperdiy-manifest.json"
        catalog.append(
            {
                "tag": tag,
                "version": version,
                "prerelease": bool(release.get("prerelease", False)),
                "published_at": release.get("published_at"),
                "manifest": f"firmware/{tag}/manifest.json",
                "manifests": manifests,
            }
        )

    stable = next((release for release in catalog if not release["prerelease"]), None)
    if stable is None:
        raise ValueError("No stable release with m5paper.factory.bin was found")

    # Keep root files for compatibility and as the no-JavaScript default.
    stable_dir = output / "firmware" / str(stable["tag"])
    shutil.copy2(stable_dir / "m5paper.factory.bin", output / "m5paper.factory.bin")
    shutil.copy2(stable_dir / "manifest.json", output / "manifest.json")
    (output / "releases.json").write_text(
        json.dumps({"default": stable["tag"], "releases": catalog}, indent=2) + "\n",
        encoding="utf-8",
    )
    (output / ".nojekyll").touch()


def prepare(
    output: Path,
    firmware: Path,
    paperdiy_firmware: Path | None = None,
    version: str | None = None,
) -> None:
    if not firmware.is_file():
        raise FileNotFoundError(
            f"Factory image not found at {firmware}. Run `pio run` first."
        )

    shutil.copytree(WEB_SOURCE, output, dirs_exist_ok=True)

    shutil.copy2(firmware, output / "m5paper.factory.bin")
    firmware_version = version or read_firmware_version()
    if not VERSION_PATTERN.fullmatch(firmware_version):
        raise ValueError("version must look like 2.7.1 or 2.7.1-beta.1")
    manifest = build_manifest(firmware_version)
    (output / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    if paperdiy_firmware and paperdiy_firmware.is_file():
        shutil.copy2(paperdiy_firmware, output / "paperdiy.factory.bin")
        (output / "paperdiy-manifest.json").write_text(
            json.dumps(build_manifest(firmware_version, "paperdiy"), indent=2) + "\n",
            encoding="utf-8",
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
    parser.add_argument(
        "--version",
        help="Firmware version for the manifest (defaults to platformio.ini)",
    )
    parser.add_argument(
        "--paperdiy-firmware",
        type=Path,
        default=DEFAULT_PAPERDIY_FIRMWARE,
        help="Path to the PaperDIY PlatformIO factory image",
    )
    parser.add_argument(
        "--releases-json",
        type=Path,
        help="GitHub releases API JSON used to build a multi-version catalog",
    )
    parser.add_argument(
        "--firmware-dir",
        type=Path,
        help="Directory containing <tag>/m5paper.factory.bin for the catalog",
    )
    return parser.parse_args()


if __name__ == "__main__":
    arguments = parse_args()
    if arguments.releases_json:
        if not arguments.firmware_dir:
            raise ValueError("--firmware-dir is required with --releases-json")
        prepare_release_catalog(
            arguments.output.resolve(),
            arguments.releases_json.resolve(),
            arguments.firmware_dir.resolve(),
        )
    else:
        prepare(
            arguments.output.resolve(),
            arguments.firmware.resolve(),
            arguments.paperdiy_firmware.resolve(),
            arguments.version,
        )
