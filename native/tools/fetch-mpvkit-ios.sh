#!/usr/bin/env bash
# Fetch libmpv + its full static dependency stack for iOS from MPVKit (https://github.com/mpvkit/MPVKit)
# as *.xcframework bundles — every binary target of MPVKit's LGPL Libmpv product. Used by the `ios` job
# in .github/workflows/release.yml and for local builds (see native/docs/ios-port.md).
#
# Usage:  ./native/tools/fetch-mpvkit-ios.sh [dest-dir]      (default: ~/ios-deps/mpvkit)
set -euo pipefail

DEST="${1:-$HOME/ios-deps/mpvkit}"
MPVKIT="0.41.0-n8.1.2"   # MPVKit release: mpv 0.41.0 / ffmpeg n8.1.2 — keep in sync with its Package.swift

URLS=(
  "https://github.com/mpvkit/MPVKit/releases/download/$MPVKIT/Libmpv.xcframework.zip"
  "https://github.com/mpvkit/MPVKit/releases/download/$MPVKIT/Libavcodec.xcframework.zip"
  "https://github.com/mpvkit/MPVKit/releases/download/$MPVKIT/Libavdevice.xcframework.zip"
  "https://github.com/mpvkit/MPVKit/releases/download/$MPVKIT/Libavformat.xcframework.zip"
  "https://github.com/mpvkit/MPVKit/releases/download/$MPVKIT/Libavfilter.xcframework.zip"
  "https://github.com/mpvkit/MPVKit/releases/download/$MPVKIT/Libavutil.xcframework.zip"
  "https://github.com/mpvkit/MPVKit/releases/download/$MPVKIT/Libswresample.xcframework.zip"
  "https://github.com/mpvkit/MPVKit/releases/download/$MPVKIT/Libswscale.xcframework.zip"
  "https://github.com/mpvkit/openssl-build/releases/download/3.3.5/Libcrypto.xcframework.zip"
  "https://github.com/mpvkit/openssl-build/releases/download/3.3.5/Libssl.xcframework.zip"
  "https://github.com/mpvkit/gnutls-build/releases/download/3.8.11/gmp.xcframework.zip"
  "https://github.com/mpvkit/gnutls-build/releases/download/3.8.11/nettle.xcframework.zip"
  "https://github.com/mpvkit/gnutls-build/releases/download/3.8.11/hogweed.xcframework.zip"
  "https://github.com/mpvkit/gnutls-build/releases/download/3.8.11/gnutls.xcframework.zip"
  "https://github.com/mpvkit/libass-build/releases/download/0.17.5/Libunibreak.xcframework.zip"
  "https://github.com/mpvkit/libass-build/releases/download/0.17.5/Libfreetype.xcframework.zip"
  "https://github.com/mpvkit/libass-build/releases/download/0.17.5/Libfribidi.xcframework.zip"
  "https://github.com/mpvkit/libass-build/releases/download/0.17.5/Libharfbuzz.xcframework.zip"
  "https://github.com/mpvkit/libass-build/releases/download/0.17.5/Libass.xcframework.zip"
  "https://github.com/mpvkit/libsmbclient-build/releases/download/4.15.13-2512/Libsmbclient.xcframework.zip"
  "https://github.com/mpvkit/libbluray-build/releases/download/1.4.0/Libbluray.xcframework.zip"
  "https://github.com/mpvkit/libuavs3d-build/releases/download/1.2.1-xcode/Libuavs3d.xcframework.zip"
  "https://github.com/mpvkit/libdovi-build/releases/download/3.3.2/Libdovi.xcframework.zip"
  "https://github.com/mpvkit/moltenvk-build/releases/download/1.4.1/MoltenVK.xcframework.zip"
  "https://github.com/mpvkit/libshaderc-build/releases/download/2025.5.0/Libshaderc_combined.xcframework.zip"
  "https://github.com/mpvkit/lcms2-build/releases/download/2.17.0/lcms2.xcframework.zip"
  "https://github.com/mpvkit/libplacebo-build/releases/download/7.360.1/Libplacebo.xcframework.zip"
  "https://github.com/mpvkit/libdav1d-build/releases/download/1.5.2-xcode/Libdav1d.xcframework.zip"
  "https://github.com/mpvkit/libuchardet-build/releases/download/0.0.8-xcode/Libuchardet.xcframework.zip"
  "https://github.com/mpvkit/libluajit-build/releases/download/2.1.0-xcode/Libluajit.xcframework.zip"
)

mkdir -p "$DEST"
cd "$DEST"
for u in "${URLS[@]}"; do
  f="$(basename "$u")"
  fw="${f%.zip}"
  [ -d "$fw" ] && { echo "have  $fw"; continue; }
  echo "fetch $f"
  curl -fsSLO "$u"
  unzip -q -o "$f"
  rm -f "$f"
done
echo "OK: $(ls -d "$DEST"/*.xcframework | wc -l | tr -d ' ') xcframeworks in $DEST"
