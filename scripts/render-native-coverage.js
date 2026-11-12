#!/usr/bin/env node
"use strict";

// Deterministic stock-native binding audit. No game data or third-party packages.
const fs = require("fs");
const os = require("os");
const path = require("path");
const crypto = require("crypto");

const ROOT = path.resolve(__dirname, "..");
const STOCK_PATH = path.join(ROOT, "assets", "progress", "stock-natives.json");
const JSON_PATH = path.join(ROOT, "assets", "progress", "native-coverage.json");
const SVG_PATH = path.join(ROOT, "assets", "progress", "native-coverage.svg");
const NATIVES_DIR = path.join(ROOT, "src", "natives");
const STOCK_REPOSITORY = "https://github.com/EliasFD3S/slrr-reverse";
const STOCK_COMMIT = "5c42f3b139aef4c7b9e0b6420c5704e22c0e6cd7";
const STOCK_URL = "https://raw.githubusercontent.com/EliasFD3S/slrr-reverse/" +
  STOCK_COMMIT + "/engine/Core/natives_stubs.cpp";
const EXPECTED_STOCK_COUNT = 376;
const STOCK_SHA256 = "7a09d8f3dd63079dbf50632efd7400aeecebdaa05a786e4958cfc12ad4c6b9f3";
const AUDIT_DATE = "2026-11-11";
const CATEGORIES = [
  ["World", c => ["GroundRef", "PhysicsRef", "Animation", "ParticleSystem"].includes(c.split(".").pop())],
  ["Language", c => c.startsWith("java.lang.")],
  ["Sound & scripts", c => c.startsWith("java.sound.") ||
    (c.startsWith("java.game.") && !c.startsWith("java.game.parts."))],
  ["Resources", c => c.startsWith("java.util.resource.")],
  ["Rendering", c => c.startsWith("java.render.")],
  ["Files & input", c => c.startsWith("java.io.")],
  ["Parts", c => c.startsWith("java.game.parts.")],
];
const PRIMITIVES = {void: "V", int: "I", float: "F"};
const SPECIAL_OBJECTS = {
  String: "java.lang.String", Object: "java.lang.Object",
  Vector3: "java.lang.Vector3", Ypr: "java.lang.Ypr", GameType: "java.lang.GameType",
  Hotkey: "java.io.Hotkey", Osd: "java.render.Osd",
};

function descriptorType(javaType, owners) {
  javaType = javaType.replace(/\s+/g, "");
  if (javaType.endsWith("[]")) return "[" + descriptorType(javaType.slice(0, -2), owners);
  if (PRIMITIVES[javaType]) return PRIMITIVES[javaType];
  if (SPECIAL_OBJECTS[javaType]) return "L" + SPECIAL_OBJECTS[javaType] + ";";
  if (owners[javaType]) return "L" + owners[javaType] + ";";
  throw new Error("unmapped stock native type: " + javaType);
}

function stockDescriptor(typed, owners) {
  const match = typed.match(/^(.+?)\s+[^ (]+\((.*)\)$/);
  if (!match) throw new Error("invalid typed stock signature: " + typed);
  const args = match[2].trim();
  const types = args ? args.split(",").map(value => value.trim().split(/\s+/).slice(0, -1).join("")) : [];
  return "(" + types.map(value => descriptorType(value, owners)).join("") + ")";
}

function parseStockSource(data) {
  const text = data.toString("utf8");
  const expression = /\{"([^"]+)",\s*"([^"]+)",\s*"([^"]+)",\s*(?:true|false),/g;
  const rows = [];
  for (let match; (match = expression.exec(text));) {
    rows.push({class: match[1], method: match[2], typed_signature: match[3]});
  }
  const owners = {};
  for (const row of rows) {
    const simple = row.class.split(".").pop();
    if (owners[simple] && owners[simple] !== row.class) throw new Error("ambiguous stock type: " + simple);
    owners[simple] = row.class;
  }
  for (const row of rows) row.descriptor = stockDescriptor(row.typed_signature, owners);
  if (rows.length !== EXPECTED_STOCK_COUNT)
    throw new Error("pinned source has " + rows.length + " rows, expected " + EXPECTED_STOCK_COUNT);
  const identities = new Set(rows.map(row => row.class + "#" + row.method + "#" + row.descriptor));
  if (identities.size !== rows.length) throw new Error("stock descriptors are not unique");
  const actualSha256 = crypto.createHash("sha256").update(data).digest("hex");
  if (actualSha256 !== STOCK_SHA256)
    throw new Error("stock input SHA-256 does not match the pinned source: " + actualSha256);
  return {
    source: {
      repository: STOCK_REPOSITORY, commit: STOCK_COMMIT, url: STOCK_URL,
      sha256: STOCK_SHA256,
    },
    count: rows.length,
    signatures: rows,
  };
}

function loadStock() {
  const document = JSON.parse(fs.readFileSync(STOCK_PATH, "utf8"));
  if (!document.source || document.source.commit !== STOCK_COMMIT || document.source.url !== STOCK_URL)
    throw new Error("stock manifest does not match the pinned source");
  if (document.source.sha256 !== STOCK_SHA256)
    throw new Error("stock manifest lacks its source SHA-256");
  const rows = document.signatures;
  if (document.count !== EXPECTED_STOCK_COUNT || !Array.isArray(rows) || rows.length !== EXPECTED_STOCK_COUNT)
    throw new Error("stock manifest is incomplete");
  const owners = {};
  for (const row of rows) owners[row.class.split(".").pop()] = row.class;
  const identities = new Set();
  for (const row of rows) {
    const identity = row.class + "#" + row.method + "#" + row.descriptor;
    if (identities.has(identity)) throw new Error("duplicate stock descriptor: " + identity);
    identities.add(identity);
    if (stockDescriptor(row.typed_signature, owners) !== row.descriptor)
      throw new Error("stock descriptor does not match typed signature: " + identity);
  }
  return rows;
}

function stripComments(text) {
  const result = [...text];
  let state = "code";
  for (let i = 0; i < text.length; ++i) {
    const char = text[i], next = text[i + 1] || "";
    if (state === "code") {
      if (char === '"') state = "string";
      else if (char === "'") state = "char";
      else if (char === "/" && next === "/") {
        result[i] = result[i + 1] = " "; ++i; state = "line";
      } else if (char === "/" && next === "*") {
        result[i] = result[i + 1] = " "; ++i; state = "block";
      }
    } else if (state === "string" || state === "char") {
      const delimiter = state === "string" ? '"' : "'";
      if (char === "\\") ++i;
      else if (char === delimiter) state = "code";
    } else if (state === "line") {
      if (char === "\n") state = "code";
      else result[i] = " ";
    } else if (state === "block") {
      if (char === "*" && next === "/") {
        result[i] = result[i + 1] = " "; ++i; state = "code";
      } else if (char !== "\n") result[i] = " ";
    }
  }
  return result.join("");
}

function matching(text, start, opening, closing) {
  let depth = 0, state = "code", delimiter = "";
  for (let i = start; i < text.length; ++i) {
    const char = text[i];
    if (state === "escape") { state = delimiter; continue; }
    if (state !== "code") {
      if (char === "\\") state = "escape";
      else if (char === state) state = "code";
      continue;
    }
    if (char === '"' || char === "'") { delimiter = char; state = char; continue; }
    if (char === opening) ++depth;
    else if (char === closing && --depth === 0) return i;
  }
  throw new Error("unmatched " + opening + " at byte " + start);
}

function splitTopLevel(text, delimiter = ",") {
  const parts = [], stack = [], pairs = {")": "(", "]": "[", "}": "{"};
  let state = "code", start = 0;
  for (let i = 0; i < text.length; ++i) {
    const char = text[i];
    if (state !== "code") {
      if (char === "\\") ++i;
      else if (char === state) state = "code";
    } else if (char === '"' || char === "'") state = char;
    else if ("([{ ".includes(char) && char !== " ") stack.push(char);
    else if (")] }".includes(char) && char !== " ") {
      if (stack.pop() !== pairs[char]) throw new Error("unbalanced C++ expression");
    } else if (char === delimiter && stack.length === 0) {
      parts.push(text.slice(start, i).trim()); start = i + 1;
    }
  }
  parts.push(text.slice(start).trim());
  return parts;
}

function ternaryParts(text) {
  function scan(start, sought) {
    let depth = 0, state = "code";
    for (let i = start; i < text.length; ++i) {
      const char = text[i];
      if (state !== "code") {
        if (char === "\\") ++i;
        else if (char === state) state = "code";
      } else if (char === '"' || char === "'") state = char;
      else if ("([{ ".includes(char) && char !== " ") ++depth;
      else if (")] }".includes(char) && char !== " ") --depth;
      else if (depth === 0 && char === sought) return i;
    }
    return -1;
  }
  const question = scan(0, "?");
  if (question < 0) return null;
  const colon = scan(question + 1, ":");
  if (colon < 0) throw new Error("unterminated ternary: " + text);
  return [text.slice(0, question).trim(), text.slice(question + 1, colon).trim(), text.slice(colon + 1).trim()];
}

function evaluate(expression, environment, constants) {
  expression = expression.trim();
  while (expression.startsWith("(") && matching(expression, 0, "(", ")") === expression.length - 1)
    expression = expression.slice(1, -1).trim();
  const concatenated = splitTopLevel(expression, "+");
  if (concatenated.length > 1)
    return concatenated.map(value => evaluate(value, environment, constants)).join("");
  if (expression.startsWith('"')) {
    let remaining = expression, result = "";
    while (remaining) {
      const literal = remaining.match(/^"(?:\\\\.|[^"\\\\])*"/);
      if (!literal) throw new Error("invalid registration string: " + expression);
      result += JSON.parse(literal[0]);
      remaining = remaining.slice(literal[0].length).trim();
    }
    return result;
  }
  const ternary = ternaryParts(expression);
  if (ternary) {
    const [condition, ifTrue, ifFalse] = ternary;
    let selected;
    const comparison = condition.match(/^(\w+)\s*>\s*(-?\d+)$/);
    if (comparison) selected = Number(environment[comparison[1]]) > Number(comparison[2]);
    else if (typeof environment[condition] === "boolean") selected = environment[condition];
    else throw new Error("unsupported conditional: " + condition);
    return evaluate(selected ? ifTrue : ifFalse, environment, constants);
  }
  if (Object.hasOwn(environment, expression)) {
    if (typeof environment[expression] !== "string") throw new Error("non-string registration value: " + expression);
    return environment[expression];
  }
  if (Object.hasOwn(constants, expression)) return constants[expression];
  throw new Error("unresolved registration expression: " + expression);
}

function registrationLoops(text) {
  const loops = [], expression = /\bfor\s*\(/g;
  for (let match; (match = expression.exec(text));) {
    const open = text.indexOf("(", match.index), close = matching(text, open, "(", ")");
    const header = text.slice(open + 1, close);
    let colon = -1;
    for (let index = 0; index < header.length; ++index) {
      if (header[index] === ":" && header[index - 1] !== ":" && header[index + 1] !== ":") {
        colon = index;
        break;
      }
    }
    if (colon < 0) continue;
    const variable = header.slice(0, colon).match(/(\w+)\s*$/);
    const valuesMatch = header.slice(colon + 1).match(/^\s*\{([^{}]*)\}\s*$/);
    if (!variable) continue;
    const values = [];
    let propertyFirst = false;
    let sourceValues = valuesMatch ? splitTopLevel(valuesMatch[1]) : [];
    if (!valuesMatch && header.includes("std::vector")) {
      sourceValues = [...header.matchAll(/"(?:\\\\.|[^"\\\\])*"/g)].map(match => match[0]);
      propertyFirst = true;
    }
    for (const value of sourceValues) {
      if (value.startsWith('"')) values.push(JSON.parse(value));
      else if (value === "true" || value === "false") values.push(value === "true");
      else if (/^-?\d+$/.test(value)) values.push(Number(value));
      else { values.length = 0; break; }
    }
    if (!values.length) continue;
    let bodyStart = close + 1;
    while (/\s/.test(text[bodyStart] || "")) ++bodyStart;
    const bodyEnd = text[bodyStart] === "{" ? matching(text, bodyStart, "{", "}") : text.indexOf(";", bodyStart);
    loops.push({start: bodyStart, end: bodyEnd, variable: variable[1], values, propertyFirst});
  }
  return loops;
}

function parameterDescriptor(descriptor) {
  const close = descriptor.indexOf(")");
  if (!descriptor.startsWith("(") || close < 0)
    throw new Error("invalid native descriptor: " + descriptor);
  return descriptor.slice(0, close + 1);
}

function parseHostRegistrations() {
  const registrations = new Map();
  const files = [
    ...fs.readdirSync(NATIVES_DIR)
      .filter(name => name.endsWith(".cpp") && name !== "native_registry.cpp")
      .map(name => path.join(NATIVES_DIR, name)),
    path.join(ROOT, "src", "resources", "part_runtime.cpp"),
  ].sort();
  for (const filePath of files) {
    const name = path.basename(filePath), text = stripComments(fs.readFileSync(filePath, "utf8"));
    const constants = {}, constantExpression = /(?:constexpr\s+)?(?:const\s+)?(?:char|auto|std::string)\s*\*?\s*(\w+)\s*(?:\[\s*\])?\s*=\s*("(?:\\.|[^"\\])*")\s*;/g;
    for (let match; (match = constantExpression.exec(text));) constants[match[1]] = JSON.parse(match[2]);
    const loops = registrationLoops(text), registerExpression = /\bregister_method\s*\(/g;
    for (let match; (match = registerExpression.exec(text));) {
      const open = text.indexOf("(", match.index), close = matching(text, open, "(", ")");
      const args = splitTopLevel(text.slice(open + 1, close));
      if (args.length < 4) throw new Error(name + ": incomplete registration at byte " + match.index);
      // These four lambda forwarding bodies are expanded at their call sites below.
      if (name === "part_runtime.cpp" && args[1].trim() === "name" &&
          args[2].trim() === "signature") continue;
      const active = loops.filter(loop => loop.start <= match.index && match.index <= loop.end);
      const combinations = active.reduce((all, loop) =>
        all.flatMap(prefix => loop.values.map(value => [
          ...prefix, [loop.variable, value],
          ...(loop.propertyFirst ? [[loop.variable + ".first", value]] : []),
        ])), [[]]);
      for (const pairs of combinations.length ? combinations : [[]]) {
        const environment = Object.fromEntries(pairs);
        let identity;
        try {
          identity = [0, 1, 2].map(index => evaluate(args[index], environment, constants));
        } catch (error) {
          const line = text.slice(0, match.index).split("\n").length;
          throw new Error("src/natives/" + name + ":" + line + ": " + error.message);
        }
        identity[2] = parameterDescriptor(identity[2]);
        const key = identity.join("#");
        const implementation = args.slice(3).some(value => value.includes("NativeImplementation::ExplicitNoop")) ? "noop" : "body";
        if (registrations.has(key) && registrations.get(key) !== implementation)
          throw new Error("conflicting host registration: " + key);
        registrations.set(key, implementation);
      }
    }
    if (name === "part_runtime.cpp") {
      const owners = {
        bind: "java.game.parts.Part",
        chassis: "java.game.parts.bodypart.Chassis",
        sfx: "java.game.parts.SfxTable",
        wheel: "java.game.parts.WheelRef",
      };
      const helperExpression = /\b(bind|chassis|sfx|wheel)\s*\(/g;
      for (let match; (match = helperExpression.exec(text));) {
        const open = text.indexOf("(", match.index), close = matching(text, open, "(", ")");
        const args = splitTopLevel(text.slice(open + 1, close));
        if (args.length < 3) continue;
        const active = loops.filter(loop => loop.start <= match.index && match.index <= loop.end);
        const combinations = active.reduce((all, loop) =>
          all.flatMap(prefix => loop.values.map(value => [
            ...prefix, [loop.variable, value],
            ...(loop.propertyFirst ? [[loop.variable + ".first", value]] : []),
          ])), [[]]);
        for (const pairs of combinations.length ? combinations : [[]]) {
          const environment = Object.fromEntries(pairs);
          let identity;
          try {
            identity = [owners[match[1]], ...[0, 1].map(index =>
              evaluate(args[index], environment, constants))];
          } catch (error) {
            const line = text.slice(0, match.index).split("\n").length;
            throw new Error("src/resources/part_runtime.cpp:" + line + ": " + error.message);
          }
          identity[2] = parameterDescriptor(identity[2]);
          registrations.set(identity.join("#"), "body");
        }
      }
    }
  }
  return registrations;
}

function categoryFor(className) {
  // World is a deliberate subset of java.util.resource and precedes Resources.
  const match = CATEGORIES.find(([, predicate]) => predicate(className));
  if (!match) throw new Error("stock owner has no category: " + className);
  return match[0];
}

function coverage() {
  const stock = loadStock(), host = parseHostRegistrations();
  const counts = Object.fromEntries(CATEGORIES.map(([name]) => [name, {body: 0, noop: 0, missing: 0}]));
  for (const row of stock) {
    const key = row.class + "#" + row.method + "#" + parameterDescriptor(row.descriptor);
    counts[categoryFor(row.class)][host.get(key) || "missing"]++;
  }
  const categories = CATEGORIES.map(([name]) => ({name, ...counts[name]}));
  const total = categories.reduce((sum, row) => sum + row.body + row.noop + row.missing, 0);
  if (total !== EXPECTED_STOCK_COUNT) throw new Error("categorized " + total + " rows, expected " + EXPECTED_STOCK_COUNT);
  return {
    updated: AUDIT_DATE,
    total,
    scope: "Stock native table (assets/progress/stock-natives.json, pinned slrr-reverse " +
      STOCK_COMMIT + "), matched against registered class/method/parameter signatures. " +
      "Explicit native_noop bindings are counted separately. Bodies can be partial " +
      "implementations or return constants; these are binding counts, not " +
      "gameplay-completion or validation scores.",
    categories,
  };
}

function jsonText(document) { return JSON.stringify(document, null, 2) + "\n"; }

// Git for Windows commonly checks text files out with CRLF line endings.
// Coverage drift is about generated content, not the checkout's newline policy.
function canonicalText(text) { return text.replace(/\r\n?/g, "\n"); }

function svgText(document) {
  const totals = Object.fromEntries(["body", "noop", "missing"].map(key =>
    [key, document.categories.reduce((sum, row) => sum + row[key], 0)]));
  const total = document.total;
  const lines = [
    '<svg xmlns="http://www.w3.org/2000/svg" width="1120" height="430" viewBox="0 0 1120 430" role="img" aria-labelledby="title desc">',
    '<title id="title">Open SLRR native binding coverage</title>',
    `<desc id="desc">${totals.body} with a body, ${totals.noop} no-op, ${totals.missing} missing, out of ${total}. Bars show counts, not percentages of game completion.</desc>`,
    '<rect width="1120" height="430" rx="12" fill="#0d1117"/>',
    '<g font-family="Segoe UI,Arial,sans-serif">',
    '<text x="28" y="34" font-size="21" fill="#f0f6fc" text-anchor="start">Native binding coverage</text>',
    `<text x="28" y="58" font-size="13" fill="#c9d1d9" text-anchor="start">${totals.body} with a body  /  ${totals.noop} no-op  /  ${totals.missing} missing  ·  ${total} stock natives</text>`,
  ];
  for (let tick = 0; tick < 5; ++tick) {
    const x = 170 + tick * 217.5;
    lines.push(`<line x1="${x.toFixed(1)}" x2="${x.toFixed(1)}" y1="80" y2="326" stroke="#21262d"/>`);
    lines.push(`<text x="${x.toFixed(1)}" y="346" font-size="12" fill="#8b949e" text-anchor="middle">${tick * 25}</text>`);
  }
  const colors = {body: "#49a58d", noop: "#d39338", missing: "#ab474b"};
  const labels = {body: "with a body", noop: "no-op", missing: "missing"};
  document.categories.forEach((row, index) => {
    const y = 85 + index * 35, escapedName = row.name.replace("&", "&amp;");
    lines.push(`<text x="156" y="${y + 17}" font-size="13" fill="#c9d1d9" text-anchor="end">${escapedName}</text>`);
    let x = 170;
    for (const key of ["body", "noop", "missing"]) {
      const count = row[key];
      if (!count) continue;
      const width = count * 8.7;
      lines.push(`<rect x="${x}" y="${y}" width="${width}" height="25" fill="${colors[key]}"><title>${escapedName}: ${count} ${labels[key]}</title></rect>`);
      const centered = width >= 17.4;
      lines.push(`<text x="${centered ? x + width / 2 : x + width + 10}" y="${y + 17}" font-size="12" fill="${centered ? "#ffffff" : "#8b949e"}" text-anchor="${centered ? "middle" : "start"}">${count}</text>`);
      x += width;
    }
    lines.push(`<text x="${x + 10}" y="${y + 17}" font-size="12" fill="#8b949e" text-anchor="start">${row.body + row.noop + row.missing}</text>`);
  });
  lines.push(
    '<text x="1040" y="369" font-size="12" fill="#8b949e" text-anchor="end">Number of natives</text>',
    '<rect x="170" y="380" width="12" height="12" rx="2" fill="#49a58d"/>',
    '<text x="190" y="391" font-size="13" fill="#c9d1d9" text-anchor="start">With a body</text>',
    '<rect x="360" y="380" width="12" height="12" rx="2" fill="#d39338"/>',
    '<text x="380" y="391" font-size="13" fill="#c9d1d9" text-anchor="start">No-op</text>',
    '<rect x="493" y="380" width="12" height="12" rx="2" fill="#ab474b"/>',
    '<text x="513" y="391" font-size="13" fill="#c9d1d9" text-anchor="start">Missing</text>',
    `<text x="1090" y="411" font-size="11" fill="#8b949e" text-anchor="end">Audited ${AUDIT_DATE} · Bodies may be partial implementations</text>`,
    '</g>', '</svg>');
  return lines.join("\n") + "\n";
}

function writeOutputs(jsonPath, svgPath) {
  const document = coverage();
  fs.writeFileSync(jsonPath, jsonText(document), "utf8");
  fs.writeFileSync(svgPath, svgText(document), "utf8");
}

function checkOutputs() {
  const document = coverage(), expected = new Map([[JSON_PATH, jsonText(document)], [SVG_PATH, svgText(document)]]);
  let clean = true;
  for (const [filePath, text] of expected) {
    if (!fs.existsSync(filePath) || canonicalText(fs.readFileSync(filePath, "utf8")) !== text) {
      console.log("DRIFT: " + path.relative(ROOT, filePath) + "; run node scripts/render-native-coverage.js");
      clean = false;
    }
  }
  if (clean) console.log("Native coverage JSON and SVG are reproducible and up to date.");
  return clean;
}

function selfTest() {
  const document = coverage();
  if (canonicalText(jsonText(document).replace(/\n/g, "\r\n")) !== jsonText(document))
    throw new Error("CRLF checkout normalization changed the coverage comparison");
  if (document.total !== EXPECTED_STOCK_COUNT || document.categories.length !== CATEGORIES.length)
    throw new Error("coverage totals failed self-test");
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), "native-coverage-test-"));
  try {
    const firstJson = path.join(directory, "first.json"), firstSvg = path.join(directory, "first.svg");
    const secondJson = path.join(directory, "second.json"), secondSvg = path.join(directory, "second.svg");
    writeOutputs(firstJson, firstSvg); writeOutputs(secondJson, secondSvg);
    if (!fs.readFileSync(firstJson).equals(fs.readFileSync(secondJson)) ||
        !fs.readFileSync(firstSvg).equals(fs.readFileSync(secondSvg)))
      throw new Error("generator output is not deterministic");
    if (JSON.stringify(JSON.parse(fs.readFileSync(firstJson, "utf8"))) !== JSON.stringify(document))
      throw new Error("generated JSON does not parse to the coverage model");
  } finally {
    fs.rmSync(directory, {recursive: true, force: true});
  }
  console.log("Native coverage generator determinism self-test passed.");
}

function main() {
  const args = process.argv.slice(2);
  if (args[0] === "--import-stock" && args.length === 2) {
    const document = parseStockSource(fs.readFileSync(args[1]));
    fs.writeFileSync(STOCK_PATH, jsonText(document), "utf8");
    console.log("Wrote " + path.relative(ROOT, STOCK_PATH) + " (" + document.count + " stock signatures)");
    return;
  }
  if (args[0] === "--self-test" && args.length === 1) { selfTest(); return; }
  if (args[0] === "--check" && args.length === 1) { process.exitCode = checkOutputs() ? 0 : 1; return; }
  if (args.length !== 0) throw new Error("usage: node scripts/render-native-coverage.js [--check|--self-test|--import-stock FILE]");
  writeOutputs(JSON_PATH, SVG_PATH);
  console.log("Wrote " + path.relative(ROOT, JSON_PATH) + " and " + path.relative(ROOT, SVG_PATH));
}

try { main(); } catch (error) {
  console.error("native coverage: " + error.message);
  process.exitCode = 2;
}
