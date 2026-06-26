const fs = require("fs");

const file = "src/hidepid_kmod.c";
const source = fs.readFileSync(file, "utf8");

let state = "code";
let line = 1;
let col = 0;
let blockStart = null;
const errors = [];

function pos() {
  return `${file}:${line}:${col + 1}`;
}

for (let i = 0; i < source.length; i++) {
  const ch = source[i];
  const next = source[i + 1];

  if (ch === "\n") {
    line++;
    col = 0;
    if (state === "line-comment") state = "code";
    continue;
  }

  if (state === "code") {
    if (ch === "/" && next === "*") {
      state = "block-comment";
      blockStart = pos();
      i++;
      col += 2;
      continue;
    }
    if (ch === "*" && next === "/") {
      errors.push(`unexpected block comment close at ${pos()}`);
      i++;
      col += 2;
      continue;
    }
    if (ch === "/" && next === "/") {
      state = "line-comment";
      i++;
      col += 2;
      continue;
    }
    if (ch === "\"") {
      state = "string";
      col++;
      continue;
    }
    if (ch === "'") {
      state = "char";
      col++;
      continue;
    }
  } else if (state === "block-comment") {
    if (ch === "*" && next === "/") {
      state = "code";
      blockStart = null;
      i++;
      col += 2;
      continue;
    }
  } else if (state === "string") {
    if (ch === "\\") {
      i++;
      col += 2;
      continue;
    }
    if (ch === "\"") state = "code";
  } else if (state === "char") {
    if (ch === "\\") {
      i++;
      col += 2;
      continue;
    }
    if (ch === "'") state = "code";
  }

  col++;
}

if (state === "block-comment") {
  errors.push(`unterminated block comment opened at ${blockStart}`);
}

if (errors.length) {
  console.error(errors.join("\n"));
  process.exit(1);
}

console.log("C block comments are lexically balanced");
