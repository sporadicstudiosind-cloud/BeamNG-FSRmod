#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Part of BeamNG FSR Bridge. See LICENSE at the repository root.
"""Fetch the AMD FSR SDK that this project builds against.

The SDK is deliberately NOT vendored into this repository. AMD's docs/license.md
grants redistribution "in binary form only", which conflicts with the per-file
MIT notices carried by the public API headers. Rather than pick a side of that
conflict and ship the headers anyway, the build fetches them. docs/LEGAL.md sets
out the reasoning in full.

What the release package *does* redistribute is the prebuilt signed DLLs from
Kits/FidelityFX/signedbin — binary form, explicitly permitted, with AMD's notice
reproduced in THIRD_PARTY_NOTICES.md.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path

SDK_REPO = "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK"

# Files the bridge compiles against. Verified present in SDK 2.3.0.
REQUIRED_HEADERS = [
    "Kits/FidelityFX/api/include/ffx_api.h",
    "Kits/FidelityFX/api/include/ffx_api_types.h",
    "Kits/FidelityFX/api/include/ffx_api_loader.h",
    "Kits/FidelityFX/api/include/dx12/ffx_api_dx12.h",
    "Kits/FidelityFX/upscalers/include/ffx_upscale.h",
    "Kits/FidelityFX/framegeneration/include/ffx_framegeneration.h",
    "Kits/FidelityFX/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h",
    "Kits/OpenSource/amd/antilag2/ffx_antilag2_dx12.h",
]

# Signed DLLs the release package redistributes. The loader is mandatory; the
# effect DLLs are per-feature, and a missing one disables just that feature.
SIGNED_BINARIES = {
    "amd_fidelityfx_loader_dx12.dll": "required",
    "amd_fidelityfx_upscaler_dx12.dll": "upscaling",
    "amd_fidelityfx_framegeneration_dx12.dll": "frame generation",
}


def run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess:
    print("$ " + " ".join(cmd))
    return subprocess.run(cmd, check=True, **kwargs)


def clone(dest: Path, ref: str | None) -> None:
    if (dest / ".git").is_dir():
        print(f"{dest} already contains a checkout; leaving it alone.")
        print("Delete it first if you want a clean fetch.")
        return

    dest.parent.mkdir(parents=True, exist_ok=True)
    cmd = ["git", "clone", "--depth", "1"]
    if ref:
        cmd += ["--branch", ref]
    cmd += [SDK_REPO, str(dest)]

    env = dict(os.environ)
    # The SDK carries large media through LFS that the build does not need.
    env["GIT_LFS_SKIP_SMUDGE"] = "1"
    run(cmd, env=env)


def verify(dest: Path) -> int:
    problems = 0

    print("\nChecking required headers:")
    for rel in REQUIRED_HEADERS:
        path = dest / rel
        status = "ok" if path.is_file() else "MISSING"
        if not path.is_file():
            problems += 1
        print(f"  [{status:>7}] {rel}")

    print("\nChecking signed binaries:")
    signed_dir = dest / "Kits/FidelityFX/signedbin"
    for name, purpose in SIGNED_BINARIES.items():
        path = signed_dir / name
        if path.is_file():
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            size_mb = path.stat().st_size / (1024 * 1024)
            print(f"  [     ok] {name}  ({size_mb:.1f} MB, {purpose})")
            print(f"            sha256 {digest}")
        else:
            problems += 1
            print(f"  [MISSING] {name}  (needed for {purpose})")

    licence = dest / "docs/license.md"
    if licence.is_file():
        print(f"\n  [     ok] docs/license.md — reproduce this in THIRD_PARTY_NOTICES.md")
    else:
        problems += 1
        print("\n  [MISSING] docs/license.md — required before redistributing any binary")

    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dest", default="third_party/FidelityFX-SDK",
                        help="where to place the checkout (default: %(default)s)")
    parser.add_argument("--ref", default=None,
                        help="tag or branch to check out (default: the repository default)")
    parser.add_argument("--verify-only", action="store_true",
                        help="check an existing checkout without cloning")
    args = parser.parse_args()

    dest = Path(args.dest).resolve()

    if not args.verify_only:
        try:
            clone(dest, args.ref)
        except subprocess.CalledProcessError as exc:
            print(f"\nclone failed ({exc}).", file=sys.stderr)
            print("If you are behind a proxy or offline, download the SDK release "
                  "manually and pass --verify-only --dest <path>.", file=sys.stderr)
            return 1
        except FileNotFoundError:
            print("git is not on PATH.", file=sys.stderr)
            return 1

    if not dest.is_dir():
        print(f"{dest} does not exist.", file=sys.stderr)
        return 1

    problems = verify(dest)

    print()
    if problems:
        print(f"{problems} problem(s) found. The build will not succeed against this "
              f"checkout.", file=sys.stderr)
        return 1

    print("SDK checkout looks complete. Configure the build with:")
    print(f"  cmake -S native -B native/build -DBFSR_BUILD_BRIDGE=ON -DFSR_SDK_DIR={dest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
