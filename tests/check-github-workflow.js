const fs = require("fs");

const workflow = fs.readFileSync(".github/workflows/build-ko.yml", "utf8");

const expectedKmis = [
  "android12-5.10",
  "android13-5.10",
  "android13-5.15",
  "android14-5.15",
  "android14-6.1",
  "android15-6.6",
  "android16-6.12",
];

for (const kmi of expectedKmis) {
  if (!workflow.includes(`- ${kmi}`)) {
    throw new Error(`workflow matrix is missing ${kmi}`);
  }
  if (!workflow.includes(`test -f ko/hidepid-${kmi}.ko`)) {
    throw new Error(`package job does not verify hidepid-${kmi}.ko`);
  }
}

for (const text of [
  "uses: actions/checkout@v5",
  "uses: actions/upload-artifact@v6",
  "uses: actions/download-artifact@v7",
  "echo docker > ~/.ddk/mode",
  "echo github > ~/.ddk/source",
  "node tests/check-kernel-source.js",
  "ddk pull \"${{ matrix.kmi }}\"",
  "HIDE_STEALTH=1 ./build-all.sh \"${{ matrix.kmi }}\"",
  "hidepid-ksu-module-stealth.zip",
]) {
  if (!workflow.includes(text)) {
    throw new Error(`workflow is missing required text: ${text}`);
  }
}

for (const text of [
  "uses: actions/checkout@v4",
  "uses: actions/upload-artifact@v4",
  "uses: actions/download-artifact@v4",
]) {
  if (workflow.includes(text)) {
    throw new Error(`workflow still uses Node 20 action: ${text}`);
  }
}

console.log("GitHub workflow checks passed");
