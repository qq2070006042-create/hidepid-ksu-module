#!/system/bin/sh
# service.sh - KSU 模块 service 阶段执行
# 此时已有 root 权限，自动检测 KMI 并加载对应 .ko
#
# 加载流程:
#   1. 检测 KMI, insmod 对应 .ko
#   2. 读取 /data/adb/hidepid.json 配置
#   3. 将 apps 写入 /proc/.pidmap (app:<pkg>)
#   4. 将 processes 通过 pidof 解析为 PID 后写入 /proc/.pidmap
#   5. 设置自动扫描

MODDIR=${0%/*}
KO_DIR="$MODDIR/ko"
CONF_FILE="/data/adb/hidepid.json"

# JSON 辅助函数
json_get_array() {
    sed -n "/\"$1\"/,/\]/p" "$2" | \
        grep -o '"[^"]*"' | \
        sed 's/"//g' | \
        tail -n +2
}

json_get_bool() {
    grep -o "\"$1\"[[:space:]]*:[[:space:]]*\(true\|false\)" "$2" | \
        grep -oE '(true|false)' | head -1
}

# 等待系统基本就绪
sleep 2

# === 加载内核模块 ===
if [ ! -e /proc/.pidmap ]; then
    KVER=$(uname -r)
    ANDROID_VER=$(echo "$KVER" | grep -oE 'android[0-9]+' | head -1)
    KERNEL_VER=$(echo "$KVER" | grep -oE '^[0-9]+\.[0-9]+' | head -1)

    if [ -z "$ANDROID_VER" ] || [ -z "$KERNEL_VER" ]; then
        echo "hidepid: cannot detect KMI from: $KVER" > /dev/kmsg 2>/dev/null
        exit 1
    fi

    KMI="${ANDROID_VER}-${KERNEL_VER}"
    KO="$KO_DIR/hidepid-${KMI}.ko"
    if [ ! -f "$KO" ]; then
        echo "hidepid: $KO not found" > /dev/kmsg 2>/dev/null
        ls "$KO_DIR"/hidepid-*.ko 2>/dev/null > /dev/kmsg
        exit 1
    fi

    if insmod "$KO"; then
        echo "hidepid: loaded $KO (KMI: $KMI)" > /dev/kmsg 2>/dev/null
    else
        echo "hidepid: insmod failed" > /dev/kmsg 2>/dev/null
        exit 1
    fi
fi

# === 加载 JSON 配置 ===
if [ ! -f "$CONF_FILE" ]; then
    echo "hidepid: no config at $CONF_FILE (first boot?)" > /dev/kmsg 2>/dev/null
    exit 0
fi

echo "hidepid: loading config from $CONF_FILE" > /dev/kmsg 2>/dev/null

# 加载隐藏应用
json_get_array "apps" "$CONF_FILE" | while IFS= read -r pkg; do
    [ -z "$pkg" ] && continue
    echo "app:$pkg" > /proc/.pidmap 2>/dev/null
done

# 加载隐藏进程 (进程名 → PID)
json_get_array "processes" "$CONF_FILE" | while IFS= read -r name; do
    [ -z "$name" ] && continue
    pids=$(pidof "$name" 2>/dev/null)
    if [ -z "$pids" ]; then
        # 回退到 ps
        pids=$(ps -A -o PID,NAME 2>/dev/null | grep -w "$name" | awk '{print $1}' | tr '\n' ' ')
    fi
    if [ -n "$pids" ]; then
        for pid in $pids; do
            echo "$pid" > /proc/.pidmap 2>/dev/null
        done
    fi
done

# 设置自动扫描
# 注意: 加载 app: 命令时内核模块会自动启动扫描
# 如果配置中 autoscan=false, 需要显式停止
autoscan=$(json_get_bool "autoscan" "$CONF_FILE")
if [ "$autoscan" = "true" ]; then
    echo autoscan > /proc/.pidmap 2>/dev/null
else
    echo stopscan > /proc/.pidmap 2>/dev/null
fi

echo "hidepid: config loaded" > /dev/kmsg 2>/dev/null
