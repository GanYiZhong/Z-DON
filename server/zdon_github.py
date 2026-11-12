"""The GitHub rolling release: every `publish` of the windows channel puts the same install zip the website hands out
(update/windows/package.zip) on one GitHub release, whose tag is moved to the commit the build came from. Uses the
GitHub CLI (`gh`, logged in to an account that can write the repository); set [github] release_repo to turn it on.
With [github] workflow set (zdon-release.yml), it then starts that workflow, which builds Linux, macOS and Android
on GitHub and adds them to the same release.
"""
from __future__ import annotations

import shutil
import subprocess
import tempfile
from pathlib import Path


def _gh(*args: str, check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(["gh", *args], capture_output=True, text=True, encoding="utf-8", check=check)


def _git(repo_dir: Path, *args: str) -> str:
    return subprocess.run(["git", "-C", str(repo_dir), *args], capture_output=True, text=True,
                          check=True).stdout.strip()


def _on_github(repo: str, sha: str) -> bool:
    return _gh("api", f"repos/{repo}/commits/{sha}", "--jq", ".sha", check=False).returncode == 0


def notes(version: str, sha: str, pushed: bool) -> str:
    built = f"`{sha[:8]}`" if pushed else f"`{sha[:8]}` (not on GitHub yet; the tag points at the latest pushed main)"
    return (f"The latest Z-DON build for Windows. This release is replaced every time a new build is published; "
            f"it is the same zip as the download on https://zdon.taiko.ac, and the game updates itself from there "
            f"(F9 on the title screen).\n\n"
            f"最新的 Z-DON Windows 版。每次發布新版都會覆蓋這個 release；跟 https://zdon.taiko.ac 的下載是同一個檔案，"
            f"之後在遊戲標題畫面按 F9 就能更新。\n\n"
            f"- Version: `{version}`\n- Built from: {built}\n\n"
            f"Unzip it and run `YataiDON.exe`. Z-DON is a fork of [YataiDON](https://github.com/Yonokid/YataiDON) "
            f"(GPL-3.0).")


def rolling_release(cfg: dict, package: Path, version: str, repo_dir: Path) -> None:
    gh_cfg = cfg["github"]
    repo, tag, asset = gh_cfg["release_repo"], gh_cfg["tag"], gh_cfg["asset"]
    if shutil.which("gh") is None:
        raise RuntimeError("the GitHub CLI (gh) isn't on PATH")
    # the build's commit: the version is "<date>-<time>-<short sha>" (zdon_server.git_version); otherwise HEAD
    rev = version.rsplit("-", 1)[-1] if version.count("-") >= 2 else "HEAD"
    try:
        sha = _git(repo_dir, "rev-parse", "--verify", f"{rev}^{{commit}}")
    except subprocess.CalledProcessError:
        sha = _git(repo_dir, "rev-parse", "HEAD")
    pushed = _on_github(repo, sha)
    target = sha if pushed else _gh("api", f"repos/{repo}/commits/main", "--jq", ".sha").stdout.strip()
    title = f"Z-DON {version}"
    body = notes(version, sha, pushed)
    with tempfile.TemporaryDirectory() as tmp:
        zip_path = Path(tmp) / asset
        shutil.copyfile(package, zip_path)
        if _gh("release", "view", tag, "--repo", repo, check=False).returncode == 0:
            # move the tag, then swap the zip in place (the release, its URL and its download count stay)
            _gh("api", "-X", "PATCH", f"repos/{repo}/git/refs/tags/{tag}", "-f", f"sha={target}", "-F", "force=true")
            _gh("release", "edit", tag, "--repo", repo, "--title", title, "--notes", body, "--latest")
            _gh("release", "upload", tag, str(zip_path), "--repo", repo, "--clobber")
        else:
            _gh("release", "create", tag, str(zip_path), "--repo", repo, "--target", target, "--title", title,
                "--notes", body, "--latest")
    print(f"GitHub release {repo}@{tag} now has {asset} ({version})"
          + ("" if pushed else f"; {sha[:8]} isn't pushed, so the tag points at main {target[:8]}"))
    # the other platforms: .github/workflows/zdon-release.yml builds them from main on GitHub's runners and adds
    # them to the same release (only for a pushed build, so they come from the same commit)
    workflow = gh_cfg.get("workflow", "")
    if not workflow:
        return
    if not pushed:
        print(f"not starting {workflow}: push main first, then run it on GitHub (Actions) or publish again")
        return
    r = _gh("workflow", "run", workflow, "--repo", repo, "--ref", "main", "-f", f"version={version}", check=False)
    print(f"started {workflow} (Linux, macOS, Android)" if r.returncode == 0
          else f"couldn't start {workflow}: {r.stderr.strip()}")
