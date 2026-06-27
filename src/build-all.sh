#!/bin/bash
# build-all.sh - build hidepid_kmod.ko for one or more Android GKI KMIs.
#
# Usage:
#   ./build-all.sh
#   ./build-all.sh android14-6.1
#   HIDE_STEALTH=1 ./build-all.sh android14-6.1
set -e

KMIS="android12-5.10 android13-5.10 android13-5.15 android14-5.15 android14-6.1 android15-6.6 android16-6.12"

if [ -n "$1" ]; then
    KMIS="$1"
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

cleanup() {
    rm -f "$SCRIPT_DIR/.stealth"
}
trap cleanup EXIT

if [ -n "$HIDE_STEALTH" ]; then
    echo "========== STEALTH MODE (dmesg logs disabled) =========="
    touch "$SCRIPT_DIR/.stealth"
fi

BUILD_FAILED=0

for kmi in $KMIS; do
    echo "========== Building $kmi =========="

    make clean 2>/dev/null || true
    rm -f "hidepid-${kmi}.ko"

    if ddk build "$kmi" 2>&1; then
        if [ -f "hidepid_kmod.ko" ]; then
            cp "hidepid_kmod.ko" "hidepid-${kmi}.ko"
            llvm-strip -d "hidepid-${kmi}.ko" 2>/dev/null || \
            strip -d "hidepid-${kmi}.ko" 2>/dev/null || true
            echo "Built hidepid-${kmi}.ko"
        else
            echo "Build succeeded but .ko not found for $kmi"
            BUILD_FAILED=1
        fi
    else
        echo "Build failed for $kmi"
        BUILD_FAILED=1
    fi
    echo ""
done

echo "========== Final output =========="
ls -lh hidepid-*.ko 2>/dev/null || echo "No .ko files produced"

if [ "$BUILD_FAILED" -ne 0 ]; then
    echo "Error: missing required ko files"
    exit 1
fi
