# hidepid 内核模块编译教程

## 功能概述

| 功能 | 说明 |
|------|------|
| PID 隐藏 | 在 /proc 目录列举时跳过目标 PID，`ps`/`ls /proc` 看不到 |
| PID 直接访问阻断 | `cat /proc/<pid>/cmdline` 等直接访问返回 ENOENT |
| 应用隐藏 | 在任意目录列举时跳过匹配包名的条目，`ls /data/data` 看不到应用目录 |
| 网络连接隐藏 | `/proc/net/tcp` 等中过滤隐藏进程的连接，`netstat` 看不到 |
| 自动扫描 | 隐藏应用后自动扫描运行进程，将匹配包名的 PID 加入隐藏列表 |
| 模块自隐藏 | 从 /proc/modules 摘除自身，lsmod 看不到 |
| WebUI 管理 | KernelSU 管理器中直接打开 WebUI 管理界面 |
| 安全卸载 | 通过 /proc/.pidmap 的 unload 命令安全卸载 |

## 架构

```
┌──────────────────────────────────────────────────┐
│                KernelSU 管理器                     │
│                   WebUI (webroot/)                │
│              点击「管理」按钮打开                   │
└──────────────────┬───────────────────────────────┘
                   │ ksu.exec("echo app:xxx > /proc/.pidmap")
                   ▼
┌──────────────────────────────────────────────────┐
│            /proc/.pidmap (通讯接口)                │
│         内核模块创建的 proc 文件 (0600)            │
└──────────────────┬───────────────────────────────┘
                   │ 读写
                   ▼
┌──────────────────────────────────────────────────┐
│          hidepid_kmod.ko (内核模块)                │
│                                                   │
│  ┌─────────────┐  ┌──────────────┐  ┌──────────┐ │
│  │ filldir hook │  │ PID 列表管理  │  │ 应用列表  │ │
│  │ (目录列举)   │  │ (64 slots)   │  │ (32 slots)│ │
│  └─────────────┘  └──────────────┘  └──────────┘ │
│  ┌─────────────┐  ┌──────────────┐               │
│  │proc_pid_lookup│ │  tcp/udp     │               │
│  │(直接访问阻断)│  │  seq_show    │               │
│  └─────────────┘  │  (网络隐藏)  │               │
│  ┌─────────────┐  └──────────────┘               │
│  │ 模块自隐藏   │  ┌──────────────┐               │
│  │ (list_del)  │  │ 进程自动扫描  │               │
│  └─────────────┘  │ (workqueue)  │               │
│                    └──────────────┘               │
└──────────────────────────────────────────────────┘
```

## 文件说明

| 文件 | 用途 |
|------|------|
| `hidepid_kmod.c` | 内核模块源码（PID + 应用隐藏 + 自动扫描） |
| `Makefile` | 构建配置，配合 ddk 使用，支持静默模式 |
| `Kbuild` | 内核构建系统配置 |
| `build-all.sh` | 只编译 .ko（不打包模块） |
| `build-module.sh` | 编译 .ko 并打包成 KernelSU 模块 zip（推荐） |
| `hidepid_module/` | KernelSU 模块目录结构 |
| `hidepid_module/webroot/index.html` | WebUI 管理界面 |
| `hidepid.sh` | PID + 应用管理脚本（命令行） |

---

## 编译

### 方式一：Docker 模式（推荐）

```bash
# 安装 ddk
sudo curl -fsSL https://raw.githubusercontent.com/Ylarod/ddk/main/scripts/ddk -o /usr/local/bin/ddk
sudo chmod +x /usr/local/bin/ddk

# 拉取 KMI 镜像
ddk pull android14-6.1

# 编译全部 KMI 并打包（含 WebUI）
./build-module.sh

# 静默模式（关闭 dmesg 日志）
./build-module.sh --stealth

# 只编译指定 KMI
./build-module.sh android14-6.1
```

### 方式二：Local 模式

```bash
sudo apt install git-lfs zstd jq
git lfs install
GIT_LFS_SKIP_SMUDGE=1 git clone --recurse-submodules https://github.com/Ylarod/ddk.git
cd ddk/prebuilts
for d in kdir clang rust src; do git lfs pull -I $d; done
cd ..
bash host/install.sh
echo "local" > ~/.ddk/mode

cd /path/to/hidepid
./build-module.sh
```

产物：`hidepid-ksu-module.zip`（含 .ko + WebUI + 脚本）

---

## 设备端使用

### 安装

```bash
# 把 zip 传到手机
adb push hidepid-ksu-module.zip /sdcard/Download/

# 打开 KernelSU 管理器 → 模块 → 从本地安装 → 选择 zip
# 重启（或重新激活越狱）后自动加载
```

安装后目录结构：
```
/data/adb/modules/hidepid/
├── module.prop          # 模块信息
├── service.sh           # 开机自动 insmod
├── uninstall.sh         # 卸载时安全移除
├── hidepid.sh           # 命令行管理工具
├── webroot/
│   └── index.html       # WebUI 管理界面
└── ko/                  # 编译好的 .ko 文件
    ├── hidepid-android14-6.1.ko
    └── ...
```

### 方式 A：WebUI 管理（推荐）

在 KernelSU 管理器中，找到 hidepid 模块，点击「管理」按钮打开 WebUI。

WebUI 功能：
- 查看/添加/移除隐藏的 PID
- 查看已安装应用列表，一键选择隐藏
- 手动输入包名隐藏应用
- 开关自动进程扫描
- 查看模块运行状态
- 清空全部 / 卸载模块

### 方式 B：命令行管理

```bash
# PID 管理
su -c "sh /data/adb/modules/hidepid/hidepid.sh add 12345"
su -c "sh /data/adb/modules/hidepid/hidepid.sh add 0"      # 隐藏自身
su -c "sh /data/adb/modules/hidepid/hidepid.sh del 12345"

# 应用管理
su -c "sh /data/adb/modules/hidepid/hidepid.sh app com.example.app"
su -c "sh /data/adb/modules/hidepid/hidepid.sh unapp com.example.app"

# 扫描控制
su -c "sh /data/adb/modules/hidepid/hidepid.sh scan"       # 手动扫描一次
su -c "sh /data/adb/modules/hidepid/hidepid.sh autoscan"   # 启动持续扫描
su -c "sh /data/adb/modules/hidepid/hidepid.sh stopscan"   # 停止扫描

# 通用
su -c "sh /data/adb/modules/hidepid/hidepid.sh list"       # 查看列表
su -c "sh /data/adb/modules/hidepid/hidepid.sh clear"      # 清空全部
su -c "sh /data/adb/modules/hidepid/hidepid.sh unload"     # 卸载模块
```

### 方式 C：直接操作 /proc 接口

```bash
echo 12345              > /proc/.pidmap   # 添加 PID
echo 0                  > /proc/.pidmap   # 隐藏当前进程
echo -12345             > /proc/.pidmap   # 移除 PID
echo "app:com.example"  > /proc/.pidmap   # 隐藏应用
echo "unapp:com.example" > /proc/.pidmap  # 取消隐藏
echo scan               > /proc/.pidmap   # 手动扫描
echo autoscan           > /proc/.pidmap   # 启动自动扫描
echo stopscan           > /proc/.pidmap   # 停止扫描
echo clear              > /proc/.pidmap   # 清空全部
echo unload             > /proc/.pidmap   # 卸载模块
cat /proc/.pidmap                         # 查看列表
```

### 验证

```bash
ls /proc | grep 12345              # 应无输出（PID 已隐藏）
ls /data/data | grep com.example   # 应无输出（应用已隐藏）
ls /proc | grep .pidmap            # 应无输出（管理接口已隐藏）
lsmod | grep hidepid               # 应无输出（模块已自隐藏）
```

---

## 与 HMA-OSS 的区别

| 特性 | 本模块 (内核态 + Zygisk) | HMA-OSS (Xposed) |
|------|------------------------|------------------|
| 工作层级 | 内核 filldir + proc_pid_lookup + tcp/udp hook | Java 框架层 PMS hook |
| `pm list packages` | 拦截 (Zygisk) | 拦截 |
| `dumpsys activity` | 拦截 (Zygisk hook AMS) | 拦截 |
| `ls /data/data` | 隐藏 (内核) | 不隐藏 |
| `ls /proc/<pid>` | 隐藏 (内核 filldir) | 不隐藏 |
| `cat /proc/<pid>/cmdline` | 阻断 (内核 proc_pid_lookup) | 不隐藏 |
| `netstat` / `/proc/net/tcp` | 隐藏 (内核 seq_show hook) | 不隐藏 |
| 需要 LSPosed/Zygisk | Zygisk (可选, 用于 Java API) | 是 |
| 管理界面 | WebUI (内置) | APK |
| 需要内核权限 | 是（KSU 越狱模式） | 否 |

**互补关系**：本模块在文件系统层面隐藏应用痕迹，HMA-OSS 在 Java API 层面隐藏应用列表。如需完全替代 HMA-OSS（拦截 `pm list packages`），需额外使用 Zygisk 模块 hook PMS。

---

## 静默模式

| 方式 | 命令 |
|------|------|
| build-module.sh | `./build-module.sh --stealth` |
| build-all.sh | `HIDE_STEALTH=1 ./build-all.sh` |
| Makefile 永久启用 | 取消 Makefile 中 `#ccflags-y += -DHIDE_DEBUG=0` 的注释 |

---

## 常见问题

### Q: 编译报错 "KDIR not set"
A: 确认 `ddk list` 能列出目标 KMI，或先 `ddk pull <kmi>`。

### Q: 设备上 insmod 报 "vermagic mismatch"
A: .ko 的 KMI 与设备内核不匹配。执行 `uname -r`，对应 KMI 为 `androidXX-X.X`。

### Q: WebUI 打不开
A: 确认 KernelSU 管理器版本支持 WebUI（KernelSU v0.7.0+）。在模块列表中点击模块的「管理」按钮。

### Q: 隐藏应用后 `pm list packages` 还能看到
A: 正常。`pm list packages` 走 binder IPC 到 system_server 的 PMS，内核模块不拦截 Java API。本模块隐藏的是文件系统层面的痕迹（/data/data, /data/app, /proc）。

### Q: 越狱模式重启后模块失效
A: 正常现象。越狱模式每次重启需重新激活 KSU，激活后 service.sh 会自动重新加载 ko。

### Q: 卸载模块时报错
A: 模块自隐藏后 `rmmod` 找不到。使用 WebUI 的卸载按钮，或 `echo unload > /proc/.pidmap`。
