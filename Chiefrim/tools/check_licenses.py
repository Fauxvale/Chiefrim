#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Keeps Chiefrim GPL-3.0 compatible (docs/LICENSING.md).

Checks, against licenses.toml:
- LICENSE at the repo root is the GNU's GPL-3.0 text, unchanged;
- every source file of Chiefrim's own carries
  "SPDX-License-Identifier: GPL-3.0-or-later";
- every FetchContent dependency in skse/CMakeLists.txt is listed, under an
  allowed license, and (once fetched) its license file still says so;
- CommonLibSSE-NG is still GPL-3.0;
- with --halo: the pinned OpenCE is still CC0, every folder in
  its port/third_party is listed and allowed or excluded, and no compiled
  file, nor any of our patches, carries an excluded component's code.

Exit status 1 on any problem. tools/setup_halo.py and tools/setup_skse.sh
run it before building.

Usage: tools/check_licenses.py [--halo]
"""

import hashlib
import re
import subprocess
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent      # Chiefrim/
REPO = ROOT.parent                                  # the git repository
SPDX = "SPDX-License-Identifier: GPL-3.0-or-later"
SOURCE_SUFFIXES = {".c", ".h", ".cpp", ".hpp", ".py", ".sh", ".cmake", ".toml", ".ini"}
SOURCE_NAMES = {"CMakeLists.txt"}

problems = []


def problem(text):
    problems.append(text)


def allowed(expression, allowed_ids):
    """an SPDX expression of ORs (WITH exceptions ignored) passes if any side does"""
    return any(part.split(" WITH ")[0].strip() in allowed_ids for part in expression.split(" OR "))


def check_markers(path, markers, what):
    if not path.is_file():
        problem(f"{what}: license file {path} is missing")
        return
    text = path.read_text(encoding="utf-8", errors="replace")
    for marker in markers:
        if marker not in text:
            problem(f"{what}: {path.name} no longer says {marker!r}; its license may have changed")


def check_project(manifest):
    license_file = REPO / "LICENSE"
    if not license_file.is_file():
        problem("LICENSE is missing at the repo root")
    elif hashlib.sha256(license_file.read_bytes()).hexdigest() != manifest["project"]["license_sha256"]:
        problem("LICENSE is not the unchanged GPL-3.0 text")


def check_headers():
    listed = subprocess.run(["git", "-C", str(REPO), "ls-files", "-z", "--", "Chiefrim"],
                            capture_output=True, text=True, check=True).stdout.split("\0")
    for name in filter(None, listed):
        path = REPO / name
        if not path.is_file() or "extern" in path.relative_to(ROOT).parts:
            continue
        if path.suffix not in SOURCE_SUFFIXES and path.name not in SOURCE_NAMES:
            continue
        head = path.read_text(encoding="utf-8", errors="replace").splitlines()[:5]
        if not any(SPDX in line for line in head):
            problem(f"{name}: no '{SPDX}' line in its first lines")


def check_skse(manifest):
    policy = manifest["policy"]
    commonlib = manifest["skse"]["commonlib"]
    base = ROOT / commonlib["path"]
    if not allowed(commonlib["license"], policy["code"]):
        problem(f"CommonLibSSE-NG: {commonlib['license']} is not allowed")
    if (base / commonlib["license_file"]).is_file():
        check_markers(base / commonlib["license_file"], commonlib["markers"], "CommonLibSSE-NG")
        check_markers(base / "README.md", [commonlib["readme_marker"]], "CommonLibSSE-NG")
        if not (base / commonlib["exceptions_file"]).is_file():
            problem("CommonLibSSE-NG: EXCEPTIONS.md is missing")
    else:
        print("check_licenses: CommonLibSSE-NG submodule not checked out; skipped")

    listed = {entry["name"]: entry for entry in manifest["skse"]["fetchcontent"]}
    cmake = (ROOT / "skse" / "CMakeLists.txt").read_text()
    declared = re.findall(r"FetchContent_Declare\(\s*([A-Za-z0-9_\-]+)", cmake)
    for name in declared:
        entry = listed.get(name)
        if not entry:
            problem(f"skse: FetchContent dependency {name!r} is not in licenses.toml")
            continue
        if not allowed(entry["license"], policy["code"]):
            problem(f"skse: {name} is {entry['license']}, which is not allowed")
        fetched = ROOT / "build" / "skse" / "_deps" / f"{name.lower()}-src"
        if fetched.is_dir():
            check_markers(fetched / entry["license_file"], entry["markers"], f"skse: {name}")
    for name in listed:
        if name not in declared:
            problem(f"licenses.toml lists skse dependency {name!r}, which skse/CMakeLists.txt no longer fetches")


def check_halo(manifest):
    policy = manifest["policy"]
    work = ROOT / "halo" / ".work"
    if not work.is_dir():
        problem("halo/.work is missing; run tools/setup_halo.py")
        return
    upstream = manifest["halo"]["upstream"]
    check_markers(work / upstream["license_file"], upstream["markers"], "OpenCE")

    listed = {entry["dir"]: entry for entry in manifest["halo"]["third_party"]}
    present = sorted(p.name for p in (work / "port" / "third_party").iterdir() if p.is_dir())
    for name in present:
        entry = listed.get(name)
        if not entry:
            problem(f"halo: port/third_party/{name} is not in licenses.toml")
            continue
        if entry.get("excluded"):
            replaced = work / entry["replaced_by"]
            override = ROOT / "halo" / "overrides" / entry["replaced_by"]
            if not override.is_file() or not replaced.is_file() or replaced.read_bytes() != override.read_bytes():
                problem(f"halo: {name} is excluded, but {entry['replaced_by']} is not Chiefrim's override")
            continue
        if not allowed(entry["license"], policy["code"]):
            problem(f"halo: {name} is {entry['license']}, which is not allowed")
        check_markers(work / "port" / "third_party" / name / entry["license_file"], entry["markers"], f"halo: {name}")

    banned = [entry["banned_marker"] for entry in listed.values() if entry.get("excluded")]
    replaced = {entry["replaced_by"] for entry in listed.values() if entry.get("excluded")}
    for patch in sorted((ROOT / "halo" / "patches").glob("*.patch")):
        text = patch.read_text(encoding="utf-8", errors="replace")
        for marker in banned:
            if marker in text:
                problem(f"halo: {patch.name} carries code of an excluded component ({marker})")
        for path in replaced:
            if f"a/{path} " in text or f"b/{path}\n" in text:
                problem(f"halo: {patch.name} touches {path}, which halo/overrides replaces whole")
    for folder in ("source", "port/linux", "port/windows", "port/include"):
        for path in (work / folder).rglob("*"):
            if path.suffix not in {".c", ".h", ".cpp"} or not path.is_file():
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            for marker in banned:
                if marker in text:
                    problem(f"halo: {path.relative_to(work)} carries code of an excluded component ({marker})")

    for entry in manifest["halo"]["in_source"]:
        if not allowed(entry["license"], policy["code"]):
            problem(f"halo: {entry['dir']} is {entry['license']}, which is not allowed")
    for entry in manifest["halo"]["data"]:
        if not allowed(entry["license"], policy["data"] + policy["code"]):
            problem(f"halo: {entry['name']} is {entry['license']}, which is not allowed for data")


def main():
    manifest = tomllib.loads((ROOT / "licenses.toml").read_text())
    for expression in policy_violations(manifest):
        problem(expression)
    check_project(manifest)
    check_headers()
    check_skse(manifest)
    if "--halo" in sys.argv[1:]:
        check_halo(manifest)

    for text in problems:
        print(f"check_licenses: {text}")
    if problems:
        print("check_licenses: FAILED (docs/LICENSING.md)")
        return 1
    print("check_licenses: ok, GPL-3.0-or-later compatible" + (" (with Halo)" if "--halo" in sys.argv else ""))
    return 0


def policy_violations(manifest):
    """an allowed list may never name a forbidden license"""
    policy = manifest["policy"]
    return [f"licenses.toml allows the forbidden {name}" for name in policy["forbidden"]
            if name in policy["code"] or name in policy["data"]]


if __name__ == "__main__":
    sys.exit(main())
