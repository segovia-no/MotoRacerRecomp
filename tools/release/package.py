#!/usr/bin/env python3
"""Package a program built without game code together with its kit (docs/28-easy-install.md).

    package.py --platform macos-arm64|macos-x86_64|linux-x86_64|windows-x86_64
               --program PATH --tools DIR --out DIR [--host-lib LIB] [--build-id ID]

PATH is a motoracer binary built with MOTORACER_GAME_MODULE=ON and no game.EXE; DIR the platform's
tools folder (tools/release/fetch_tools.py). Writes to --out:
    macOS    MotoRacer-<platform>.zip      MotoRacer.app (kit in Contents/Resources/kit), ad-hoc signed
    Windows  MotoRacer-<platform>.zip      MotoRacer/motoracer.exe + MotoRacer/kit/
    Linux    MotoRacer-<platform>.tar.xz   MotoRacer/motoracer + MotoRacer/kit/
Nothing in a package is translated from game.EXE: the program builds that on the player's machine.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tarfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))


def make_kit(tools, out, host_lib, build_id):
    cmd = [sys.executable, os.path.join(HERE, "make_kit.py"), "--tools", tools, "--out", out]
    if host_lib:
        cmd += ["--host-lib", host_lib]
    if build_id:
        cmd += ["--build-id", build_id]
    subprocess.run(cmd, check=True)


def zip_dir(src, zip_path, base):
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for d, _, names in os.walk(src):
            for n in names:
                p = os.path.join(d, n)
                info = zipfile.ZipInfo.from_file(p, os.path.relpath(p, base))
                info.compress_type = zipfile.ZIP_DEFLATED
                with open(p, "rb") as f:
                    z.writestr(info, f.read())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--platform", required=True,
                    choices=["macos-arm64", "macos-x86_64", "linux-x86_64", "windows-x86_64"])
    ap.add_argument("--program", required=True)
    ap.add_argument("--tools", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--host-lib")
    ap.add_argument("--build-id")
    args = ap.parse_args()
    out = os.path.abspath(args.out)
    stage = os.path.join(out, "stage-" + args.platform)
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(stage)
    name = "MotoRacer-" + args.platform

    if args.platform.startswith("macos"):
        subprocess.run([os.path.join(ROOT, "tools", "make_app.sh"), args.program, stage], check=True,
                       stdout=subprocess.DEVNULL)
        app = os.path.join(stage, "MotoRacer.app")
        make_kit(args.tools, os.path.join(app, "Contents", "Resources", "kit"), None, args.build_id)
        # sign again with the kit inside (ad-hoc: no Apple Developer ID)
        subprocess.run(["codesign", "--force", "--deep", "--sign", "-", app], check=True)
        archive = os.path.join(out, name + ".zip")
        if os.path.exists(archive):
            os.remove(archive)
        # ditto keeps the bundle's symlinks, permissions and signature
        subprocess.run(["ditto", "-c", "-k", "--keepParent", app, archive], check=True)
    else:
        top = os.path.join(stage, "MotoRacer")
        os.makedirs(top)
        exe = "motoracer.exe" if args.platform.startswith("windows") else "motoracer"
        shutil.copy2(args.program, os.path.join(top, exe))
        make_kit(args.tools, os.path.join(top, "kit"), args.host_lib, args.build_id)
        if args.platform.startswith("windows"):
            archive = os.path.join(out, name + ".zip")
            zip_dir(top, archive, stage)
        else:
            archive = os.path.join(out, name + ".tar.xz")
            with tarfile.open(archive, "w:xz") as t:
                t.add(top, arcname="MotoRacer")
    print(f"{archive}: {os.path.getsize(archive) / 1e6:.0f} MB")


if __name__ == "__main__":
    main()
