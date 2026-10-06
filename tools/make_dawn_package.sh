#!/usr/bin/env bash
# ============================================================================
#  Builds the WinlatorXR *Dawn* package: the files that are extracted over an
#  existing COD4 installation plus a container profile Dawn can import, in the
#  same shape as the Far Cry and Crysis VR Dawn packages.
#
#    tools/make_dawn_package.sh <version> [out-dir]
#
#  Same payload as KisakCOD-VR-WinlatorXR-<version>.zip, but laid out to be
#  unzipped straight into D:\CallOfDuty4 rather than inside a top folder, and
#  with the .wxrprofile.json and README_DAWN.txt added.
#
#  Uses the llvm-mingw cross build in build-mingw-rel; text files come from
#  the committed tree, so commit before packaging.
# ============================================================================

set -euo pipefail

VERSION="${1:?usage: tools/make_dawn_package.sh <version> [out-dir]}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${2:-$REPO/release/out}"
NAME="KisakCOD-VR-WinlatorXR-Dawn-$VERSION"
PACKAGE_DIR="$OUT/$NAME"
ZIP_FILE="$OUT/$NAME.zip"
PROFILE="CallOfDuty4-VR - proton-9.0-x86_64 - Quest 3.wxrprofile.json"

cd "$REPO"

if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
    echo "Commit first: the package records the commit it was built from." >&2
    exit 1
fi

cmake --build build-mingw-rel --target KisakCOD-sp

rm -rf "$PACKAGE_DIR" "$ZIP_FILE" "$ZIP_FILE.sha256"
mkdir -p "$PACKAGE_DIR/miles"

cp bin/KisakCOD-sp.exe bin/mss32.dll bin/steam_api.dll \
    deps/binklib/binkw32.dll "$PACKAGE_DIR/"
cp bin/miles/* "$PACKAGE_DIR/miles/"

for f in Launch-KisakCOD-VR-WinlatorXR.bat CallOfDuty4-VR.desktop \
         Install-CoD4-Shortcut.bat; do
    git show "HEAD:release/winlatorxr/$f" > "$PACKAGE_DIR/$f"
done
git show HEAD:release/package/VR-Settings.bat > "$PACKAGE_DIR/VR-Settings.bat"
git show "HEAD:release/winlatorxr/dawn/$PROFILE" > "$PACKAGE_DIR/$PROFILE"
git show HEAD:release/winlatorxr/dawn/README_DAWN.txt \
    | sed 's/$/\r/' > "$PACKAGE_DIR/README_DAWN.txt"
git show HEAD:docs/WINLATORXR.md > "$PACKAGE_DIR/WINLATORXR.txt"
git show HEAD:CONTROLS.md > "$PACKAGE_DIR/CONTROLS.txt"
git show HEAD:KNOWN-ISSUES.md > "$PACKAGE_DIR/KNOWN-ISSUES.txt"
git show HEAD:THIRD-PARTY-NOTICES.md > "$PACKAGE_DIR/THIRD-PARTY-NOTICES.txt"
git show HEAD:LICENSE > "$PACKAGE_DIR/LICENSE-GPLv3.txt"
git rev-parse HEAD > "$PACKAGE_DIR/SOURCE.txt"

python3 - "$PACKAGE_DIR" "$ZIP_FILE" <<'EOF'
import os
import sys
import zipfile

package_dir, zip_file = sys.argv[1], sys.argv[2]
count = 0
with zipfile.ZipFile(zip_file, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for root, dirs, files in os.walk(package_dir):
        dirs.sort()
        for name in sorted(files):
            path = os.path.join(root, name)
            z.write(path, os.path.relpath(path, package_dir))
            count += 1
print('zip: %s (%d files)' % (zip_file, count))
EOF

(cd "$OUT" && sha256sum "$NAME.zip" > "$NAME.zip.sha256")

echo
echo "Package: $PACKAGE_DIR"
echo "Zip:     $ZIP_FILE"
