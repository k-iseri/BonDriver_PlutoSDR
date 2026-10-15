#!/bin/bash
# recpluto / isdbtd の配布 tar.gz を dist/ に作る（Linux / macOS。GitHub Actions のリリースとローカル確認の両方で使う）。
#   tools/package_recpluto.sh <version> <label> <rt build dir> <libiio の install prefix>
#   例: tools/package_recpluto.sh v1.5.1 linux-x86_64 rt/build/rt-native $PWD/iio-prefix
# 前提: rt を -DISDBT_BUNDLE_RPATH=ON でビルド済み（実行ファイルの隣の lib/ を rpath で探す）。
# libiio（LGPL-2.1+）は共有ライブラリのまま lib/ に同梱する（静的リンクにしない）。libusb / libxml2 は
# Linux では OS のパッケージ（libusb-1.0-0, libxml2）を使い、macOS は libusb を同梱、libxml2 は OS 付属のものを使う。
set -eu
VERSION=$1; LABEL=$2; BUILD=$3; PREFIX=$4
REPO=$(cd "$(dirname "$0")/.." && pwd)
NAME="recpluto-$VERSION-$LABEL"
STAGE="$REPO/dist/$NAME"
rm -rf "$STAGE" && mkdir -p "$STAGE/lib" "$STAGE/mirakurun"
cp "$BUILD/recpluto" "$BUILD/isdbtd" "$STAGE/"
cp "$REPO/LICENSE" "$STAGE/"
cp "$REPO/tools/mirakurun/"*.sample "$REPO/tools/mirakurun/99-plutosdr.rules" "$REPO/tools/mirakurun/ch2_to_channels.py" "$STAGE/mirakurun/"
cp "$REPO/docs/mirakurun.md" "$STAGE/"

OS=$(uname -s)
if [ "$OS" = Darwin ]; then
  # libiio の dylib と、それが依存する Homebrew の libusb を同梱し、参照を相対にする
  LIBIIO=$(ls "$PREFIX"/lib/libiio.*.dylib | grep -v '\.1\.dylib$' | head -1)   # libiio.1.0.0.dylib
  cp "$LIBIIO" "$STAGE/lib/libiio.1.dylib"
  LIBUSB=$(otool -L "$STAGE/lib/libiio.1.dylib" | awk '/libusb-1.0/ {print $1}' | head -1)
  if [ -n "$LIBUSB" ]; then
    cp "$(readlink -f "$LIBUSB")" "$STAGE/lib/libusb-1.0.0.dylib"
    install_name_tool -id @rpath/libusb-1.0.0.dylib "$STAGE/lib/libusb-1.0.0.dylib"
    install_name_tool -change "$LIBUSB" @loader_path/libusb-1.0.0.dylib "$STAGE/lib/libiio.1.dylib"
  fi
  install_name_tool -id @rpath/libiio.1.dylib "$STAGE/lib/libiio.1.dylib"
  for f in "$STAGE"/lib/*.dylib "$STAGE/recpluto" "$STAGE/isdbtd"; do codesign -f -s - "$f"; done
  otool -L "$STAGE/recpluto" | sed 's/^/  /'
  otool -L "$STAGE/lib/libiio.1.dylib" | sed 's/^/  /'
else
  LIBIIO=$(ls "$PREFIX"/lib*/libiio.so.1.* "$PREFIX"/lib/*/libiio.so.1.* 2>/dev/null | head -1)
  cp "$LIBIIO" "$STAGE/lib/libiio.so.1"
  (cd "$STAGE" && ldd recpluto | sed 's/^/  /') || true
fi

cat > "$STAGE/README.txt" <<EOF
recpluto $VERSION ($LABEL) — Pluto SDR で地上デジタル放送（ISDB-T フルセグ）とケーブルテレビの BS/CS
トランスモジュレーション（ISDB-C）を受信し、Mirakurun のチューナーコマンドとして TS を標準出力に流す。
isdbtd は同じ復調器の TCP デーモン版（録音ファイルの再生もできる）。

使い方（詳細は同梱の mirakurun.md）:
  ./recpluto 27 10 test.ts          # UHF 27ch を 10 秒録画
  ./recpluto --scan gr:13-52        # 受信できる局を channels.yml の形で表示
  ./recpluto --type=BS C41 - -      # ケーブルの BS/CS 再送信（C13..C63 / UHF 帯は --type=BS）を標準出力へ
  tuners.yml: command: "recpluto --type=<type> --tsid=<tsid> <channel> - -"（mirakurun/ の雛形）

必要なもの:
  - Linux: libusb-1.0-0 と libxml2（通常は入っている）。Pluto の USB を一般ユーザーに許可するには
    mirakurun/99-plutosdr.rules を /etc/udev/rules.d/ にコピーして udevadm control --reload
  - macOS: 初回は "xattr -d com.apple.quarantine recpluto isdbtd lib/*.dylib" で隔離属性を外す
    （署名・公証していないため）
  - lib/ の libiio（LGPL-2.1+、https://github.com/analogdevicesinc/libiio）は共有ライブラリとして同梱。
    macOS は libusb（LGPL-2.1+、https://libusb.info）も同梱。差し替え可能。
  - recpluto / isdbtd 本体は MIT（LICENSE）。ソース: https://github.com/k-iseri/BonDriver_PlutoSDR
EOF

mkdir -p "$REPO/dist"
tar -C "$REPO/dist" -czf "$REPO/dist/$NAME.tar.gz" "$NAME"
echo "packaged: dist/$NAME.tar.gz"
tar -tzf "$REPO/dist/$NAME.tar.gz" | sed 's/^/  /'
