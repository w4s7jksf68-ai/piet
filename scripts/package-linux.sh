#!/bin/sh
# Package a scripts/build-linux.sh build as a relocatable tar.xz and a .deb.
# Also needs dpkg-dev, file and xz-utils. Both packages use system libraries;
# no WebKit/GTK copies or network access are needed by the installed app.
set -eu
cd "$(dirname "$0")/.."
test "$(uname -s)" = Linux || { echo 'Run this script on Linux.' >&2; exit 1; }
app="$PWD/piet/native/build/piet_native"
engine="$PWD/build/cobra"
test -x "$app" && test -x "$engine" || { echo 'Run scripts/build-linux.sh first.' >&2; exit 1; }
version=$("$app" --version | python3 -c 'import json,sys; print(json.load(sys.stdin)["piet_version"])')
case "$version" in
  A.*) deb_version="1.${version#A.}" ;;
  *) echo "Unsupported package version: $version" >&2; exit 1 ;;
esac
arch=$(dpkg --print-architecture)
case "$arch" in
  amd64) label=x64 ;;
  arm64) label=arm64 ;;
  *) echo "Unsupported architecture: $arch" >&2; exit 1 ;;
esac
out="$PWD/dist/linux-$arch"
name="PIET-$version-linux-$label"
staged="$out/$name"
rm -rf "$staged" "$out/deb-root" "$out/debian"
mkdir -p "$staged/resources/frontend" "$staged/resources/sample" "$staged/demo"
strip -o "$staged/PIET" "$app"
strip -o "$staged/resources/cobra" "$engine"
cp piet/frontend/index.html "$staged/resources/frontend/"
cp -R piet/frontend/css piet/frontend/js "$staged/resources/frontend/"
cp piet/packaging/sample/fatalities.csv "$staged/resources/sample/"
cp LICENSE.txt NOTICE.txt "$staged/"
for file in fatalities_tour.do pangram_tour.do HANDBOOK.md fatalities.csv; do
  cp "demos/fatalities/$file" "$staged/demo/"
done
if [ -f demos/fatalities/사용법.md ]; then
  cp demos/fatalities/사용법.md "$staged/demo/PANGRAM_GUIDE_KO.md"
fi
cat piet/packaging/licenses/[0-9][0-9]-*.txt > "$staged/resources/THIRD_PARTY_NOTICES.txt"
cp "$staged/resources/THIRD_PARTY_NOTICES.txt" "$staged/"
# Let dpkg derive the minimum versions from the actual ELF symbols used.
mkdir -p "$out/debian"
cat > "$out/debian/control" <<'EOF'
Source: piet
Section: science
Priority: optional
Maintainer: M.K. Kim <criskim1997@users.noreply.github.com>

Package: piet
Architecture: any
Description: Offline econometrics desktop application
EOF
deps=$(cd "$out" && dpkg-shlibdeps -O -e"$staged/PIET" -e"$staged/resources/cobra" | sed -n 's/^shlibs:Depends=//p')
test -n "$deps"
cat > "$staged/README-LINUX.txt" <<EOF
PIET $version / Linux $label

Ubuntu 24.04 or newer. Requires GTK 3, WebKitGTK 4.1, OpenBLAS and LAPACKE.
On Ubuntu/Debian, install dependencies with:
  sudo apt install libwebkit2gtk-4.1-0 libgtk-3-0 libopenblas0 liblapacke

Extract this entire folder and run ./PIET from a desktop session.
Keep resources/ beside PIET; no source checkout or Python is needed.
Copy demo/ to a writable folder before using or modifying its examples.
The .deb installs to /opt/piet and adds a PIET application-menu entry.

Exact library requirements from this build:
$deps

License: GPL-3.0-or-later (LICENSE.txt). See THIRD_PARTY_NOTICES.txt.
Source and build instructions: https://github.com/w4s7jksf68-ai/piet
EOF
debroot="$out/deb-root"
mkdir -p "$debroot/opt/piet" "$debroot/usr/bin" "$debroot/usr/share/applications" "$debroot/DEBIAN"
cp -R "$staged/." "$debroot/opt/piet/"
ln -s /opt/piet/PIET "$debroot/usr/bin/piet"
cat > "$debroot/usr/share/applications/piet.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=PIET
Comment=Offline econometrics with Stata-compatible syntax
Exec=piet
Icon=applications-science
Terminal=false
Categories=Education;Science;Math;
StartupWMClass=PIET
EOF
cat > "$debroot/DEBIAN/control" <<EOF
Package: piet
Version: $deb_version
Architecture: $arch
Maintainer: M.K. Kim <criskim1997@users.noreply.github.com>
Section: science
Priority: optional
Depends: $deps
Installed-Size: $(du -sk "$debroot/opt/piet" | cut -f1)
Homepage: https://github.com/w4s7jksf68-ai/piet
Description: Offline econometrics desktop application
 PIET $version with the CoBrA engine, sample data and teaching examples.
EOF
dpkg-deb --root-owner-group --build "$debroot" "$out/$name.deb"
tar -cJf "$out/$name.tar.xz" -C "$out" "$name"
(cd "$out" && sha256sum "$name.deb" > "$name.deb.sha256" && sha256sum "$name.tar.xz" > "$name.tar.xz.sha256")
echo "Packages: $out/$name.deb and $out/$name.tar.xz"
