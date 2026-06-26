#!/system/bin/sh
# hidepid.sh - PID + 应用隐藏管理工具（模块版）
# 安装位置: /data/adb/modules/hidepid/hidepid.sh
#
# 功能:
#   - PID 隐藏: 隐藏 /proc/<pid> 目录条目
#   - 应用隐藏: 隐藏 /data/data/<pkg>, /data/app/*/<pkg> 等目录条目
#   - 自动扫描: 隐藏应用后自动扫描运行进程，将匹配包名的 PID 加入隐藏
#   - 模块自隐藏: 从 /proc/modules 摘除自身
#   - 持久化配置: 保存/加载到 /data/adb/hidepid.json (JSON 格式, 存进程名不存PID)

MODDIR=${0%/*}
PROC_IFACE="/proc/.pidmap"
CONF_FILE="/data/adb/hidepid.json"

is_loaded() {
    [ -e "$PROC_IFACE" ]
}

ensure_loaded() {
    if is_loaded; then
        return 0
    fi

    KO_DIR="$MODDIR/ko"
    KVER=$(uname -r)
    ANDROID_VER=$(echo "$KVER" | grep -oE 'android[0-9]+' | head -1)
    KERNEL_VER=$(echo "$KVER" | grep -oE '^[0-9]+\.[0-9]+' | head -1)
    KMI="${ANDROID_VER}-${KERNEL_VER}"
    if [ -z "$ANDROID_VER" ] || [ -z "$KERNEL_VER" ]; then
        echo "Error: cannot detect KMI from: $KVER"
        exit 1
    fi

    KO="$KO_DIR/hidepid-${KMI}.ko"
    if [ ! -f "$KO" ]; then
        echo "Error: $KO not found"
        ls "$KO_DIR"/hidepid-*.ko 2>/dev/null
        exit 1
    fi

    echo "Loading ko: $KO (KMI: $KMI)"
    if ! insmod "$KO"; then
        echo "Error: insmod failed"
        dmesg | grep hidepid | tail -10
        exit 1
    fi
    echo "✓ Module loaded"
}

# ==================== JSON 辅助函数 ====================

# 从 JSON 文件提取数组值 (每行输出一个)
json_get_array() {
    local file="$1" key="$2"
    sed -n "/\"$key\"/,/\]/p" "$file" | \
        grep -o '"[^"]*"' | \
        sed 's/"//g' | \
        tail -n +2
}

# 从 JSON 文件提取布尔值
json_get_bool() {
    local file="$1" key="$2"
    grep -o "\"$key\"[[:space:]]*:[[:space:]]*\(true\|false\)" "$file" | \
        grep -oE '(true|false)' | head -1
}

# 将换行分隔的值合并为 JSON 数组字符串
# 输入: stdin, 输出: "val1", "val2"
# 转义 JSON 特殊字符: \ " 和控制字符 (保留换行作为 awk 行分隔符)
join_json_array() {
    sed 's/\\/\\\\/g; s/"/\\"/g' | \
    tr -d '\000-\011\013-\037' | \
    awk 'NR>1{printf ", "} {printf "\"%s\"", $0}'
}

# ==================== 进程名 ↔ PID 转换 ====================

# 通过进程名解析 PID (可能返回多个, 空格分隔)
resolve_pid_by_name() {
    local name="$1"
    local pids

    # 优先 pidof
    pids=$(pidof "$name" 2>/dev/null)
    if [ -n "$pids" ]; then
        echo "$pids"
        return
    fi

    # 回退到 ps + grep
    pids=$(ps -A -o PID,NAME 2>/dev/null | grep -w "$name" | awk '{print $1}' | tr '\n' ' ')
    if [ -n "$pids" ]; then
        echo "$pids"
    fi
}

# 通过 PID 获取进程名
resolve_name_by_pid() {
    local pid="$1"
    local name

    # 优先读 cmdline (完整包名)
    name=$(cat "/proc/$pid/cmdline" 2>/dev/null | tr '\0' '\n' | head -1)
    # 回退到 comm
    if [ -z "$name" ]; then
        name=$(cat "/proc/$pid/comm" 2>/dev/null)
    fi
    echo "$name"
}

# ==================== 配置文件管理 ====================

LOCK_FILE="/data/adb/hidepid.lock"

# 保存当前 /proc/.pidmap 状态到 JSON 配置文件
# PID 会转换为进程名存储 (重启后仍可恢复)
save_config() {
    if ! is_loaded; then
        echo "Module not loaded, nothing to save"
        return 1
    fi

    # 防止并发写入 (等待最多 5 秒)
    local i=0
    while [ -f "$LOCK_FILE" ] && [ $i -lt 50 ]; do
        sleep 0.1
        i=$((i + 1))
    done
    touch "$LOCK_FILE"
    trap 'rm -f "$LOCK_FILE"' EXIT

    local content
    content=$(cat "$PROC_IFACE" 2>/dev/null)
    if [ -z "$content" ]; then
        echo "Error: cannot read $PROC_IFACE"
        rm -f "$LOCK_FILE"
        return 1
    fi

    # 提取隐藏应用 (格式: "app: com.example.app")
    local apps
    apps=$(echo "$content" | grep -E '^app:' | sed 's/^app:\s*//' | sort -u)

    # 提取隐藏 PID, 转换为进程名
    local procs
    procs=$(echo "$content" | grep -E '^pid:' | sed 's/^pid:\s*//' | while IFS= read -r pid; do
        local name
        name=$(resolve_name_by_pid "$pid")
        # 跳过空名称和已在 apps 列表中的 (auto-scan 会自动发现)
        if [ -n "$name" ]; then
            echo "$name"
        fi
    done | sort -u)

    # 从内核模块 proc 输出读取实际扫描状态
    local autoscan="false"
    if echo "$content" | grep -q '^scan: on'; then
        autoscan="true"
    fi

    # 生成 JSON 数组字符串
    local apps_json procs_json
    apps_json=$(echo "$apps" | grep -v '^$' | join_json_array)
    procs_json=$(echo "$procs" | grep -v '^$' | join_json_array)

    # 原子写入: 先写临时文件, 再 rename
    # 使用 PID 后缀避免并发 save_config 的临时文件冲突
    local tmp_file="${CONF_FILE}.tmp.$$"
    {
        echo '{'
        echo "  \"apps\": [$apps_json],"
        echo "  \"processes\": [$procs_json],"
        echo "  \"autoscan\": $autoscan"
        echo '}'
    } > "$tmp_file"

    if mv "$tmp_file" "$CONF_FILE" 2>/dev/null; then
        local app_count proc_count
        app_count=$(echo "$apps" | grep -c . 2>/dev/null) || true
        proc_count=$(echo "$procs" | grep -c . 2>/dev/null) || true
        [ -z "$app_count" ] && app_count=0
        [ -z "$proc_count" ] && proc_count=0
        echo "✓ 配置已保存到 $CONF_FILE"
        echo "  应用: $app_count 个, 进程: $proc_count 个, 自动扫描: $autoscan"
    else
        echo "Error: failed to write $CONF_FILE"
        rm -f "$tmp_file"
    fi

    rm -f "$LOCK_FILE"
    trap - EXIT
}

# 从 JSON 配置文件加载到 /proc/.pidmap
# 进程名通过 pidof 解析为 PID (未运行的进程会被跳过)
load_config() {
    if [ ! -f "$CONF_FILE" ]; then
        echo "No config file at $CONF_FILE"
        return 1
    fi

    if ! is_loaded; then
        echo "Module not loaded, cannot load config"
        return 1
    fi

    local app_count=0 proc_count=0 resolved=0 autoscan_set=0

    # 加载隐藏应用
    json_get_array "$CONF_FILE" "apps" | while IFS= read -r pkg; do
        [ -z "$pkg" ] && continue
        echo "app:$pkg" > "$PROC_IFACE" 2>/dev/null
    done
    app_count=$(json_get_array "$CONF_FILE" "apps" | grep -c . 2>/dev/null) || true
    [ -z "$app_count" ] && app_count=0

    # 加载隐藏进程 (解析进程名 → PID)
    json_get_array "$CONF_FILE" "processes" | while IFS= read -r name; do
        [ -z "$name" ] && continue
        local pids
        pids=$(resolve_pid_by_name "$name")
        if [ -n "$pids" ]; then
            for pid in $pids; do
                echo "$pid" > "$PROC_IFACE" 2>/dev/null
            done
        fi
    done
    proc_count=$(json_get_array "$CONF_FILE" "processes" | grep -c . 2>/dev/null) || true
    [ -z "$proc_count" ] && proc_count=0

    # 设置自动扫描
    local autoscan
    autoscan=$(json_get_bool "$CONF_FILE" "autoscan")
    if [ "$autoscan" = "true" ]; then
        echo autoscan > "$PROC_IFACE" 2>/dev/null
    else
        echo stopscan > "$PROC_IFACE" 2>/dev/null
    fi

    echo "✓ 已加载配置: $app_count 个应用, $proc_count 个进程"
    echo "  (未运行的进程会在启动后被 auto-scan 发现, 需开启自动扫描)"
    if [ "$autoscan" = "true" ]; then
        echo "  自动扫描: 已开启"
    fi
}

case "$1" in
    # === PID/进程管理 ===
    add)
        if [ -z "$2" ]; then
            echo "Usage: sh hidepid.sh add <pid|进程名>  (0=隐藏自身)"
            echo "  数字: 作为 PID 添加"
            echo "  文本: 作为进程名, 通过 pidof 解析为 PID"
            exit 1
        fi
        ensure_loaded
        case "$2" in
            ''|*[!0-9]*)
                # 非纯数字: 作为进程名, 通过 pidof 解析为 PID
                pids=$(resolve_pid_by_name "$2")
                if [ -z "$pids" ]; then
                    echo "Error: process '$2' not found"
                    exit 1
                fi
                for pid in $pids; do
                    echo "$pid" > "$PROC_IFACE"
                done
                echo "✓ Done (进程: $2 → PID: $pids)"
                ;;
            *)
                # 纯数字: 直接作为 PID
                echo "$2" > "$PROC_IFACE"
                echo "✓ Done (PID: $2)"
                ;;
        esac
        cat "$PROC_IFACE"
        save_config > /dev/null 2>&1
        ;;

    del)
        if [ -z "$2" ]; then
            echo "Usage: sh hidepid.sh del <pid|进程名>"
            exit 1
        fi
        if ! is_loaded; then
            echo "Module not loaded"
            exit 0
        fi
        case "$2" in
            ''|*[!0-9]*)
                # 非纯数字: 作为进程名, 通过 pidof 解析为 PID
                pids=$(resolve_pid_by_name "$2")
                if [ -z "$pids" ]; then
                    echo "Error: process '$2' not found"
                    exit 1
                fi
                for pid in $pids; do
                    echo "-$pid" > "$PROC_IFACE"
                done
                echo "✓ Done (进程: $2 → PID: $pids)"
                ;;
            *)
                # 纯数字: 直接作为 PID
                echo "-$2" > "$PROC_IFACE"
                echo "✓ Done (PID: $2)"
                ;;
        esac
        cat "$PROC_IFACE"
        save_config > /dev/null 2>&1
        ;;

    # === 应用管理 ===
    app)
        if [ -z "$2" ]; then
            echo "Usage: sh hidepid.sh app <package_name>"
            echo "       sh hidepid.sh app com.example.app"
            exit 1
        fi
        ensure_loaded
        echo "app:$2" > "$PROC_IFACE"
        echo "✓ App hidden: $2"
        echo "  (auto-scan started, running processes will be hidden)"
        save_config > /dev/null 2>&1
        ;;

    unapp)
        if [ -z "$2" ]; then
            echo "Usage: sh hidepid.sh unapp <package_name>"
            exit 1
        fi
        if ! is_loaded; then
            echo "Module not loaded"
            exit 0
        fi
        echo "unapp:$2" > "$PROC_IFACE"
        echo "✓ App unhidden: $2"
        save_config > /dev/null 2>&1
        ;;

    # === 扫描控制 ===
    scan)
        if ! is_loaded; then
            echo "Module not loaded"
            exit 0
        fi
        echo scan > "$PROC_IFACE"
        echo "✓ Manual scan triggered"
        sleep 1
        cat "$PROC_IFACE"
        ;;

    autoscan)
        if ! is_loaded; then
            echo "Module not loaded"
            exit 0
        fi
        echo autoscan > "$PROC_IFACE"
        echo "✓ Auto-scan started (every 3s)"
        save_config > /dev/null 2>&1
        ;;

    stopscan)
        if ! is_loaded; then
            echo "Module not loaded"
            exit 0
        fi
        echo stopscan > "$PROC_IFACE"
        echo "✓ Auto-scan stopped"
        save_config > /dev/null 2>&1
        ;;

    # === 配置管理 ===
    save)
        save_config
        ;;

    load)
        load_config
        ;;

    config)
        echo "Config file: $CONF_FILE"
        if [ -f "$CONF_FILE" ]; then
            echo "---"
            cat "$CONF_FILE"
        else
            echo "(not found)"
        fi
        ;;

    # === 通用 ===
    list)
        if ! is_loaded; then
            echo "Module not loaded"
            exit 0
        fi
        cat "$PROC_IFACE"
        ;;

    clear)
        if ! is_loaded; then
            echo "Module not loaded"
            exit 0
        fi
        echo clear > "$PROC_IFACE"
        echo "✓ All PIDs and Apps cleared (scan stopped)"
        save_config > /dev/null 2>&1
        ;;

    unload)
        if is_loaded; then
            echo unload > "$PROC_IFACE"
            echo "✓ Unload scheduled (500ms delay)"
            sleep 2
            if lsmod 2>/dev/null | grep -q hidepid_kmod; then
                echo "  Module still loaded (rmmod may have failed)"
                echo "  Try: rmmod hidepid_kmod"
            else
                echo "  Module unloaded successfully"
            fi
        else
            echo "Module not loaded"
        fi
        ;;

    *)
        echo "hidepid.sh - PID + 应用隐藏管理工具"
        echo ""
        echo "进程/PID 管理:"
        echo "  sh hidepid.sh add <pid|名称>    隐藏 (数字=PID, 文本=进程名自动解析)"
        echo "  sh hidepid.sh del <pid|名称>    取消隐藏"
        echo ""
        echo "应用管理:"
        echo "  sh hidepid.sh app <pkg>         隐藏应用 (自动启动进程扫描)"
        echo "  sh hidepid.sh unapp <pkg>       取消隐藏应用"
        echo ""
        echo "扫描控制:"
        echo "  sh hidepid.sh scan              手动扫描一次"
        echo "  sh hidepid.sh autoscan          启动持续自动扫描 (每3秒)"
        echo "  sh hidepid.sh stopscan          停止自动扫描"
        echo ""
        echo "配置管理:"
        echo "  sh hidepid.sh save              保存当前列表到 $CONF_FILE"
        echo "  sh hidepid.sh load              从配置加载到内核模块"
        echo "  sh hidepid.sh config            查看配置文件内容"
        echo "  (add/del/app/unapp/clear 操作后自动保存)"
        echo ""
        echo "通用:"
        echo "  sh hidepid.sh list              查看已隐藏列表"
        echo "  sh hidepid.sh clear             清空所有隐藏"
        echo "  sh hidepid.sh unload            卸载模块"
        echo ""
        echo "配置文件格式 (JSON):"
        echo "  {"
        echo "    \"apps\": [\"com.example.app\"],"
        echo "    \"processes\": [\"com.tencent.mm:push\"],"
        echo "    \"autoscan\": true"
        echo "  }"
        echo "  apps: 应用包名 (隐藏目录条目 + 自动扫描进程)"
        echo "  processes: 进程名 (加载时通过 pidof 解析为 PID)"
        echo "  autoscan: 是否开启自动扫描"
        ;;
esac
