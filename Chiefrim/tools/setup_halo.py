#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds the Halo side of Chiefrim (docs/DESIGN.md §14).

Chiefrim does not fork halo-ce-universal. It pins an upstream commit, and
this script makes the work tree halo/.work (git-ignored) from it:

1. clone upstream into halo/.work (first run), or reset it to the pin;
2. apply halo/patches/*.patch (the small hooks marked CHIEFRIM);
3. copy halo/overrides/** over the tree: whole files Chiefrim replaces,
   for licensing (docs/LICENSING.md: xiso.c);
4. copy halo/src/* and protocol/chiefrim_protocol.h to source/chiefrim/,
   where the game's build picks up every .c file by itself, and
   halo/src/port/* to port/linux/src/ (code on the port's OpenGL device,
   built with the platform layer's flags);
5. check licenses (tools/check_licenses.py), then configure and build with
   ninja (unless --no-build).

The result is halo/.work/build/linux/halo. No game data is involved: the
game asks for the user's own Xbox disc image, or finds maps/ next to it.

Usage: tools/setup_halo.py [--release] [--no-build] [--keep]
  --keep  don't reset halo/.work: rebuild what is there (for working on the
          hooks; tools/save_halo_patch.sh writes them back to halo/patches)
"""

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HALO = ROOT / "halo"
WORK = HALO / ".work"
UPSTREAM_URL = "https://github.com/cybersecurity/halo-ce-universal"
UPSTREAM_PIN = (HALO / "UPSTREAM").read_text().split()[0]


def run(*args, cwd=WORK):
    print("+", " ".join(str(a) for a in args), flush=True)
    subprocess.run([str(a) for a in args], cwd=cwd, check=True)


def install(source, destination):
    """Copies a file whose contents changed, stamped now: ninja rebuilds by
    time, and a copy keeping the source's older time (shutil.copy2) left
    objects built from an edited work tree in place."""
    if destination.exists() and destination.read_bytes() == source.read_bytes():
        return
    shutil.copyfile(source, destination)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--release", action="store_true")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--keep", action="store_true")
    options = parser.parse_args()

    if not WORK.exists():
        run("git", "clone", UPSTREAM_URL, WORK, cwd=ROOT)
    if not options.keep:
        run("git", "fetch", "--quiet", "origin")
        run("git", "checkout", "--quiet", "--force", "--detach", UPSTREAM_PIN)
        run("git", "clean", "-fdq", "-e", "build/", "-e", "assets/")
        for patch in sorted((HALO / "patches").glob("*.patch")):
            run("git", "apply", "--whitespace=nowarn", patch)

    overrides = HALO / "overrides"
    for source in sorted(overrides.rglob("*")):
        if source.is_file():
            destination = WORK / source.relative_to(overrides)
            destination.parent.mkdir(parents=True, exist_ok=True)
            install(source, destination)

    target = WORK / "source" / "chiefrim"
    target.mkdir(exist_ok=True)
    for source in sorted((HALO / "src").iterdir()):
        if source.is_file():
            install(source, target / source.name)
    for source in sorted((HALO / "src" / "port").iterdir()):
        install(source, WORK / "port" / "linux" / "src" / source.name)
    install(ROOT / "protocol" / "chiefrim_protocol.h", target / "chiefrim_protocol.h")

    run(sys.executable, ROOT / "tools" / "check_licenses.py", "--halo", cwd=ROOT)
    if options.no_build:
        return 0
    configure = [sys.executable, "configure.py", "--pgo=off", "--lto=off"]
    if options.release:
        configure.append("--release")
    run(*configure)
    run("ninja", "linux")
    print(f"built {WORK / 'build' / 'linux' / 'halo'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
