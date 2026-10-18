#!/usr/bin/env bash
# builds release packages into dist/: .deb, .tar.gz and an AppImage
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION=$(sed -n 's/^project(Clipper VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
VERSION+=$(sed -n 's/^set(CLIPPER_VERSION_SUFFIX "\([^"]*\)").*/\1/p' CMakeLists.txt)  # 0.1.0-beta
ARCH=x86_64
DIST=$PWD/dist
JOBS=${JOBS:-2}  # full parallel builds run out of memory on 8 GB
rm -rf "$DIST" && mkdir -p "$DIST"

cmake --preset release
cmake --build --preset release -j "$JOBS"

# .deb
(cd build/release && cpack -G DEB -B "$DIST/cpack" >/dev/null)
mv "$DIST"/cpack/*.deb "$DIST/"
rm -rf "$DIST/cpack"

# .tar.gz (copy bin/ + share/ into ~/.local)
STAGE="$DIST/clipper-$VERSION"
cmake --install build/release --prefix "$STAGE" --strip >/dev/null
cp LICENSE README.md CHANGELOG.md "$STAGE/"
tar -C "$DIST" -czf "$DIST/clipper-$VERSION-linux-$ARCH.tar.gz" "clipper-$VERSION"
rm -rf "$STAGE"

# AppImage via linuxdeploy (pinned), bundles the non-system libs
LINUXDEPLOY_VERSION=1-alpha-20251107-1
TOOLS=build/tools
mkdir -p "$TOOLS"
if [ ! -x "$TOOLS/linuxdeploy-$ARCH.AppImage" ]; then
    curl -fsSL -o "$TOOLS/linuxdeploy-$ARCH.AppImage" \
        "https://github.com/linuxdeploy/linuxdeploy/releases/download/$LINUXDEPLOY_VERSION/linuxdeploy-$ARCH.AppImage"
    chmod +x "$TOOLS/linuxdeploy-$ARCH.AppImage"
fi
APPDIR=build/AppDir
rm -rf "$APPDIR"
DESTDIR=$APPDIR cmake --install build/release --prefix /usr --strip >/dev/null
# --appimage-extract-and-run: works without fuse
(cd "$DIST" && LDAI_OUTPUT="Clipper-$VERSION-$ARCH.AppImage" \
    "$OLDPWD/$TOOLS/linuxdeploy-$ARCH.AppImage" --appimage-extract-and-run \
    --appdir "$OLDPWD/$APPDIR" --output appimage >/dev/null)

(cd "$DIST" && sha256sum ./*.deb ./*.tar.gz ./*.AppImage > SHA256SUMS)
ls -la "$DIST"
