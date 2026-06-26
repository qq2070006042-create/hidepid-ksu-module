#!/bin/bash
# build-all.sh - 为所有 GKI 5.10+ KMI 编译 hidepid_kmod.ko
#
# 依赖: ddk (https://github.com/Ylarod/ddk)
#
# 用法:
#   ./build-all.sh              # 编译全部 7 个 KMI
#   ./build-all.sh android14-6.1  # 只编译指定 KMI
#   HIDE_STEALTH=1 ./build-all.sh  # 静默模式（关闭 dmesg 日志）
set -e

KMIS="android12-5.10 android13-5.10 android13-5.15 android14-5.15 android14-6.1 android15-6.6 android16-6.12"

if [ -n "$1" ]; then
    KMIS="$1"
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

if [ -n "$HIDE_STEALTH" ]; then
    echo "========== STEALTH MODE (dmesg logs disabled) =========="
    touch "$SCRIPT_DIR/.stealth"
fi

for kmi in $KMIS; do
    echo "========== Building $kmi =========="

    # 清理上次构建产物
    make clean 2>/dev/null || true

    # 用 ddk 构建（Docker 模式或 Local 模式均可）
    if ddk build "$kmi" 2>&1; then
        if [ -f "hidepid_kmod.ko" ]; then
            cp "hidepid_kmod.ko" "hidepid-${kmi}.ko"
            llvm-strip -d "hidepid-${kmi}.ko" 2>/dev/null || \
            strip -d "hidepid-${kmi}.ko" 2>/dev/null || true
            echo "✓ Built hidepid-${kmi}.ko"
        else
            echo "✗ Build succeeded but .ko not found for $kmi"
        fi
    else
        echo "✗ Build failed for $kmi"
    fi
    echo ""
done

echo "========== Final output =========="
rm -f "$SCRIPT_DIR/.stealth"
ls -lh hidepid-*.ko 2>/dev/null || echo "No .ko files produced"
