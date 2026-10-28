#!/bin/sh
# 本地构建 ForceCloseOreUI.so（产物：build/android/arm64-v8a/release/libForceCloseOreUI.so）
set -e
CDIR=$(cd "$(dirname "$0")" && pwd)
cd "$CDIR"
xmake f -p android -a arm64-v8a -c -y
xmake -y
echo "OK -> $CDIR/build/android/arm64-v8a/release/libForceCloseOreUI.so"
