const fs = require("fs");

const html = fs.readFileSync("webroot/index.html", "utf8");

for (const command of [
  "sh /data/adb/modules/hidepid/hidepid.sh add",
  "sh /data/adb/modules/hidepid/hidepid.sh app",
]) {
  if (!html.includes(command)) {
    throw new Error(`WebUI must use ${command} so actions can load the ko on demand`);
  }
}

for (const legacy of [
  "echo ' + shellQuote(input) + ' > ' + PROC",
  "echo ' + shellQuote('app:' + pkg) + ' > ' + PROC",
]) {
  if (html.includes(legacy)) {
    throw new Error(`WebUI still uses direct proc write: ${legacy}`);
  }
}

console.log("WebUI load path checks passed");
