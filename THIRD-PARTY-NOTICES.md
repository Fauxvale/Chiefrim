# Third-party notices

Chiefrim is licensed under the **GNU General Public License v3.0 or later** (`LICENSE`).
Its binaries include, or are built from, the components below. Each keeps its own license,
and every one of them is compatible with GPL-3.0. Their full license texts are in their own
source trees: the CommonLibSSE-NG submodule, the dependencies CMake fetches, and the pinned
halo-ce-universal checkout.

`Chiefrim/licenses.toml` is the machine-checked list of these components.
`Chiefrim/tools/check_licenses.py` enforces it (see `Chiefrim/docs/LICENSING.md`).

No game data from Skyrim or Halo is part of Chiefrim. Users supply both games.

## SKSE plugin (`Chiefrim.dll`)

| Component | License | Notice |
|---|---|---|
| [CommonLibSSE-NG](https://github.com/alandtse/CommonLibVR/tree/ng) | GPL-3.0-or-later with the Modding Exception and the GPL-3.0 Linking Exception (its `EXCEPTIONS.md`) | Copyright the CommonLibSSE-NG authors; originally based on CommonLibSSE (MIT) |
| [spdlog](https://github.com/gabime/spdlog) | MIT | Copyright (c) 2016 - present, Gabi Melman and spdlog contributors |
| [{fmt}](https://github.com/fmtlib/fmt) (bundled in spdlog) | MIT | Copyright (c) 2012 - present, Victor Zverovich and {fmt} contributors |
| [rapidcsv](https://github.com/d99kris/rapidcsv) | BSD-3-Clause | Copyright (c) 2017, Kristofer Berggren |
| [DirectXTK](https://github.com/microsoft/DirectXTK) (SimpleMath only) | MIT | Copyright (c) Microsoft Corporation |

The plugin uses Microsoft's C/C++ runtime and the Windows SDK (including DirectXMath) as
**system libraries** (GPL-3.0 §1). Chiefrim doesn't distribute them.

## Halo (built from halo-ce-universal)

| Component | License | Notice |
|---|---|---|
| [halo-ce-universal](https://github.com/cybersecurity/halo-ce-universal) (the decompilation and its ports) | CC0-1.0 | Dedicated to the public domain by its authors |
| expat | MIT | Copyright (c) 1998-2000 Thai Open Source Software Center Ltd and Clark Cooper; Copyright (c) 2001-2025 Expat maintainers |
| kcp | MIT | Copyright (c) 2017 Lin Wei |
| Mbed TLS | Apache-2.0 (elected from Apache-2.0 OR GPL-2.0-or-later) | Copyright The Mbed TLS Contributors |
| miniupnpc | BSD-3-Clause | Copyright (c) 2005-2025, Thomas BERNARD |
| Monocypher | CC0-1.0 (elected from BSD-2-Clause OR CC0-1.0) | Copyright (c) 2017-2023, Loup Vaillant |
| musl (math functions) | MIT | Copyright (c) 2005-2020 Rich Felker, et al. |
| stb | MIT (elected from MIT OR Unlicense) | Copyright (c) 2017 Sean Barrett |
| tomlc17 | MIT | Copyright (c) 2024-2026, CK Tan |
| zlib | Zlib | Copyright (C) 1995-1998 Jean-loup Gailly and Mark Adler |
| libtiff | libtiff | Copyright (c) 1988-1992 Sam Leffler; Copyright (c) 1991-1992 Silicon Graphics, Inc. |
| Overpass (font, embedded as data) | OFL-1.1 | Copyright 2021 The Overpass Project Authors |
| OpenCE (font, embedded as data) | OFL-1.1 | Copyright 2026 The OpenCE Project Authors |
| Newtown (font, embedded as data) | Public domain | Roger White (1994) |

**Not included:** extract-xiso. Its license is the original 4-clause BSD license, which is
incompatible with the GPL. Chiefrim's Halo build replaces the port's disc-image reader
(`port/linux/src/xiso.c`, which follows extract-xiso) with its own stub
(`Chiefrim/halo/overrides`).

Halo links the user's own SDL3, glibc, OpenGL and audio libraries as system libraries.

## Design reference

Chiefrim's architecture follows [SkyCraft](https://github.com/chasmlol/SkyCraft) (MIT,
Copyright (c) 2026 chasmlol). Parts of `Chiefrim/skse/src` are adapted from SkyCraft's SKSE
plugin:
- the log setup and the `PlayerCharacter::Update` hook;
- the camera hooks (`Camera.cpp`);
- the Havok collision harvesting (`Collision.cpp`): gathering the world's bodies, walking their
  shape trees, primitives as triangles, and the fault guards.
- the compositor's Present hook and state handling (`Overlay.cpp`), and the depth copy after
  `Main::RenderWorld`;
- combat (`Combat.cpp`): the nearby actors, hits through Skyrim's own hit processing, the player's
  damage refunded and sent on, and the player killed when the other game's dies.

SkyCraft's license:

> MIT License
>
> Copyright (c) 2026 chasmlol
>
> Permission is hereby granted, free of charge, to any person obtaining a copy of this software
> and associated documentation files (the "Software"), to deal in the Software without
> restriction, including without limitation the rights to use, copy, modify, merge, publish,
> distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
> Software is furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all copies or
> substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
> BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
> NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
> DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
