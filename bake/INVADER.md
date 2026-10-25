# Building the bundle's tools from source

The bundle's programs (the port's Invader and atracdenc) are built from the
source in `source/`, by `source/build_tools.sh`, the same script that made
them. To change one, or to build them for a system the bundle doesn't cover:

```
bash source/build_tools.sh work tools
```

It fetches the pinned sources (Invader d43ce55 with the port's patches,
atracdenc cb17070, and the libraries, each checked by its SHA-256), builds
them, and writes `tools/invader/bin` and `tools/atracdenc/bin`: copy those over
the bundle's. It needs bash, git, curl, CMake 3.15 or later, Ninja, a C11 and
C++20 compiler, Python 3 and Rust (cargo). It runs on Linux, macOS, and on
Windows in MSYS2's UCRT64 shell.

`source/invader-src.tar.gz` is the exact source the bundle's Invader was built
from (upstream Invader with both patches applied, Rat In a Tube included);
`source/atracdenc-src.tar.gz` and `source/libsndfile-1.2.2.tar.xz` are
atracdenc's and libsndfile's.

The fork adds to upstream Invader (SnowyMouse/invader, commit d43ce55):
`-X` (tag address), `-G` (model and level geometry to a separate pack), `-K`
(keyframed animations), packed collision, Windows-1252 script sources, and
tag IDs in `-w`'s index (a line may end in a tab and the tag's ID in hex: the
multiplayer maps keep the disc map's IDs, salts included).
`invader-psp-fork.patch` is the whole change; `invader-no-editor.patch` only
lets Invader build without Qt6 and SDL2, which its tag editor alone needs.
