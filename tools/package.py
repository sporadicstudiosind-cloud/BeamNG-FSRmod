#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Part of BeamNG FSR Bridge. See LICENSE at the repository root.
"""Build the two distributable artefacts.

  1. beamng_fsr.zip       the BeamNG mod, dropped into the user mods folder
  2. beamng_fsr_bridge/   the native bridge, copied beside BeamNG.drive.exe

They are packaged separately on purpose. BeamNG mod ZIPs are content archives —
they cannot place a DLL next to the executable, and nothing in this project
tries to make them. Keeping the halves visibly separate also makes it obvious to
a user that installing the mod alone does not enable FSR.
"""

from __future__ import annotations

import argparse
import hashlib
import shutil
import sys
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# Copied from the SDK checkout into the bridge directory. AMD's licence permits
# redistribution in binary form; docs/license.md must travel with them.
SIGNED_DLLS = [
    "amd_fidelityfx_loader_dx12.dll",
    "amd_fidelityfx_upscaler_dx12.dll",
    "amd_fidelityfx_framegeneration_dx12.dll",
]

# Built by native/CMakeLists.txt.
BRIDGE_BINARIES = ["dxgi.dll", "d3d12.dll", "beamng_fsr_bridge.dll"]


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build_mod_zip(out_dir: Path) -> Path:
    """Packages mod/ as a BeamNG mod archive.

    The archive root must contain lua/, ui/, shaders/ directly — BeamNG mounts
    the ZIP as a virtual filesystem overlay, so an extra top-level folder would
    put every path one level too deep and nothing would load.
    """
    mod_dir = REPO / "mod"
    if not mod_dir.is_dir():
        raise SystemExit(f"{mod_dir} not found")

    out_dir.mkdir(parents=True, exist_ok=True)
    zip_path = out_dir / "beamng_fsr.zip"

    included = 0
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in sorted(mod_dir.rglob("*")):
            if not path.is_file():
                continue
            archive.write(path, path.relative_to(mod_dir).as_posix())
            included += 1

    print(f"mod archive:   {zip_path}  ({included} files, "
          f"{zip_path.stat().st_size / 1024:.0f} KB)")
    return zip_path


def build_bridge_dir(out_dir: Path, build_dir: Path, sdk_dir: Path | None) -> Path:
    """Assembles the directory a user copies beside BeamNG.drive.exe."""
    bridge_out = out_dir / "beamng_fsr_bridge"
    if bridge_out.exists():
        shutil.rmtree(bridge_out)
    bridge_out.mkdir(parents=True)

    missing: list[str] = []

    for name in BRIDGE_BINARIES:
        src = build_dir / name
        if src.is_file():
            shutil.copy2(src, bridge_out / name)
            print(f"  bridge:      {name}  ({src.stat().st_size / 1024:.0f} KB)")
        else:
            missing.append(f"{name} (build it: see docs/BUILD.md)")

    if sdk_dir:
        signed = sdk_dir / "Kits/FidelityFX/signedbin"
        for name in SIGNED_DLLS:
            src = signed / name
            if src.is_file():
                shutil.copy2(src, bridge_out / name)
                print(f"  AMD signed:  {name}  ({src.stat().st_size / (1024*1024):.1f} MB)")
                print(f"               sha256 {sha256(src)}")
            else:
                missing.append(f"{name} (from the AMD FSR SDK signedbin folder)")

        # AMD's licence requires the notice to accompany the binaries.
        licence = sdk_dir / "docs/license.md"
        if licence.is_file():
            shutil.copy2(licence, bridge_out / "AMD_FSR_SDK_LICENSE.md")
        else:
            missing.append("AMD docs/license.md (required to redistribute the DLLs)")
    else:
        missing.append("the AMD signed DLLs (--sdk-dir was not given)")

    for name in ("THIRD_PARTY_NOTICES.md", "LICENSE"):
        src = REPO / name
        if src.is_file():
            shutil.copy2(src, bridge_out / name)

    readme = bridge_out / "README.txt"
    readme.write_text(
        "BeamNG FSR Bridge — native half\n"
        "===============================\n\n"
        "Copy every file in this folder into the directory containing\n"
        "BeamNG.drive.exe (typically:\n"
        "  Steam\\steamapps\\common\\BeamNG.drive\\Bin64\\ )\n\n"
        "No BeamNG file is modified, overwritten or replaced. dxgi.dll and\n"
        "d3d12.dll here are proxies that forward to the genuine system DLLs;\n"
        "Windows loads them first because the executable's own directory\n"
        "precedes the system directory in the DLL search order.\n\n"
        "To uninstall, delete the files listed in docs/UNINSTALL.md. Nothing\n"
        "else needs undoing.\n\n"
        "BeamNG must be running the Direct3D 12 renderer. Vulkan and Direct3D\n"
        "11 cannot work — AMD ships no Vulkan FSR backend and FSR 3.1+ requires\n"
        "Direct3D 12. See docs/FEASIBILITY.md.\n\n"
        "The AMD FSR runtime DLLs in this folder are redistributed under AMD's\n"
        "licence, reproduced in AMD_FSR_SDK_LICENSE.md.\n",
        encoding="utf-8")

    if missing:
        print("\nIncomplete package. Missing:")
        for item in missing:
            print(f"  - {item}")

    return bridge_out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", default="dist", help="output directory (default: %(default)s)")
    parser.add_argument("--build-dir", default="native/build-win",
                        help="directory containing the built bridge DLLs")
    parser.add_argument("--sdk-dir", default=None,
                        help="AMD FSR SDK checkout, for the signed runtime DLLs")
    parser.add_argument("--mod-only", action="store_true",
                        help="build only the BeamNG mod archive")
    args = parser.parse_args()

    out_dir = (REPO / args.out).resolve()
    print(f"packaging into {out_dir}\n")

    build_mod_zip(out_dir)

    if not args.mod_only:
        print("\nbridge package:")
        sdk = Path(args.sdk_dir).resolve() if args.sdk_dir else None
        build_bridge_dir(out_dir, (REPO / args.build_dir).resolve(), sdk)

    print("\nDone. Installation instructions: docs/INSTALL.md")
    return 0


if __name__ == "__main__":
    sys.exit(main())
