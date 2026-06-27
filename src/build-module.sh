#!/bin/bash
# build-module.sh - 编译 .ko 并打包成 KernelSU 模块 zip
#
# 依赖: ddk (https://github.com/Ylarod/ddk)
#
# 用法:
#   ./build-module.sh                    # 编译全部 KMI 并打包（调试模式）
#   ./build-module.sh android14-6.1      # 只编译指定 KMI 并打包
#   ./build-module.sh --stealth          # 静默模式（关闭 dmesg 日志）
#   ./build-module.sh --stealth android14-6.1  # 静默 + 指定 KMI
set -e

KMIS="android12-5.10 android13-5.10 android13-5.15 android14-5.15 android14-6.1 android15-6.6 android16-6.12"

STEALTH=0
TARGET_KMIS=""
for arg in "$@"; do
    case "$arg" in
        --stealth|-s)
            STEALTH=1
            ;;
        *)
            TARGET_KMIS="$arg"
            ;;
    esac
done

if [ -n "$TARGET_KMIS" ]; then
    KMIS="$TARGET_KMIS"
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MODULE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC_DIR="$SCRIPT_DIR"

if [ "$STEALTH" -eq 1 ]; then
    echo "========== STEALTH MODE (dmesg logs disabled) =========="
    # 通过环境变量和哨兵文件传递给 Makefile（兼容 ddk Docker 模式）
    export HIDE_STEALTH=1
    touch "$SRC_DIR/.stealth"
fi

cd "$SRC_DIR"

for required in module.prop service.sh uninstall.sh hidepid.sh hidepid.json webroot; do
    if [ ! -e "$MODULE_DIR/$required" ]; then
        echo "✗ Missing module file: $MODULE_DIR/$required"
        exit 1
    fi
done

mkdir -p "$MODULE_DIR/ko"
BUILD_FAILED=0

for kmi in $KMIS; do
    echo "========== Building $kmi =========="

    make clean 2>/dev/null || true
    rm -f "$MODULE_DIR/ko/hidepid-${kmi}.ko"

    if ddk build "$kmi" 2>&1; then
        if [ -f "hidepid_kmod.ko" ]; then
            cp "hidepid_kmod.ko" "$MODULE_DIR/ko/hidepid-${kmi}.ko"
            llvm-strip -d "$MODULE_DIR/ko/hidepid-${kmi}.ko" 2>/dev/null || \
            strip -d "$MODULE_DIR/ko/hidepid-${kmi}.ko" 2>/dev/null || true
            echo "✓ Built hidepid-${kmi}.ko"
        else
            echo "✗ Build succeeded but .ko not found for $kmi"
        fi
    else
        echo "✗ Build failed for $kmi"
    fi
    if [ ! -f "$MODULE_DIR/ko/hidepid-${kmi}.ko" ]; then
        BUILD_FAILED=1
    fi
    echo ""
done

if [ "$BUILD_FAILED" -ne 0 ]; then
    echo "Error: missing required ko files, refusing to package incomplete module"
    exit 1
fi

# 打包成 KSU 模块 zip
echo "========== Packaging KSU module =========="
cd "$MODULE_DIR"
chmod +x service.sh uninstall.sh hidepid.sh

# 删除 README.txt（不需要打包进 zip）
rm -f ko/README.txt

OUTPUT_ZIP="$MODULE_DIR/hidepid-ksu-module.zip"
if [ "$STEALTH" -eq 1 ]; then
    OUTPUT_ZIP="$MODULE_DIR/hidepid-ksu-module-stealth.zip"
fi

zip -r "$OUTPUT_ZIP" \
    module.prop \
    service.sh \
    uninstall.sh \
    hidepid.sh \
    hidepid.json \
    ko/ \
    webroot/

echo ""
echo "========== Done =========="
# 清理哨兵文件
rm -f "$SRC_DIR/.stealth"
ls -lh "$OUTPUT_ZIP"
echo ""
if [ "$STEALTH" -eq 1 ]; then
    echo "模式: 静默（无 dmesg 日志）"
else
    echo "模式: 调试（有 dmesg 日志，可用 dmesg | grep hidepid 排查）"
    echo "  生产环境推荐: ./build-module.sh --stealth"
fi
echo ""
echo "刷入方法:"
echo "  1. 把 $(basename "$OUTPUT_ZIP") 传到手机"
echo "  2. 打开 KernelSU 管理器 → 模块 → 从本地安装"
echo "  3. 重启（或重新激活越狱）后自动加载"
echo "  4. 在 KSU 管理器中点击模块的「管理」按钮打开 WebUI"
echo ""
echo "管理隐藏:"
echo "  WebUI: KSU 管理器 → 模块 → 管理"
echo "  命令行: su -c 'sh /data/adb/modules/hidepid/hidepid.sh add 12345'"
