#!/usr/bin/env bash
# Builds release executables with the network embedded and zips them with the README.
# Usage: tools/release.sh <version> <net.nnue>     e.g. tools/release.sh 1.0 nets/run1/epoch-10.nnue
# Output: release/Hypernova-<version>/ and release/Hypernova-<version>-windows.zip
set -euo pipefail
cd "$(dirname "$0")/.."
version=$1
net=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
out=release/Hypernova-$version
rm -rf "$out" && mkdir -p "$out"

# x86-64-v3: AVX2 CPUs (2015+), the build CCRL tests. x86-64-v2: older CPUs with POPCNT (2009+).
for arch in x86-64-v3 x86-64-v2; do
  build=cpp/build-release-$arch
  cmake -S cpp -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSN_ARCH=$arch -DSN_NET="$net" \
    -DCMAKE_CXX_FLAGS="-DSN_VERSION=\\\"$version\\\"" >/dev/null
  cmake --build "$build" --target supernova supernova_tests >/dev/null
  "$build/supernova_tests.exe" --quick >/dev/null
  suffix=$([ $arch = x86-64-v3 ] && echo avx2 || echo popcnt)
  cp "$build/supernova.exe" "$out/Hypernova-$version-$suffix.exe"
done

cp README.md "$out/README.md"
cp LICENSE "$out/LICENSE"
# Source: tracked files only (no build output, data or local paths), from the committed state.
git archive --format=zip --prefix="Hypernova-$version-source/" -o "release/Hypernova-$version-source.zip" HEAD   cpp train tools datagen.js engine.js LICENSE README.md
cp "$net" "$out/hypernova-$version.nnue"
(cd release && rm -f "Hypernova-$version-windows.zip" && powershell -NoProfile -Command \
  "Compress-Archive -Path 'Hypernova-$version' -DestinationPath 'Hypernova-$version-windows.zip'")
for exe in "$out"/*.exe; do
  echo "$(basename "$exe"): $("$exe" bench quit | tail -1)"
done
ls -la release
