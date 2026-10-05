# Licensing

Chiefrim is licensed under the **GNU General Public License, version 3 or (at your option) any
later version** (SPDX: `GPL-3.0-or-later`). The full text is `LICENSE` at the repo root.

It is GPL-3.0 because the SKSE plugin links **CommonLibSSE-NG**, which is GPL-3.0-or-later
(with its own Modding Exception for Skyrim and SKSE). A plugin that links it is a combined work
and must be GPL-compatible. "Or later" matches CommonLibSSE-NG's own terms.

## The rule: everything stays GPL-3.0 compatible

1. **Our own files** carry `SPDX-License-Identifier: GPL-3.0-or-later` in their first lines:
   C, C++, Python, shell, CMake and TOML.
2. **Every third-party component** compiled or linked into a Chiefrim binary is listed in
   `Chiefrim/licenses.toml`, under a license from its `policy.code` list. Fonts and images
   embedded as data may also use `policy.data` (OFL-1.1, CC-BY, CC0, public domain).
3. **Never** in a distributed build: GPL-incompatible licenses (`policy.forbidden`), such as
   the original 4-clause BSD license, GPL-2.0-only, SSPL, non-commercial or no-derivatives
   terms, the JSON license, or proprietary code.
4. **Dual-licensed components** list the license Chiefrim takes (`taken`): Mbed TLS takes
   Apache-2.0, Monocypher CC0-1.0, and stb MIT.
5. **Game data is never part of Chiefrim.** Skyrim's and Halo's data stay with the user (the
   repo ignores `HaloProjects/`; Halo reads the user's own extracted `maps/`).

### Enforcement

`Chiefrim/tools/check_licenses.py` checks all of the above:
- `LICENSE` is the GNU's GPL-3.0 text, unchanged.
- Every tracked source file of ours has its SPDX line.
- Every `FetchContent_Declare` in `skse/CMakeLists.txt` is listed and allowed, and its fetched
  license file still says what we recorded.
- CommonLibSSE-NG is still GPL-3.0.
- With `--halo`: the pinned halo-ce-universal is still CC0; every folder in its
  `port/third_party` is listed and allowed, or excluded; and no compiled file still carries an
  excluded component's code.

It runs automatically in `tools/setup_halo.py`, `tools/setup_skse.sh` (before and after
fetching dependencies) and `tools/package_skse.sh`. **A violation stops the build.** Run it by
hand with:

```sh
Chiefrim/tools/check_licenses.py --halo
```

### Adding a dependency

1. Find its license (the SPDX id) and check it's in `policy.code`. If it isn't, don't add it.
   Ask first.
2. Add an entry to `Chiefrim/licenses.toml`: the name, the license, the license file, a few
   marker strings from that file, and the copyright notice.
3. Add it to `THIRD-PARTY-NOTICES.md` at the repo root.
4. Run the checker.

The same applies when moving `halo/UPSTREAM` to a newer halo-ce-universal: a new folder in
`port/third_party` fails the check until it's listed.

## Known exclusion: extract-xiso

The Halo port copies `maps/` out of an Xbox disc image with `port/linux/src/xiso.c`. That
file follows extract-xiso and carries its license: the original **4-clause BSD license**, whose
advertising clause makes it **incompatible with the GPL**. Chiefrim's build replaces that one
file with a stub (`Chiefrim/halo/overrides/port/linux/src/xiso.c`, applied by `setup_halo.py`)
that asks for an already-extracted `maps/` instead. That's what Chiefrim uses anyway
(`HALO_MAPS`). The stub contains none of the original code, and the patches in `halo/patches`
never touch that file.

A GPL-clean disc-image reader written from the XDVDFS format alone could bring the feature
back later, if it's wanted.

## System libraries

These are used as **system libraries** (GPL-3.0 §1) and Chiefrim never distributes them:
- On Windows/Proton: the MSVC C/C++ runtime and the Windows SDK. CommonLibSSE-NG's exception
  also names Windows and SKSE as Modding Libraries.
- On Linux: SDL3, glibc, OpenGL and the audio libraries.

The xwin download of Microsoft's CRT and SDK is a build tool. Its files are never committed or
shipped.

## Releasing binaries

A binary release (for example the mod zip from `tools/package_skse.sh`) must:
- include `LICENSE` and `THIRD-PARTY-NOTICES.md` (the zip does, as
  `SKSE/Plugins/Chiefrim-LICENSE.txt` and `Chiefrim-THIRD-PARTY-NOTICES.md`);
- point to the **corresponding source**: this repository at the release's commit, including the
  CommonLibSSE-NG submodule commit, `halo/UPSTREAM`, `halo/patches` and `halo/overrides`. For
  example, a public repo with a release tag.

Whether a built Halo executable may be distributed at all is a separate question from these
licenses, because the decompilation is of Microsoft's game. Decide it before any public
release (DESIGN.md §15).
