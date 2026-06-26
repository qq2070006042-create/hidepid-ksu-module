const fs = require("fs");

const source = fs.readFileSync("src/hidepid_kmod.c", "utf8");

if (/\bmodule_(alloc|free)\s*\(/.test(source) &&
    !source.includes("#include <linux/moduleloader.h>")) {
  throw new Error("hidepid_kmod.c uses module_alloc/module_free but does not include <linux/moduleloader.h>");
}

if (/\bmodule_free\s*\(/.test(source)) {
  throw new Error("Android GKI moduleloader.h exposes module_memfree(), not module_free()");
}

console.log("Kernel source checks passed");
