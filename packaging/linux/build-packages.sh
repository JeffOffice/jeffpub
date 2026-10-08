#!/usr/bin/env bash
# Builds the Linux packages from a finished build: an AppImage and a .deb.
# Both carry their own copy of Qt, so they run on any recent Debian or
# Ubuntu whatever Qt version the system has.
#
#   packaging/linux/build-packages.sh <build dir> <output dir>
#
# Needs QMAKE pointing at Qt's qmake, and linuxdeploy plus
# linuxdeploy-plugin-qt on PATH (or LINUXDEPLOY set).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/../.." && pwd)
build=$(cd "$1" && pwd)
out=$(mkdir -p "$2" && cd "$2" && pwd)
ver=$(sed -n 's/^project(JeffPub VERSION \([0-9.]*\).*/\1/p' "$src/CMakeLists.txt")
arch=amd64
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# 1. AppDir: the program, its fonts and dictionaries, desktop entry and icon.
app=$work/AppDir
install -Dm755 "$build/JeffPub" "$app/usr/bin/JeffPub"
mkdir -p "$app/usr/share/jeffpub"
cp -r "$src/resources/fonts" "$src/resources/dict" "$app/usr/share/jeffpub/"
mkdir -p "$app/usr/share/doc/jeffpub"
{ printf 'JeffPub\nCopyright (c) 2026 JeffOffice LLC\nLicense: GPL-3.0 (below)\n\n'; cat "$src/LICENSE"; } > "$app/usr/share/doc/jeffpub/copyright"
install -Dm644 "$src/README.md" "$app/usr/share/doc/jeffpub/README.md"
# PDFium's license texts and those of the libraries built into it.
mkdir -p "$app/usr/share/doc/jeffpub/pdfium"
cp "$src/third_party/pdfium/licenses/"* "$app/usr/share/doc/jeffpub/pdfium/"
install -Dm644 "$here/jeffpub.desktop" "$app/usr/share/applications/jeffpub.desktop"
install -Dm644 "$src/resources/app.png" "$app/usr/share/icons/hicolor/256x256/apps/jeffpub.png"
install -Dm644 "$here/jeffpub-mime.xml" "$app/usr/share/mime/packages/jeffpub.xml"

# The desktop entry runs "jeffpub".
ln -s JeffPub "$app/usr/bin/jeffpub"

# 2. linuxdeploy copies Qt, its plugins and the other libraries the program
#    needs (PDFium among them) into the AppDir, then writes the AppImage.
ld=${LINUXDEPLOY:-linuxdeploy}
export EXTRA_QT_MODULES="svg"
# Wayland desktops, and offscreen for command-line PDF export without a display.
export EXTRA_PLATFORM_PLUGINS="libqoffscreen.so;libqwayland-egl.so;libqwayland-generic.so"
( cd "$out" && LDAI_OUTPUT="JeffPub-$ver-x86_64.AppImage" "$ld" --appdir "$app" \
    --executable "$app/usr/bin/JeffPub" \
    --library "$build/_deps/pdfium/lib/libpdfium.so" \
    --desktop-file "$app/usr/share/applications/jeffpub.desktop" \
    --icon-file "$app/usr/share/icons/hicolor/256x256/apps/jeffpub.png" \
    --plugin qt --output appimage )

# 3. .deb: the same tree under /opt/jeffpub, a command in /usr/bin, and
#    the desktop entry, icon and file type where the desktop finds them.
deb=$work/deb
mkdir -p "$deb/opt/jeffpub" "$deb/usr/bin" "$deb/DEBIAN"
cp -a "$app/usr/." "$deb/opt/jeffpub/"
rm -f "$deb/opt/jeffpub/bin/jeffpub"
mkdir -p "$deb/usr/share"
mv "$deb/opt/jeffpub/share/applications" "$deb/opt/jeffpub/share/icons" "$deb/opt/jeffpub/share/mime" "$deb/usr/share/"
mkdir -p "$deb/usr/share/doc"
mv "$deb/opt/jeffpub/share/doc/jeffpub" "$deb/usr/share/doc/"
ln -s /opt/jeffpub/bin/JeffPub "$deb/usr/bin/jeffpub"
size=$(du -sk "$deb" | cut -f1)
# JeffPub was called JeffPub 79 (package jeffpub79) through 0.5.0; this
# package replaces that one.
cat > "$deb/DEBIAN/control" <<CONTROL
Package: jeffpub
Version: $ver
Section: graphics
Priority: optional
Architecture: $arch
Installed-Size: $size
Depends: libc6 (>= 2.35), libstdc++6, libgl1, libegl1, libfontconfig1, libfreetype6, libdbus-1-3, libxkbcommon0, libxkbcommon-x11-0, libx11-6, libx11-xcb1, libxcb1, libxcb-cursor0, libxcb-icccm4, libxcb-image0, libxcb-keysyms1, libxcb-randr0, libxcb-render-util0, libxcb-shape0, libxcb-xinerama0, libxcb-xkb1, libwayland-client0, libwayland-cursor0, libwayland-egl1, libglib2.0-0 | libglib2.0-0t64
Conflicts: jeffpub79
Replaces: jeffpub79
Provides: jeffpub79
Maintainer: JeffOffice LLC <https://github.com/JeffOffice/jeffpub>
Homepage: https://github.com/JeffOffice/jeffpub
Description: Desktop publishing: flyers, newsletters, brochures and cards
 JeffPub is a free, open-source desktop publishing program. It lays out
 text boxes, pictures, shapes and tables on the page, comes with templates,
 color and font schemes, mail merge and PDF export, and opens .pub files.
CONTROL
dpkg-deb --build --root-owner-group "$deb" "$out/jeffpub_${ver}_${arch}.deb"
ls -la "$out"
