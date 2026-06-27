const fs = require("fs");

const source = fs.readFileSync("src/hidepid_kmod.c", "utf8");
const codeOnly = source
  .replace(/\/\*[\s\S]*?\*\//g, "")
  .replace(/\/\/.*$/gm, "")
  .replace(/"(?:\\.|[^"\\])*"/g, '""');

if (/\bmodule_(alloc|free)\s*\(/.test(source) &&
    !source.includes("#include <linux/moduleloader.h>")) {
  throw new Error("hidepid_kmod.c uses module_alloc/module_free but does not include <linux/moduleloader.h>");
}

if (/\bmodule_free\s*\(/.test(source)) {
  throw new Error("Android GKI moduleloader.h exposes module_memfree(), not module_free()");
}

if (/\bmodule_(alloc|memfree)\s*\(/.test(source)) {
  throw new Error("trampoline memory must use alloc_trampoline/free_trampoline compatibility wrappers");
}

for (const symbol of [
  "set_memory_rw",
  "set_memory_ro",
  "set_memory_x",
  "access_remote_vm",
  "call_usermodehelper",
]) {
  if (new RegExp(`\\b${symbol}\\s*\\(`).test(codeOnly)) {
    throw new Error(`${symbol} is not exported on some Android GKI kernels; resolve it via kallsyms instead of calling it directly`);
  }
}

if (/file->f_owner\.pid/.test(source.replace(/static struct pid \*file_owner_pid[\s\S]*?\n}/, ""))) {
  throw new Error("file->f_owner is a pointer on Linux 6.12; use file_owner_pid()");
}

if (/strcmp\s*\(\s*name\s*,\s*PROC_NAME\s*\)/.test(codeOnly) ||
    /strcmp\s*\(\s*name\s*,\s*MODULE_NAME_STR\s*\)/.test(codeOnly)) {
  throw new Error("filldir names are length-delimited; do not compare them with strcmp()");
}

if (/static\s+bool\s+parse_pid\s*\(\s*const\s+char\s+\*name\s*,\s*pid_t\s+\*out\s*\)/.test(codeOnly)) {
  throw new Error("parse_pid must take a name length; filldir and qstr names are not guaranteed NUL-terminated");
}

if (!/#define\s+HOOK_BYTES\s+24\b/.test(source)) {
  throw new Error("ARM64 inline hook patch must reserve 24 bytes for a BTI-safe absolute branch");
}

if (!source.includes("0xD503245F")) {
  throw new Error("ARM64 inline hook patch must preserve a BTI C landing pad");
}

if (/p\[0\]\s*=\s*0x58000050/.test(source)) {
  throw new Error("ARM64 inline hook patch starts with LDR instead of BTI C");
}

for (const badCall of [
  "parse_pid(name, &pid)",
  "parse_pid(dentry->d_name.name, &pid)",
  "should_hide_name(name)",
]) {
  if (codeOnly.includes(badCall)) {
    throw new Error(`${badCall} ignores the provided name length`);
  }
}

const scanWork = source.match(/static void scan_work_fn[\s\S]*?\n}/)?.[0] || "";
if (/rcu_read_lock\s*\(\s*\)[\s\S]*get_task_cmdline\s*\(/.test(
    scanWork.replace(/rcu_read_unlock\s*\(\s*\)[\s\S]*/g, "")
)) {
  throw new Error("scan_work_fn must not call get_task_cmdline() while holding rcu_read_lock()");
}

const getTaskCommArg = scanWork.match(/get_task_comm\s*\(\s*(\w+)\s*,/);
if (getTaskCommArg) {
  const commVar = getTaskCommArg[1];
  const declaration = new RegExp(`char\\s+${commVar}\\s*\\[\\s*TASK_COMM_LEN\\s*\\]`);
  if (!declaration.test(scanWork)) {
    throw new Error("get_task_comm() requires a TASK_COMM_LEN-sized buffer on Android GKI kernels");
  }
}

const initFunction = source.match(/static int __init hidepid_init[\s\S]*?module_init/s)?.[0] || "";
if (initFunction.includes("install_hook(")) {
  throw new Error("module init must not install inline hooks; load must be side-effect minimal");
}

for (const riskyInitCall of [
  "resolve_kln(",
  "resolve_memory_symbols(",
  "resolve_module_symbols(",
  "resolve_optional_runtime_symbols(",
]) {
  if (initFunction.includes(riskyInitCall)) {
    throw new Error(`module init must not call ${riskyInitCall}; resolve kernel symbols lazily`);
  }
}

if (initFunction.includes("hide_module_from_list(")) {
  throw new Error("module init must not hide the module from the modules list");
}

for (const symbol of [
  "tcp4_seq_show",
  "tcp6_seq_show",
  "udp4_seq_show",
  "udp6_seq_show",
]) {
  if (initFunction.includes(`install_hook(&h_${symbol}`)) {
    throw new Error(`${symbol} hook must not be installed during module init; keep boot load minimal`);
  }
}

const enableCoreHooksStart = source.indexOf("static int enable_core_hooks");
const disableCoreHooksStart = source.indexOf("static void disable_core_hooks", enableCoreHooksStart);
const enableCoreHooks = enableCoreHooksStart >= 0 && disableCoreHooksStart > enableCoreHooksStart
  ? source.slice(enableCoreHooksStart, disableCoreHooksStart)
  : "";
if (enableCoreHooks.includes("install_hook(")) {
  throw new Error("core hook enable path must not use inline text patching");
}

if (!enableCoreHooks.includes("register_core_kprobe(")) {
  throw new Error("core hook enable path should use kernel kprobe registration");
}

if (!source.includes("register_kprobe(kp)")) {
  throw new Error("register_core_kprobe() must call register_kprobe()");
}

if (/for\s*\(\s*(?:const\s+)?(?:char|int|long|pid_t|size_t|unsigned|struct)\b/.test(source)) {
  throw new Error("kernel C code must not declare variables in for-loop initializers");
}

for (const line of source.split(/\r?\n/)) {
  if (line.includes("static ")) continue;
  if (/char\s+\w+\s*\[\s*MAX_HIDE_APPS\s*\]\s*\[\s*MAX_PKG_NAME_LEN\s*\]/.test(line)) {
    throw new Error("large app arrays must be heap-allocated, not placed on the kernel stack");
  }
  if (/pid_t\s+\w+\s*\[\s*MAX_HIDE_PIDS\s*\]/.test(line)) {
    throw new Error("large PID arrays must be heap-allocated, not placed on the kernel stack");
  }
}

console.log("Kernel source checks passed");
