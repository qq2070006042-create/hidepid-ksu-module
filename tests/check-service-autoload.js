const fs = require("fs");

const service = fs.readFileSync("service.sh", "utf8");
const hidepid = fs.readFileSync("hidepid.sh", "utf8");

const enableMarker = "/data/adb/hidepid.enable";
const markerIndex = service.indexOf(enableMarker);
if (markerIndex === -1) {
  throw new Error(`service.sh must require ${enableMarker} before loading the ko`);
}

const firstInsmod = service.indexOf('if insmod "$KO"');
if (firstInsmod === -1) {
  throw new Error("service.sh no longer has a module load path");
}

if (firstInsmod < markerIndex) {
  throw new Error("service.sh checks for the enable marker only after the first insmod");
}

const markerCheck = `[ ! -f "$ENABLE_FILE" ]`;
if (!service.includes(markerCheck)) {
  throw new Error(`service.sh must skip autoload when ${enableMarker} is absent`);
}

if (!service.includes("autoload disabled")) {
  throw new Error("service.sh should log when boot autoload is disabled");
}

if (service.includes("hooks:on")) {
  throw new Error("service.sh must not enable core hooks during boot autoload");
}

if (!hidepid.includes("hooks:on") || !hidepid.includes("hooks:off")) {
  throw new Error("hidepid.sh must expose explicit core hook controls");
}

const addBranch = hidepid.match(/\n\s*add\)[\s\S]*?\n\s*del\)/)?.[0] || "";
if (addBranch.includes("hooks:on")) {
  throw new Error("hidepid.sh add must not implicitly enable core hooks");
}

const appBranch = hidepid.match(/\n\s*app\)[\s\S]*?\n\s*unapp\)/)?.[0] || "";
if (appBranch.includes("hooks:on")) {
  throw new Error("hidepid.sh app must not implicitly enable core hooks");
}

console.log("service.sh autoload guard checks passed");
