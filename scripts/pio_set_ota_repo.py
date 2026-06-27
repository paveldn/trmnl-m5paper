import re
import subprocess
from urllib.parse import urlparse

Import("env")


def _extract_owner_repo(remote_url: str):
    url = (remote_url or "").strip()

    # Matches both HTTPS and SSH GitHub remotes, e.g.:
    # https://github.com/owner/repo(.git)
    # git@github.com:owner/repo(.git)
    m = re.search(r"github\.com[:/](?P<owner>[^/]+)/(?P<repo>[^/\s]+?)(?:\.git)?$", url)
    if not m:
        return None, None

    owner = m.group("owner")
    repo = m.group("repo")
    return owner, repo


def _remote_host(remote_url: str):
    url = (remote_url or "").strip()
    if not url:
        return ""

    # SCP-style remotes: git@host:owner/repo.git
    scp_match = re.match(r"^[^@]+@(?P<host>[^:]+):.+$", url)
    if scp_match:
        return scp_match.group("host").lower()

    # URL-style remotes: https://host/owner/repo.git or ssh://git@host/owner/repo.git
    parsed = urlparse(url)
    if parsed.hostname:
        return parsed.hostname.lower()

    return ""


def _get_origin_url(project_dir: str):
    try:
        out = subprocess.check_output(
            ["git", "config", "--get", "remote.origin.url"],
            cwd=project_dir,
            stderr=subprocess.DEVNULL,
            text=True,
        )
        return out.strip()
    except Exception:
        return ""


project_dir = env.subst("$PROJECT_DIR")
origin_url = _get_origin_url(project_dir)
owner, repo = _extract_owner_repo(origin_url)
host = _remote_host(origin_url)

if owner and repo:
    env.Append(
        BUILD_FLAGS=[
            '-DOTA_GITHUB_OWNER=\\"%s\\"' % owner,
            '-DOTA_GITHUB_REPO=\\"%s\\"' % repo,
        ]
    )
    print("[ota] Using GitHub releases source from origin: %s/%s" % (owner, repo))
else:
    if not origin_url:
        print("[ota] No git remote.origin.url found; using firmware defaults")
    elif host and host != "github.com":
        print("[ota] Non-GitHub origin '%s' detected; using firmware defaults" % host)
    else:
        print("[ota] Could not parse GitHub owner/repo from origin; using firmware defaults")
