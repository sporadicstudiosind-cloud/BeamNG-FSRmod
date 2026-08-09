#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Part of BeamNG FSR Bridge. See LICENSE at the repository root.
"""Build the two independently installable products.

    dist/framegen/   AMD FSR Frame Generation
    dist/upscaling/  AMD FSR Upscaling

Each contains a BeamNG mod ZIP and a bridge folder. Either can be installed on
its own; installing both is also fine.

The two bridge folders contain the *same* binaries — that is what makes
co-installation safe, since overwriting a file with an identical copy is a
no-op. What differs is the module manifest each drops into
`beamng_fsr/modules/`, and the shared runtime starts a feature only when its
manifest is present.

The mod ZIPs likewise share `mods/shared/`, copied into both. BeamNG mounts mod
archives as a virtual filesystem overlay, so identical files from two archives
simply shadow each other harmlessly.
"""

from __future__ import annotations

import argparse
import hashlib
import shutil
import sys
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# Built by native/CMakeLists.txt. Identical in both products.
BRIDGE_BINARIES = ["dxgi.dll", "d3d12.dll", "beamng_fsr_bridge.dll"]

# AMD's licence permits redistribution in binary form; docs/license.md must
# travel with them. Only the effect DLL a product actually uses is shipped, so
# installing frame generation alone does not drag in a 27 MB upscaler.
PRODUCTS = {
    "framegen": {
        "title": "AMD FSR Frame Generation",
        "mod_dir": "mods/framegen",
        "zip_name": "beamng_fsr_framegen.zip",
        "manifest": "packaging/manifests/framegen.module.json",
        "effect_dlls": ["amd_fidelityfx_framegeneration_dx12.dll"],
        "ui_app": "FsrFrameGeneration",
        "note": "Frame generation raises displayed frame rate and also input\n"
                "latency. The panel reports rendered and displayed rates\n"
                "separately; they are not the same number.",
    },
    "upscaling": {
        "title": "AMD FSR Upscaling",
        "mod_dir": "mods/upscaling",
        "zip_name": "beamng_fsr_upscaling.zip",
        "manifest": "packaging/manifests/upscaling.module.json",
        "effect_dlls": ["amd_fidelityfx_upscaler_dx12.dll"],
        "ui_app": "FsrUpscaling",
        "note": "BeamNG exposes no camera-jitter hook, so FSR runs without the\n"
                "sub-pixel jitter its temporal upscaler is designed around.\n"
                "Read docs/LIMITATIONS.md before deciding this is for you.",
    },
}

# Required by both; the loader routes to whichever effect DLLs are present.
LOADER_DLL = "amd_fidelityfx_loader_dx12.dll"

SHARED_MOD_DIR = "mods/shared"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build_mod_zip(product: str, spec: dict, out_dir: Path) -> Path:
    """Packages one product's mod archive.

    The archive root must contain lua/, ui/, shaders/ directly — BeamNG mounts
    the ZIP as a virtual filesystem overlay, so an extra top-level folder would
    put every path one level too deep and nothing would load.
    """
    product_dir = REPO / spec["mod_dir"]
    shared_dir = REPO / SHARED_MOD_DIR
    if not product_dir.is_dir():
        raise SystemExit(f"{product_dir} not found")

    out_dir.mkdir(parents=True, exist_ok=True)
    zip_path = out_dir / spec["zip_name"]

    entries: dict[str, Path] = {}

    # Product-specific files first.
    #
    # mod_info.json is deliberately excluded from the archive. BeamNG's packing
    # rules state the ZIP root "must contain only top-level folders" and warn
    # against loose files there, and a root-level mod_info.json would also
    # collide between the two products when both are installed — BeamNG mounts
    # archives as an overlay, so one would silently shadow the other. The file
    # stays in the source tree as publishing metadata for the mod repository.
    for path in sorted(product_dir.rglob("*")):
        if path.is_file() and path.name != "mod_info.json":
            entries[path.relative_to(product_dir).as_posix()] = path

    # Then the shared half, mapped into the places each product expects it.
    for path in sorted((shared_dir / "lua").rglob("*")):
        if path.is_file():
            entries[path.relative_to(shared_dir).as_posix()] = path
    for path in sorted((shared_dir / "shaders").rglob("*")):
        if path.is_file():
            entries[path.relative_to(shared_dir).as_posix()] = path

    # The stylesheet and shared UI helpers sit beside each product's app, because
    # a BeamNG UI app resolves its assets relative to its own directory.
    app_dir = f"ui/modules/apps/{spec['ui_app']}"
    entries[f"{app_dir}/app.css"] = shared_dir / "app.css"
    entries[f"{app_dir}/ui_shared.js"] = shared_dir / "ui_shared.js"

    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for arcname, path in sorted(entries.items()):
            archive.write(path, arcname)

    print(f"  mod archive:  {zip_path.name}  "
          f"({len(entries)} files, {zip_path.stat().st_size / 1024:.0f} KB)")
    return zip_path


def build_bridge_dir(product: str, spec: dict, out_dir: Path,
                     build_dir: Path, sdk_dir: Path | None) -> list[str]:
    """Assembles the folder a user copies beside BeamNG.drive.exe."""
    bridge_out = out_dir / "bridge"
    if bridge_out.exists():
        shutil.rmtree(bridge_out)
    bridge_out.mkdir(parents=True)

    missing: list[str] = []

    for name in BRIDGE_BINARIES:
        src = build_dir / name
        if src.is_file():
            shutil.copy2(src, bridge_out / name)
        else:
            missing.append(f"{name} (build it: see docs/BUILD.md)")
    if not missing:
        print(f"  bridge:       {', '.join(BRIDGE_BINARIES)}")

    # The manifest is the entire mechanism: without it the shared runtime never
    # starts this product's feature, which is how one product stays inert when
    # only the other is installed.
    modules_dir = bridge_out / "beamng_fsr" / "modules"
    modules_dir.mkdir(parents=True, exist_ok=True)
    manifest_src = REPO / spec["manifest"]
    if manifest_src.is_file():
        shutil.copy2(manifest_src, modules_dir / manifest_src.name)
        print(f"  manifest:     beamng_fsr/modules/{manifest_src.name}")
    else:
        missing.append(f"{spec['manifest']} (the module manifest)")

    if sdk_dir:
        signed = sdk_dir / "Kits/FidelityFX/signedbin"
        for name in [LOADER_DLL] + spec["effect_dlls"]:
            src = signed / name
            if src.is_file():
                shutil.copy2(src, bridge_out / name)
                print(f"  AMD signed:   {name}  ({src.stat().st_size / (1024*1024):.1f} MB)")
                print(f"                sha256 {sha256(src)}")
            else:
                missing.append(f"{name} (from the AMD FSR SDK signedbin folder)")

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

    other = "upscaling" if product == "framegen" else "framegen"
    (bridge_out / "README.txt").write_text(
        f"{spec['title']} — native half\n"
        f"{'=' * (len(spec['title']) + 14)}\n\n"
        "Copy everything in this folder into the directory containing\n"
        "BeamNG.drive.exe (typically:\n"
        "  Steam\\steamapps\\common\\BeamNG.drive\\Bin64\\ )\n\n"
        "That includes the beamng_fsr\\modules\\ folder — the manifest inside it\n"
        "is what tells the bridge this product is installed.\n\n"
        "This product works on its own. The separate\n"
        f"{PRODUCTS[other]['title']} product is optional; installing both is\n"
        "supported and they share these same binaries, so copying one over the\n"
        "other is harmless.\n\n"
        "No BeamNG file is modified, overwritten or replaced. dxgi.dll and\n"
        "d3d12.dll here are proxies that forward to the genuine system DLLs;\n"
        "Windows loads them first because the executable's own directory\n"
        "precedes the system directory in the DLL search order.\n\n"
        "BeamNG must be running the Direct3D 12 renderer. Vulkan and Direct3D 11\n"
        "cannot work — AMD ships no Vulkan FSR backend and FSR 3.1+ requires\n"
        "Direct3D 12. See docs/FEASIBILITY.md.\n\n"
        f"{spec['note']}\n\n"
        "To uninstall, see docs/UNINSTALL.md. Deleting this product's manifest\n"
        "alone disables it while leaving the other product working.\n\n"
        "The AMD FSR runtime DLLs here are redistributed under AMD's licence,\n"
        "reproduced in AMD_FSR_SDK_LICENSE.md.\n",
        encoding="utf-8")

    return missing


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", default="dist", help="output directory (default: %(default)s)")
    parser.add_argument("--build-dir", default="native/build-win",
                        help="directory containing the built bridge DLLs")
    parser.add_argument("--sdk-dir", default=None,
                        help="AMD FSR SDK checkout, for the signed runtime DLLs")
    parser.add_argument("--product", choices=sorted(PRODUCTS), default=None,
                        help="build only one product (default: both)")
    parser.add_argument("--mod-only", action="store_true",
                        help="build only the BeamNG mod archives")
    args = parser.parse_args()

    out_root = (REPO / args.out).resolve()
    build_dir = (REPO / args.build_dir).resolve()
    sdk = Path(args.sdk_dir).resolve() if args.sdk_dir else None

    selected = [args.product] if args.product else sorted(PRODUCTS)
    print(f"packaging into {out_root}\n")

    all_missing: dict[str, list[str]] = {}
    for product in selected:
        spec = PRODUCTS[product]
        print(f"{spec['title']}")
        out_dir = out_root / product
        build_mod_zip(product, spec, out_dir)
        if not args.mod_only:
            missing = build_bridge_dir(product, spec, out_dir, build_dir, sdk)
            if missing:
                all_missing[spec["title"]] = missing
        print()

    if all_missing:
        print("Incomplete package(s):")
        for title, items in all_missing.items():
            print(f"  {title}")
            for item in items:
                print(f"    - {item}")
        print()

    print("Done. Each product under dist/ installs independently.")
    print("Installation instructions: docs/INSTALL.md")
    return 0


if __name__ == "__main__":
    sys.exit(main())
