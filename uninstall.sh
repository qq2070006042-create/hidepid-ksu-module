#!/system/bin/sh
# uninstall.sh - KSU 模块卸载时执行
#
# 模块加载后会自隐藏（从 /proc/modules 摘除），
# rmmod 找不到模块名，必须通过 proc 接口触发安全卸载:
#   echo unload > /proc/.pidmap
# 内核会: 恢复模块到链表 → 移除 hook → 异步 rmmod

if [ -e /proc/.pidmap ]; then
    echo unload > /proc/.pidmap
    # 等待异步 rmmod 完成
    sleep 2
fi

# 询问是否清理配置文件 (模块卸载时保留配置, 以备重新安装)
# 如需彻底清理, 手动删除: rm /data/adb/hidepid.json
