#!/bin/sh
set -eu

cd "$(dirname "$0")/.."
tag=$(git describe --tags --exact-match HEAD)
printf '%s' "$tag" | grep -Eq '^v[0-9]+\.[0-9]+\.[0-9]+-beta\.[0-9]+$' || {
  echo "Package from a public beta tag (vX.Y.Z-beta.N)." >&2; exit 1;
}
test -z "$(git status --porcelain)" || {
  echo "Commit the public source before packaging." >&2; exit 1;
}
test "$(uname -s)" = Darwin && test "$(uname -m)" = arm64 || {
  echo "This package is for macOS Apple Silicon." >&2; exit 1;
}
for file in build/cobra piet/native/build/piet_native; do
  test -x "$file" || { echo "Missing $file; run sh scripts/build-macos.sh first." >&2; exit 1; }
  test "$(lipo -archs "$file")" = arm64 || { echo "Wrong architecture: $file" >&2; exit 1; }
  test "$(vtool -show-build "$file" | awk '$1 == "minos" { print $2 }')" = 13.3 || {
    echo "Wrong macOS deployment target: $file; rebuild with scripts/build-macos.sh." >&2; exit 1;
  }
  if strings "$file" | grep -F -q "$(pwd -P)"; then
    echo "Local build path found in $file; rebuild with scripts/build-macos.sh." >&2; exit 1;
  fi
done
for file in LICENSE.txt NOTICE.txt THIRD_PARTY_NOTICES.txt piet/packaging/sample/synthetic_panel.csv; do
  test -s "$file" || { echo "Missing distribution file: $file" >&2; exit 1; }
done

tmp=$(mktemp -d "${TMPDIR:-/tmp}/piet-dmg.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
app="$tmp/image/PIET.app"
resources="$app/Contents/Resources"
mkdir -p "$app/Contents/MacOS" "$resources/sample" dist
cp piet/native/build/piet_native "$app/Contents/MacOS/piet_native"
cp build/cobra "$resources/cobra"
cp -R piet/frontend "$resources/frontend"
cp piet/packaging/sample/synthetic_panel.csv "$resources/sample/"
cp LICENSE.txt NOTICE.txt THIRD_PARTY_NOTICES.txt "$resources/"

python3 - "$app/Contents/Info.plist" "$tag" "$app/Contents/MacOS/piet_native" "$resources/cobra" <<'PY'
import json
import plistlib
import subprocess
import sys

path, tag, piet, cobra = sys.argv[1:]
version, revision = tag[1:].split("-beta.", 1)
major, minor, _ = version.split(".")
native = json.loads(subprocess.check_output([piet, "--version"]))
engine = json.loads(subprocess.check_output([cobra, "version", "--json"]))
if native["piet_version"] != tag[1:]:
    raise SystemExit("The app version does not match the release tag")
info = {
    "CFBundleDisplayName": "PIET",
    "CFBundleExecutable": "piet_native",
    "CFBundleIdentifier": "dev.piet.app",
    "CFBundleInfoDictionaryVersion": "6.0",
    "CFBundleName": "PIET",
    "CFBundlePackageType": "APPL",
    "CFBundleShortVersionString": version,
    "CFBundleVersion": f"{major}.{minor}.{revision}",
    "LSMinimumSystemVersion": "13.3",
    "NSHighResolutionCapable": True,
    "PIETPublicBetaTag": tag,
    "PIETReleaseTuple": {
        "piet_version": native["piet_version"],
        "cobra_version": engine["cobra_version"],
        "contract_version_A": native["contract_a"],
        "contract_version_B": native["contract_b"],
    },
}
with open(path, "wb") as output:
    plistlib.dump(info, output)
PY

codesign --force --deep --sign - "$app"
codesign --verify --deep --strict "$app"
ln -s /Applications "$tmp/image/Applications"
name="PIET-${tag#v}-macos-arm64.dmg"
hdiutil create -quiet -volname "PIET Beta" -srcfolder "$tmp/image" -format UDZO -ov "dist/$name"
(cd dist && shasum -a 256 "$name" > "$name.sha256")
echo "Built dist/$name"
