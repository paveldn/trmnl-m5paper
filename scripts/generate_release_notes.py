#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import re
import subprocess
from pathlib import Path


def run_git(args: list[str]) -> str:
    result = subprocess.run(
        ["git", *args],
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.strip()


def try_run_git(args: list[str]) -> str:
    result = subprocess.run(
        ["git", *args],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return ""
    return result.stdout.strip()


def ref_exists(ref: str) -> bool:
    result = subprocess.run(
        ["git", "rev-parse", "--verify", "-q", f"{ref}^{{commit}}"],
        capture_output=True,
        text=True,
    )
    return result.returncode == 0


def detect_repo_url() -> str:
    gh_server = (os.getenv("GITHUB_SERVER_URL") or "").strip()
    gh_repo = (os.getenv("GITHUB_REPOSITORY") or "").strip()
    if gh_server and gh_repo:
        return f"{gh_server.rstrip('/')}/{gh_repo}"

    origin = try_run_git(["config", "--get", "remote.origin.url"])
    if not origin:
        return ""

    origin = origin.strip()

    m = re.search(r"github\.com[:/](?P<owner>[^/]+)/(?P<repo>[^/\s]+?)(?:\.git)?$", origin)
    if m:
        owner = m.group("owner")
        repo = m.group("repo")
        return f"https://github.com/{owner}/{repo}"

    if origin.startswith("http://") or origin.startswith("https://"):
        return origin[:-4] if origin.endswith(".git") else origin

    return ""


def changelog_line(repo_url: str, previous_tag: str, current_tag: str) -> str:
    if previous_tag:
        label = f"{previous_tag}...{current_tag}"
        if repo_url:
            return f"Full Changelog: [{label}]({repo_url}/compare/{label})"
        return f"Full Changelog: {label}"

    if repo_url:
        return f"Full Changelog: [{current_tag}]({repo_url}/releases/tag/{current_tag})"
    return f"Full Changelog: {current_tag}"


def collect_commits(previous_tag: str, current_tag: str) -> list[str]:
    upper_ref = current_tag if ref_exists(current_tag) else "HEAD"

    if previous_tag:
        range_spec = f"{previous_tag}..{upper_ref}"
    else:
        range_spec = upper_ref

    result = subprocess.run(
        [
            "git",
            "log",
            "--no-merges",
            "--pretty=format:%s",
            "--reverse",
            range_spec,
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    return [line.strip() for line in result.stdout.splitlines() if line.strip()]


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate markdown release notes from git history.")
    parser.add_argument("--current-tag", required=True)
    parser.add_argument("--previous-tag", default="")
    parser.add_argument("--output", required=True)
    parser.add_argument("--title", default="Release Notes")
    args = parser.parse_args()

    repo_url = detect_repo_url()
    commits = collect_commits(args.previous_tag, args.current_tag)

    lines = [f"# {args.title}", ""]
    if args.previous_tag:
        lines.append(f"Changes since {args.previous_tag}")
    else:
        lines.append(f"Changes for {args.current_tag}")
    lines.append("")

    if commits:
        for subject in commits:
            lines.append(f"- {subject}")
    else:
        lines.append("- No commit messages found in this range.")

    lines.append("")
    lines.append(changelog_line(repo_url, args.previous_tag, args.current_tag))

    Path(args.output).write_text("\n".join(lines) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
