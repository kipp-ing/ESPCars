#!/usr/bin/env python3
"""Build immutable P9 images in detached scratch worktrees; never upload."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

REPO = Path(__file__).resolve().parents[2]
REFERENCE = "cb0db60"
ROLES = {
    **{f"ring-{b}": (b, f"p9-ring-{b}.yaml") for b in ("green", "blue", "orange")},
    **{
        f"selftest-{b}": (b, f"p9-selftest-{b}.yaml")
        for b in ("green", "blue", "orange")
    },
    **{
        f"profile-{b}": (b, f"p9-profile-{b}.yaml") for b in ("green", "blue", "orange")
    },
    "load-green": ("green", "p9-load-green.yaml"),
    "load-blue": ("blue", "p9-load-blue.yaml"),
    "gateway-orange": ("orange", "p9-gateway-orange.yaml"),
    "rotate-orange": ("orange", "p9-rotate-orange.yaml"),
    "collect-orange": ("orange", "p9-collect-orange.yaml"),
}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command: list[str], *, cwd: Path, log: Path | None = None) -> str:
    result = subprocess.run(
        command, cwd=cwd, text=True, capture_output=True, check=False
    )
    output = result.stdout + result.stderr
    if log is not None:
        log.write_text(output, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(command)}; {log or output[-1500:]}"
        )
    return result.stdout.strip()


@contextmanager
def worktree(repo: Path, sha: str):
    with tempfile.TemporaryDirectory(prefix="p9-worktree-") as temp:
        # Keep even Git's worktree registry in scratch: the source checkout may
        # be read-only, and parallel harness runs must not alter its metadata.
        registry = Path(temp) / "git"
        run(
            ["git", "clone", "--shared", "--no-checkout", str(repo), str(registry)],
            cwd=repo,
        )
        tree = Path(temp) / "source"
        run(["git", "worktree", "add", "--detach", str(tree), sha], cwd=registry)
        try:
            yield tree
        finally:
            run(["git", "worktree", "remove", "--force", str(tree)], cwd=registry)


def snapshot(source: Path, destination: Path) -> str:
    """Freeze public YAML/headers only. Never copy logs, captures or secrets."""
    destination.mkdir(parents=True)
    for path in sorted(source.iterdir()):
        if path.suffix in (".yaml", ".h") and path.name != "secrets.yaml":
            shutil.copyfile(path, destination / path.name)
    digest = hashlib.sha256()
    for path in sorted(destination.iterdir()):
        digest.update(path.name.encode() + b"\0" + path.read_bytes())
    return digest.hexdigest()


def stage(tree: Path, frozen: Path, secrets: Path | None) -> Path:
    hil = tree / "tests/hil"
    shutil.rmtree(hil)
    shutil.copytree(frozen, hil)
    if secrets:
        shutil.copyfile(secrets, hil / "secrets.yaml")
    else:
        # Config-only validation needs syntax, not credentials. Never flash this build.
        (hil / "secrets.yaml").write_text(
            'wifi_ssid: "fictional-p9"\nwifi_password: "fictional-password"\n',
            encoding="utf-8",
        )
    return hil


def manifest_entry(
    ref: str, sha: str, role: str, yaml_name: str, image: Path, factory: Path
) -> dict:
    return {
        "ref": ref,
        "sha": sha,
        "role": role,
        "board": ROLES[role][0],
        "yaml": f"tests/hil/{yaml_name}",
        "image": str(image),
        "sha256": sha256(image),
        "flash_image": str(factory),
        "flash_sha256": sha256(factory),
        "flash_offset": "0x0",
    }


def build(
    ref: str,
    out: Path,
    *,
    repo: Path = REPO,
    yaml_source: Path | None = None,
    secrets: Path | None = None,
    config_only: bool = False,
) -> Path:
    out = out.resolve()
    if out.is_relative_to(repo.resolve()):
        raise ValueError("P9 artifacts and bench data must live outside the repository")
    if out.exists() and any(out.iterdir()):
        raise ValueError("output directory must be empty (immutable image set)")
    if not config_only and secrets is None:
        raise ValueError(
            "build needs --secrets outside the repo for the collection image; --config-only needs none"
        )
    if secrets and secrets.resolve().is_relative_to(repo.resolve()):
        raise ValueError("keep real credentials outside the repository")
    out.mkdir(parents=True, exist_ok=True)
    sha = run(["git", "rev-parse", "--verify", f"{ref}^{{commit}}"], cwd=repo)
    head = run(["git", "rev-parse", "HEAD"], cwd=repo)
    baseline = run(["git", "rev-parse", REFERENCE], cwd=repo)
    frozen = out / "yaml"
    yaml_hash = snapshot(yaml_source or repo / "tests/hil", frozen)
    python = str(repo / ".venv/bin/python")
    tool_version = run([python, "-m", "esphome", "version"], cwd=repo)
    checks = []
    # Both schemas must accept precisely the same frozen YAML. No ref-specific rewrites.
    for label, check_sha in dict.fromkeys(
        [("reference", baseline), ("HEAD", head), ("target", sha)]
    ):
        with worktree(repo, check_sha) as tree:
            hil = stage(tree, frozen, secrets)
            for role, (_, yaml_name) in ROLES.items():
                log = out / f"config-{label}-{role}.log"
                run(
                    [python, "-m", "esphome", "config", str(hil / yaml_name)],
                    cwd=tree,
                    log=log,
                )
                checks.append(
                    {
                        "ref": label,
                        "sha": check_sha,
                        "role": role,
                        "result": "PASS",
                        "log": str(log),
                    }
                )
    result = {
        "schema": 1,
        "ref": ref,
        "sha": sha,
        "yaml_source_sha": head,
        "yaml_sha256": yaml_hash,
        "tool_version": tool_version,
        "config_checks": checks,
        "config_only": config_only,
        "images": [],
    }
    if not config_only:
        with worktree(repo, sha) as tree:
            hil = stage(tree, frozen, secrets)
            for role, (_, yaml_name) in ROLES.items():
                # Unique absolute directory even though all variants keep the board's node name.
                build_dir = tree / f"p9-build/{role}"
                wrapper = hil / f"build-{role}.yaml"
                wrapper.write_text(
                    f"packages:\n  bench: !include {yaml_name}\nesphome:\n  build_path: {json.dumps(str(build_dir))}\n",
                    encoding="utf-8",
                )
                run(
                    [python, "-m", "esphome", "compile", str(wrapper)],
                    cwd=tree,
                    log=out / f"compile-{role}.log",
                )
                # ESPHome's native IDF backend writes <node>.bin and an OTA
                # copy; PlatformIO writes firmware.bin. Both are app-only.
                apps = list(build_dir.rglob("firmware.ota.bin")) or list(
                    build_dir.rglob("firmware.bin")
                )
                factories = [p for p in build_dir.rglob("*.bin") if "factory" in p.name]
                if len(apps) != 1 or len(factories) != 1:
                    raise RuntimeError(
                        f"{role}: expected exactly one firmware.bin and one factory image; refusing ambiguous upload"
                    )
                image_dir = out / role
                image_dir.mkdir()
                image, factory = (
                    image_dir / "firmware.bin",
                    image_dir / "firmware.factory.bin",
                )
                shutil.copyfile(apps[0], image)
                shutil.copyfile(factories[0], factory)
                result["images"].append(
                    manifest_entry(ref, sha, role, yaml_name, image, factory)
                )
    path = out / "manifest.json"
    path.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ref")
    parser.add_argument("--out", type=Path)
    parser.add_argument(
        "--yaml-source", type=Path, help="reuse the first build's frozen yaml directory"
    )
    parser.add_argument(
        "--secrets",
        type=Path,
        help="external WiFi secrets file; never stored with the public snapshot",
    )
    parser.add_argument("--config-only", action="store_true")
    args = parser.parse_args()
    out = args.out or Path(tempfile.mkdtemp(prefix="p9-images-"))
    try:
        print(
            build(
                args.ref,
                out,
                yaml_source=args.yaml_source,
                secrets=args.secrets,
                config_only=args.config_only,
            )
        )
    except (OSError, ValueError, RuntimeError) as exc:
        parser.exit(2, f"{exc}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
