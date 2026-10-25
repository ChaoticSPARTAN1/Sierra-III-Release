#!/usr/bin/env bash
# Builds the bake bundle's native tools from source: the port's Invader fork
# (invader-build, invader-extract, invader-info) and atracdenc, the encoder
# for the XMB menu's sound. Each is linked statically against every library
# it uses, so a bundle needs nothing installed beyond the system's own C and
# C++ runtime: no LD_LIBRARY_PATH, no DLLs to find.
#
#   build_tools.sh <work folder> <out folder>
#
# <out folder> gets:
#   invader/bin/        invader-build, invader-extract, invader-info (.exe on Windows)
#   invader/definitions Invader's tag definitions (the bake's map readers use them)
#   atracdenc/bin/      atracdenc (.exe on Windows)
#   source/             the full source of the GPL and LGPL pieces, as built
#   licenses/           each component's own licence texts (assemble.py lists them)
#
# Runs on Linux (gcc), macOS (clang) and Windows under MSYS2's UCRT64 shell
# (gcc). On macOS it builds arm64 and x86_64 one after the other and joins
# them into universal binaries (Corrosion, which builds Invader's Rust part,
# makes one architecture at a time). HOST=x86_64-w64-mingw32 cross-builds the
# Windows tools on Linux (mingw-w64 and Rust's x86_64-pc-windows-gnu target).
# environment:
#   JOBS      parallel jobs (default: every core)
#   MACARCH   macOS: build this one architecture only (arm64 or x86_64)
#
# Needs: bash, git, curl, tar (with xz), cmake 3.15+, ninja, a C11/C++20
# compiler, Python 3, and Rust (cargo, rustc). No game data is read or written.
set -euo pipefail

WORK=${1:?usage: build_tools.sh <work folder> <out folder>}
OUT=${2:?usage: build_tools.sh <work folder> <out folder>}
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
mkdir -p "$WORK" "$OUT"
WORK=$(cd "$WORK" && pwd); OUT=$(cd "$OUT" && pwd)
say() { echo "== build_tools: $*"; }

# What is built, pinned: the git sources by commit (upstream/pins.env), the
# tarballs by SHA-256
# shellcheck disable=SC1091
. "$REPO/upstream/pins.env"
RIAT_URL=https://github.com/SnowyMouse/riat
TARBALLS=(
  # name            file                       sha256                                                             urls
  "zlib            zlib-1.3.1.tar.gz          9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23 https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz"
  "libogg          libogg-1.3.5.tar.gz        0eb4b4b9420a0f51db142ba3f9c64b333f826532dc0f48c6410ae51f4799b664 https://github.com/xiph/ogg/releases/download/v1.3.5/libogg-1.3.5.tar.gz"
  "libvorbis       libvorbis-1.3.7.tar.gz     0e982409a9c3fc82ee06e08205b1355e5c6aa4c36bca58146ef399621b0ce5ab https://github.com/xiph/vorbis/releases/download/v1.3.7/libvorbis-1.3.7.tar.gz"
  "flac            flac-1.4.3.tar.xz          6c58e69cd22348f441b861092b825e591d0b822e106de6eb0ee4d05d27205b70 https://github.com/xiph/flac/releases/download/1.4.3/flac-1.4.3.tar.xz"
  "libsamplerate   libsamplerate-0.2.2.tar.xz 3258da280511d24b49d6b08615bbe824d0cacc9842b0e4caf11c52cf2b043893 https://github.com/libsndfile/libsamplerate/releases/download/0.2.2/libsamplerate-0.2.2.tar.xz"
  "libsquish       libsquish-1.15.tgz         628796eeba608866183a61d080d46967c9dda6723bc0a3ec52324c85d2147269 https://downloads.sourceforge.net/project/libsquish/libsquish-1.15.tgz http://archive.ubuntu.com/ubuntu/pool/universe/libs/libsquish/libsquish_1.15.orig.tar.gz"
  "libsndfile      libsndfile-1.2.2.tar.xz    3799ca9924d3125038880367bf1468e53a1b7e3686a934f098b7e1d286cdb80e https://github.com/libsndfile/libsndfile/releases/download/1.2.2/libsndfile-1.2.2.tar.xz"
)

# --- the platform ---------------------------------------------------------
OS=$(uname -s)
HOST=${HOST:-}
EXE=""
PLATFORM=()          # cmake arguments every build takes
RUNTIME=""           # linker flags that take the C++ runtime in statically
RUST_TARGET=""       # Rust's target, when it isn't this machine's
RUN=""               # how to run a built tool here: wine for a Windows cross build, arch for the other Mac half

if [[ $OS == Darwin && -z ${MACARCH:-} ]]; then
  # both architectures, then one universal binary of each tool
  for arch in arm64 x86_64; do
    MACARCH=$arch "$BASH" "$HERE/build_tools.sh" "$WORK/$arch" "$WORK/out-$arch"
  done
  say "universal binaries into $OUT"
  rm -rf "${OUT:?}"/*
  cp -R "$WORK/out-arm64/." "$OUT/"
  for f in invader/bin/invader-build invader/bin/invader-extract invader/bin/invader-info atracdenc/bin/atracdenc; do
    lipo -create "$WORK/out-arm64/$f" "$WORK/out-x86_64/$f" -output "$OUT/$f"
    codesign --force -s - "$OUT/$f"
    lipo -info "$OUT/$f"
  done
  "$OUT/invader/bin/invader-info" --help >/dev/null && "$OUT/atracdenc/bin/atracdenc" --help >/dev/null
  if arch -x86_64 /usr/bin/true 2>/dev/null; then
    arch -x86_64 "$OUT/invader/bin/invader-info" --help >/dev/null && arch -x86_64 "$OUT/atracdenc/bin/atracdenc" --help >/dev/null
    say "both architectures run (x86_64 under Rosetta)"
  else
    say "arm64 runs; no Rosetta here to run the x86_64 half"
  fi
  du -sh "$OUT"/invader/bin/* "$OUT"/atracdenc/bin/* "$OUT/source"
  exit 0
fi

case "$HOST:$OS" in
  x86_64-w64-mingw32:Linux)
    TARGET=windows; EXE=.exe; RUST_TARGET=x86_64-pc-windows-gnu
    cat > "$WORK/mingw.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc-posix)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++-posix)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32 $WORK/deps)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF
    PLATFORM=(-DCMAKE_TOOLCHAIN_FILE="$WORK/mingw.cmake")
    RUNTIME="-static"
    if command -v wine >/dev/null; then RUN=wine; fi
    ;;
  :Linux)
    TARGET=linux
    RUNTIME="-static-libstdc++ -static-libgcc"
    ;;
  :Darwin)
    TARGET=macos
    PLATFORM=(-DCMAKE_OSX_ARCHITECTURES="$MACARCH" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0)
    [[ $MACARCH != "$(uname -m)" ]] && RUST_TARGET=$([[ $MACARCH == arm64 ]] && echo aarch64 || echo x86_64)-apple-darwin
    if [[ $MACARCH != "$(uname -m)" ]]; then
      # the other half runs here only under Rosetta; without it, it is built and not run
      if arch "-$MACARCH" /usr/bin/true 2>/dev/null; then RUN="arch -$MACARCH"; else HOST=$MACARCH-apple-darwin; fi
    fi
    ;;
  :MINGW*|:MSYS*)
    TARGET=windows; EXE=.exe
    RUNTIME="-static"
    ;;
  *) echo "build_tools: unknown platform $OS (HOST=$HOST)"; exit 1 ;;
esac
# CMake and the compilers are native Windows programs under MSYS2: they take
# C:/... paths, not MSYS2's /c/...
native() { if [[ $OS == MINGW* || $OS == MSYS* ]]; then cygpath -m "$1"; else echo "$1"; fi; }
JOBS=${JOBS:-$( (nproc || sysctl -n hw.ncpu) 2>/dev/null || echo 2)}
DL=$WORK/dl SRC=$WORK/src B=$WORK/build DEPS=$WORK/deps
mkdir -p "$DL" "$SRC" "$B" "$DEPS"
D=$(native "$DEPS")
COMMON=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ${PLATFORM[@]+"${PLATFORM[@]}"})
DEPARGS=("${COMMON[@]}" -DCMAKE_INSTALL_PREFIX="$D" -DCMAKE_PREFIX_PATH="$D" -DCMAKE_INSTALL_LIBDIR=lib
         -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_TESTING=OFF)
say "target $TARGET${MACARCH:+ $MACARCH}, $JOBS jobs, work in $WORK"

sha256() { if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }

fetch() {        # <file> <sha256> <url> [url ...]: into $DL, checked
  local file=$1 sum=$2; shift 2
  if [[ -f $DL/$file && $(sha256 "$DL/$file") == "$sum" ]]; then return; fi
  for url in "$@"; do
    rm -f "$DL/$file"
    if curl -fsSL --retry 3 -o "$DL/$file" "$url" && [[ $(sha256 "$DL/$file") == "$sum" ]]; then return; fi
    echo "build_tools: $url failed or didn't match its SHA-256"
  done
  rm -f "$DL/$file"; echo "build_tools: no good copy of $file"; exit 1
}

unpack() {       # <file> <folder>: a tarball into $SRC/<folder>, one level down
  rm -rf "${SRC:?}/$2"; mkdir -p "$SRC/$2"
  case $1 in
    libsquish-*) tar -xzf "$DL/$1" -C "$SRC/$2" ;;           # this one has no top folder
    *) tar -xf "$DL/$1" -C "$SRC/$2" --strip-components=1 ;;
  esac
}

git_at() {       # <url> <commit> <folder>: that commit alone, no history
  rm -rf "${SRC:?}/$3"; mkdir -p "$SRC/$3"
  git -C "$SRC/$3" init -q
  git -C "$SRC/$3" fetch -q --depth 1 "$1" "$2"
  git -C "$SRC/$3" -c advice.detachedHead=false checkout -q FETCH_HEAD
}

logged() {       # <log> <command ...>: quiet unless it fails
  local log=$1; shift
  "$@" >"$log" 2>&1 || { grep -n -E "error|Error" "$log" | head -30; tail -40 "$log"; echo "build_tools: failed: $*"; exit 1; }
}

dep() {          # <folder> [cmake args]: configure, build, install into $DEPS
  local name=$1; shift
  say "$name"
  logged "$B/$name.configure.log" cmake -S "$(native "$SRC/$name")" -B "$(native "$B/$name")" "${DEPARGS[@]}" "$@"
  logged "$B/$name.build.log" cmake --build "$(native "$B/$name")" -j "$JOBS"
  logged "$B/$name.install.log" cmake --install "$(native "$B/$name")"
}

# --- the libraries, static ------------------------------------------------
# A stamp of the pins: a work folder kept between runs (a CI cache) rebuilds
# the libraries only when they change
STAMP=$(printf '%s\n' "$TARGET" "${TARBALLS[@]}" ${PLATFORM[@]+"${PLATFORM[@]}"} | (sha256sum 2>/dev/null || shasum -a 256) | cut -c1-16)
for line in "${TARBALLS[@]}"; do
  read -r name file sum urls <<<"$line"
  # shellcheck disable=SC2086
  fetch "$file" "$sum" $urls
done
if [[ -f $DEPS/.built-$STAMP ]]; then
  say "libraries: kept from an earlier run ($STAMP)"
  for line in "${TARBALLS[@]}"; do read -r name file _ <<<"$line"; [[ -d $SRC/$name ]] || unpack "$file" "$name"; done
else
  rm -rf "${DEPS:?}"/* "${DEPS:?}"/.built-*
  for line in "${TARBALLS[@]}"; do read -r name file _ <<<"$line"; unpack "$file" "$name"; done
  dep zlib
  # zlib's CMake always builds a shared library beside the static one; only the
  # static one is wanted, under the name every find_package(ZLIB) expects
  rm -f "$DEPS"/lib/libz.so* "$DEPS"/lib/libz.*dylib "$DEPS"/lib/libzlib.dll.a "$DEPS"/bin/libzlib*.dll "$DEPS"/bin/zlib*.dll
  [[ -f $DEPS/lib/libzlibstatic.a ]] && mv "$DEPS/lib/libzlibstatic.a" "$DEPS/lib/libz.a"
  dep libogg -DINSTALL_DOCS=OFF
  dep libvorbis
  dep flac -DBUILD_CXXLIBS=OFF -DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_DOCS=OFF -DINSTALL_MANPAGES=OFF \
      -DWITH_OGG=ON -DWITH_STACK_PROTECTOR=OFF -DWITH_FORTIFY_SOURCE=OFF
  dep libsamplerate -DLIBSAMPLERATE_EXAMPLES=OFF -DLIBSAMPLERATE_INSTALL=ON
  # no OpenMP (no libgomp to carry); SSE2 only on x86-64
  dep libsquish -DBUILD_SQUISH_WITH_OPENMP=OFF \
      -DBUILD_SQUISH_WITH_SSE2=$([[ ${MACARCH:-x86_64} == x86_64 ]] && echo ON || echo OFF)
  # WAV in and out is all the bake asks of it (snd0.py writes a 16-bit PCM
  # .wav): no external codecs, so no further libraries or licences
  dep libsndfile -DENABLE_EXTERNAL_LIBS=OFF -DENABLE_MPEG=OFF -DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF \
      -DENABLE_CPACK=OFF -DINSTALL_MANPAGES=OFF -DBUILD_REGTEST=OFF -DENABLE_PACKAGE_CONFIG=OFF
  touch "$DEPS/.built-$STAMP"
fi

# --- Invader: upstream at the pin, the port's fork patch, the build patch --
say "invader at ${INVADER_COMMIT:0:7}"
git_at "$INVADER_URL" "$INVADER_COMMIT" invader
rm -rf "$SRC/invader/ext/riat"
git_at "$RIAT_URL" "$INVADER_RIAT_COMMIT" invader/ext/riat
for patch in psp-fork.patch no-editor.patch; do
  git -C "$SRC/invader" apply --whitespace=nowarn "$REPO/upstream/invader/$patch"
done
INV_ARGS=("${COMMON[@]}" -DBUILD_SHARED_LIBS=OFF -DINVADER_EDIT_QT=OFF -DINVADER_USE_SYSTEM_CORROSION=OFF
  -DCMAKE_PREFIX_PATH="$D" -DZLIB_LIBRARY="$D/lib/libz.a" -DZLIB_INCLUDE_DIR="$D/include"
  -DCMAKE_DISABLE_FIND_PACKAGE_TIFF=ON -DCMAKE_DISABLE_FIND_PACKAGE_LibArchive=ON -DCMAKE_DISABLE_FIND_PACKAGE_Freetype=ON
  -DCMAKE_C_FLAGS="-I$D/include" -DCMAKE_CXX_FLAGS="-I$D/include -DFLAC__NO_DLL"
  -DCMAKE_EXE_LINKER_FLAGS="-L$D/lib $RUNTIME")
[[ -n $RUST_TARGET ]] && INV_ARGS+=(-DRust_CARGO_TARGET="$RUST_TARGET")
logged "$B/invader.configure.log" cmake -S "$(native "$SRC/invader")" -B "$(native "$B/invader")" "${INV_ARGS[@]}"
logged "$B/invader.build.log" cmake --build "$(native "$B/invader")" -j "$JOBS" --target invader-build invader-extract invader-info

# --- atracdenc, on a static libsndfile ------------------------------------
say "atracdenc at ${ATRACDENC_COMMIT:0:7}"
git_at "$ATRACDENC_URL" "$ATRACDENC_COMMIT" atracdenc
rm -rf "$SRC/atracdenc/src/lib/libgha"
git_at "$ATRACDENC_LIBGHA_URL" "$ATRACDENC_LIBGHA_COMMIT" atracdenc/src/lib/libgha
# its CMakeLists makes every macOS build universal; here each architecture is
# built on its own (above), so that line goes (the shipped source says so)
sed -i.orig '/set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")/d' "$SRC/atracdenc/CMakeLists.txt"
rm -f "$SRC/atracdenc/CMakeLists.txt.orig"
logged "$B/atracdenc.configure.log" cmake -S "$(native "$SRC/atracdenc")" -B "$(native "$B/atracdenc")" "${COMMON[@]}" \
  -DATRACDENC_PCM_IO_BACKEND=libsndfile -DSNDFILE_LIBRARY="$D/lib/libsndfile.a" -DLIBSNDFILE_INCLUDE_DIR="$D/include" \
  -DCMAKE_DISABLE_FIND_PACKAGE_GTest=ON -DCMAKE_EXE_LINKER_FLAGS="$RUNTIME"
logged "$B/atracdenc.build.log" cmake --build "$(native "$B/atracdenc")" -j "$JOBS" --target atracdenc

# --- out ------------------------------------------------------------------
say "into $OUT"
rm -rf "$OUT/invader" "$OUT/atracdenc" "$OUT/source" "$OUT/licenses"
mkdir -p "$OUT/invader/bin" "$OUT/invader/definitions" "$OUT/atracdenc/bin" "$OUT/source" "$OUT/licenses"
for tool in invader-build invader-extract invader-info; do
  cp "$(find "$B/invader" -maxdepth 2 -type f -name "$tool$EXE" | head -1)" "$OUT/invader/bin/"
done
cp "$(find "$B/atracdenc" -maxdepth 3 -type f -name "atracdenc$EXE" | head -1)" "$OUT/atracdenc/bin/"
for f in "$OUT"/invader/bin/* "$OUT"/atracdenc/bin/*; do
  case $TARGET in
    macos) strip -x "$f"; codesign --force -s - "$f" ;;     # stripping undoes the linker's ad-hoc signature
    windows) "${HOST:+$HOST-}strip" "$f" ;;
    *) strip "$f" ;;
  esac
done
cp "$SRC"/invader/src/tag/hek/definition/*.json "$SRC/invader/include/invader/hek/fourcc.hpp" "$OUT/invader/definitions/"

# The GPL and LGPL pieces' complete source, exactly as built, with the
# script that built them
tar -czf "$OUT/source/invader-src.tar.gz" -C "$SRC" --exclude=.git invader
tar -czf "$OUT/source/atracdenc-src.tar.gz" -C "$SRC" --exclude=.git atracdenc
cp "$DL/libsndfile-1.2.2.tar.xz" "$OUT/source/"
cp "$REPO/upstream/invader/psp-fork.patch" "$OUT/source/invader-psp-fork.patch"
cp "$REPO/upstream/invader/no-editor.patch" "$OUT/source/invader-no-editor.patch"
cp "$HERE/build_tools.sh" "$OUT/source/build_tools.sh"

# Each component's own licence texts
lic() { local name=$1; shift; mkdir -p "$OUT/licenses/$name"; cp "$@" "$OUT/licenses/$name/"; }
lic invader "$SRC/invader/LICENSE.md" "$SRC/invader/gplv3.txt" "$SRC/invader/src/sound/reclaimer-license.txt"
cp "$SRC/invader/src/sound/adpcm_xq/license.txt" "$OUT/licenses/invader/adpcm_xq-license.txt"
lic riat "$SRC/invader/ext/riat/LICENSE.txt" "$SRC/invader/ext/riat/COPYING"
lic atracdenc "$SRC/atracdenc/LICENSE"
lic libgha "$SRC/atracdenc/src/lib/libgha/LICENSE"
lic kissfft "$SRC/atracdenc/src/lib/libgha/src/3rd/kissfft/COPYING"
lic libsndfile "$SRC/libsndfile/COPYING"
lic zlib "$SRC/zlib/LICENSE"
lic libogg "$SRC/libogg/COPYING"
lic libvorbis "$SRC/libvorbis/COPYING"
lic flac "$SRC/flac/COPYING.Xiph"
lic libsamplerate "$SRC/libsamplerate/COPYING"
lic libsquish "$SRC/libsquish/LICENSE.txt"
# Rust's standard library is linked into Invader's script compiler (riat)
RUSTDOC=$(rustc --print sysroot)/share/doc/rust
RUSTLIC=$(find "$RUSTDOC" "$(rustc --print sysroot)/share/licenses" -maxdepth 2 -type f \
            \( -name 'COPYRIGHT-library.html' -o -name 'MIT.txt' -o -name 'Apache-2.0.txt' -o -name 'LICENSE-MIT' \
               -o -name 'LICENSE-APACHE' \) 2>/dev/null || true)
# shellcheck disable=SC2086
[[ -n $RUSTLIC ]] && lic rust-std $RUSTLIC
# The C++ runtime linked in statically (gcc's on Linux and Windows): its
# runtime library exception
if [[ $TARGET != macos ]]; then
  GCCEXC=$(find "$RUSTDOC" "${MINGW_PREFIX:-/nonexistent}/share/licenses" /usr/share/doc -maxdepth 3 -type f \
             \( -iname '*exception*3.1*' -o -name 'COPYING.RUNTIME' -o -iname 'RUNTIME.LIBRARY.EXCEPTION*' \) \
             2>/dev/null | head -1 || true)
  [[ -n $GCCEXC ]] && lic gcc-runtime "$GCCEXC"
fi
# On Windows, MinGW-w64's runtime and winpthreads too. They are matched by
# their package's own folder (the prefix itself, /ucrt64, holds "crt") and
# copied under its name, as several packages ship a file called LICENSE
if [[ $TARGET == windows ]]; then
  for dir in "${MINGW_PREFIX:-/nonexistent}"/share/licenses/* /usr/share/doc/mingw-w64-common; do
    case ${dir##*/} in *crt*|*winpthread*|mingw-w64-common) ;; *) continue ;; esac
    for f in "$dir"/*; do
      [[ -f $f && $f != *.gz ]] || continue
      mkdir -p "$OUT/licenses/mingw-w64"
      cp "$f" "$OUT/licenses/mingw-w64/${dir##*/}-${f##*/}"
    done
  done
fi
# The engine the PSP build runs (none of its code is here; CC0)
mkdir -p "$OUT/licenses/chupathingyce"
curl -fsSL --retry 3 -o "$OUT/licenses/chupathingyce/LICENSE.md" \
  "${ENGINE_CHUPA_URL/github.com/raw.githubusercontent.com}/$ENGINE_CHUPA_COMMIT/LICENSE.md" ||
  rm -rf "$OUT/licenses/chupathingyce"

# --- does it run ------------------------------------------------------------
if [[ -z $HOST || -n $RUN ]]; then
  for tool in invader-build invader-extract invader-info; do
    # shellcheck disable=SC2086
    logged "$B/run-$tool.log" $RUN "$OUT/invader/bin/$tool$EXE" --help
  done
  # shellcheck disable=SC2086
  logged "$B/run-atracdenc.log" $RUN "$OUT/atracdenc/bin/atracdenc$EXE" --help
  say "the tools run"
fi
du -sh "$OUT"/invader/bin/* "$OUT"/atracdenc/bin/* "$OUT/source"
