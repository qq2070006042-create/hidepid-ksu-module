const fs = require("fs");

const source = fs.readFileSync("src/hidepid_kmod.c", "utf8");

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

if (/file->f_owner\.pid/.test(source.replace(/static struct pid \*file_owner_pid[\s\S]*?\n}/, ""))) {
  throw new Error("file->f_owner is a pointer on Linux 6.12; use file_owner_pid()");
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
