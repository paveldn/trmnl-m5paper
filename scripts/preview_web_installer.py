#!/usr/bin/env python3
"""Build and serve the release-based browser flasher locally."""

from __future__ import annotations

import argparse
import json
import shutil
import tempfile
import urllib.request
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from prepare_web_installer import TAG_PATTERN, prepare_release_catalog


DEFAULT_REPOSITORY = "paveldn/trmnl-m5paper"
ASSET_NAME = "m5paper.factory.bin"


def request(url: str) -> urllib.request.Request:
    return urllib.request.Request(
        url,
        headers={
            "Accept": "application/vnd.github+json",
            "User-Agent": "trmnl-m5paper-local-preview",
            "X-GitHub-Api-Version": "2022-11-28",
        },
    )


def fetch_releases(repository: str) -> list[dict[str, object]]:
    url = f"https://api.github.com/repos/{repository}/releases?per_page=100"
    with urllib.request.urlopen(request(url)) as response:
        payload = json.load(response)
    if not isinstance(payload, list):
        raise ValueError("GitHub releases API did not return an array")
    return payload


def download_release_assets(
    releases: list[dict[str, object]], destination: Path
) -> int:
    downloaded = 0
    for release in releases:
        if release.get("draft", False):
            continue

        tag = release.get("tag_name")
        if not isinstance(tag, str) or not TAG_PATTERN.fullmatch(tag):
            continue

        assets = release.get("assets", [])
        if not isinstance(assets, list):
            continue
        asset = next(
            (
                candidate
                for candidate in assets
                if isinstance(candidate, dict) and candidate.get("name") == ASSET_NAME
            ),
            None,
        )
        if asset is None:
            continue

        download_url = asset.get("browser_download_url")
        if not isinstance(download_url, str):
            continue

        target_dir = destination / tag
        target_dir.mkdir(parents=True, exist_ok=True)
        target = target_dir / ASSET_NAME
        print(f"Downloading {tag}...")
        with urllib.request.urlopen(request(download_url)) as response, target.open("wb") as output:
            shutil.copyfileobj(response, output)
        downloaded += 1
    return downloaded


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", default=DEFAULT_REPOSITORY)
    parser.add_argument(
        "--output",
        type=Path,
        default=Path(__file__).resolve().parent.parent / "dist" / "web-installer-local",
    )
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--build-only", action="store_true")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    releases = fetch_releases(args.repository)

    with tempfile.TemporaryDirectory(prefix="m5paper-releases-") as temp:
        temp_dir = Path(temp)
        firmware_dir = temp_dir / "firmware"
        count = download_release_assets(releases, firmware_dir)
        if count == 0:
            raise RuntimeError(f"No releases contain {ASSET_NAME}")

        releases_json = temp_dir / "releases.json"
        releases_json.write_text(json.dumps(releases), encoding="utf-8")
        output = args.output.resolve()
        prepare_release_catalog(output, releases_json, firmware_dir)

    print(f"Prepared {count} firmware versions in {output}")
    if args.build_only:
        return

    handler = partial(SimpleHTTPRequestHandler, directory=str(output))
    server = ThreadingHTTPServer((args.host, args.port), handler)
    print(f"Open http://{args.host}:{args.port} (press Ctrl+C to stop)")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nPreview stopped")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
