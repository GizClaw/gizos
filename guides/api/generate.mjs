import { execFileSync } from "node:child_process";
import { existsSync, mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { XMLParser } from "fast-xml-parser";

import { apiSources } from "./sources.mjs";

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const options = new Map(
  process.argv.slice(2).map((argument) => {
    const separator = argument.indexOf("=");
    if (!argument.startsWith("--") || separator < 3) {
      throw new Error(`Invalid argument: ${argument}`);
    }
    return [argument.slice(0, separator), argument.slice(separator + 1)];
  }),
);
const repositoryRoot = path.resolve(
  options.get("--repository-root") ?? path.resolve(scriptDirectory, "../.."),
);
const generatedRoot = path.resolve(
  repositoryRoot,
  options.get("--generated-root") ?? "guides/.generated",
);
const xmlRoot = path.join(generatedRoot, "doxygen/xml");
const markdownRoot = path.join(generatedRoot, "api");
const doxyfile = path.join(repositoryRoot, "guides/api/Doxyfile");

const parser = new XMLParser({
  ignoreAttributes: false,
  parseTagValue: false,
  trimValues: false,
});
const orderedParser = new XMLParser({
  ignoreAttributes: false,
  parseTagValue: false,
  preserveOrder: true,
  trimValues: false,
});
const orderedElements = new Map();
const sourceLines = new Map();

function asArray(value) {
  if (value === undefined || value === null) {
    return [];
  }
  return Array.isArray(value) ? value : [value];
}

function normalizedPath(value) {
  return value.replaceAll("\\", "/").replace(/^\.\//, "");
}

function walkHeaders(relativeRoot) {
  const absoluteRoot = path.join(repositoryRoot, relativeRoot);
  const entries = readdirSync(absoluteRoot, { withFileTypes: true });
  return entries.flatMap((entry) => {
    const child = path.join(relativeRoot, entry.name);
    if (entry.isDirectory()) {
      return walkHeaders(child);
    }
    return entry.isFile() && entry.name.endsWith(".h") ? [normalizedPath(child)] : [];
  });
}

function sourceHeaders(source) {
  if (source.headers) {
    return source.headers.map(normalizedPath);
  }
  return source.roots.flatMap(walkHeaders).sort();
}

function runDoxygen() {
  rmSync(path.join(generatedRoot, "doxygen"), { recursive: true, force: true });
  mkdirSync(generatedRoot, { recursive: true });

  try {
    execFileSync("doxygen", [doxyfile], {
      cwd: repositoryRoot,
      env: {
        ...process.env,
        H2_GUIDES_GENERATED_ROOT: generatedRoot,
      },
      stdio: "inherit",
    });
  } catch (error) {
    if (error.code !== "ENOENT") {
      throw error;
    }
    try {
      execFileSync(
        "docker",
        [
          "run",
          "--rm",
          "-v",
          `${repositoryRoot}:/workspace`,
          "-w",
          "/workspace",
          "alpine:3.22",
          "sh",
          "-c",
          "apk add --no-cache doxygen >/dev/null && doxygen guides/api/Doxyfile",
        ],
        { cwd: repositoryRoot, stdio: "inherit" },
      );
    } catch (dockerError) {
      if (dockerError.code === "ENOENT") {
        throw new Error(
          "没有找到 doxygen 或 docker。请先安装 Doxygen，再运行 npm run guides:api。macOS 可以使用 brew install doxygen。",
        );
      }
      throw dockerError;
    }
  }
}

function flattenText(value) {
  if (value === undefined || value === null) {
    return "";
  }
  if (typeof value === "string" || typeof value === "number") {
    return String(value);
  }
  if (Array.isArray(value)) {
    return value.map(flattenText).join(" ");
  }
  if (typeof value === "object") {
    return Object.entries(value)
      .filter(([key]) => !key.startsWith("@_"))
      .map(([key, child]) => (key === "sp" ? " " : flattenText(child)))
      .join(" ");
  }
  return "";
}

function cleanText(value) {
  return flattenText(value)
    .replace(/\s+([,.;:)])/g, "$1")
    .replace(/([(])\s+/g, "$1")
    .replace(/\s+/g, " ")
    .trim();
}

function indexOrderedElements(nodes) {
  if (!Array.isArray(nodes)) {
    return;
  }
  for (const node of nodes) {
    if (!node || typeof node !== "object") {
      continue;
    }
    const id = node[":@"]?.["@_id"];
    for (const [key, children] of Object.entries(node)) {
      if (key === ":@" || !Array.isArray(children)) {
        continue;
      }
      if (id) {
        orderedElements.set(id, children);
      }
      indexOrderedElements(children);
    }
  }
}

function orderedChild(nodes, key) {
  if (!Array.isArray(nodes)) {
    return [];
  }
  return nodes.flatMap((node) => (Array.isArray(node?.[key]) ? node[key] : []));
}

function orderedText(nodes, excludedKeys = new Set()) {
  if (!Array.isArray(nodes)) {
    return "";
  }
  let output = "";
  for (const node of nodes) {
    if (!node || typeof node !== "object") {
      continue;
    }
    if (typeof node["#text"] === "string") {
      output += node["#text"];
    }
    for (const [key, children] of Object.entries(node)) {
      if (key === "#text" || key === ":@" || excludedKeys.has(key)) {
        continue;
      }
      output += orderedText(children, excludedKeys);
    }
  }
  return output.replace(/\s+/g, " ").trim();
}

function proseOnly(value) {
  if (Array.isArray(value)) {
    return value.map(proseOnly);
  }
  if (value && typeof value === "object") {
    return Object.fromEntries(
      Object.entries(value)
        .filter(([key]) => !["parameterlist", "simplesect"].includes(key))
        .map(([key, child]) => [key, proseOnly(child)]),
    );
  }
  return value;
}

function description(member) {
  const ordered = orderedElements.get(member["@_id"]);
  if (ordered) {
    return [
      orderedText(orderedChild(ordered, "briefdescription")),
      orderedText(
        orderedChild(ordered, "detaileddescription"),
        new Set(["parameterlist", "simplesect"]),
      ),
    ]
      .filter(Boolean)
      .filter((text, index, values) => values.indexOf(text) === index)
      .join("\n\n");
  }
  return [cleanText(member.briefdescription), cleanText(proseOnly(member.detaileddescription))]
    .filter(Boolean)
    .filter((text, index, values) => values.indexOf(text) === index)
    .join("\n\n");
}

function findNodes(value, key, output = []) {
  if (Array.isArray(value)) {
    for (const child of value) {
      findNodes(child, key, output);
    }
    return output;
  }
  if (!value || typeof value !== "object") {
    return output;
  }
  for (const [childKey, child] of Object.entries(value)) {
    if (childKey === key) {
      output.push(...asArray(child));
    }
    findNodes(child, key, output);
  }
  return output;
}

function renderFunctionDetails(member) {
  const output = [];
  const parameterLists = findNodes(member.detaileddescription, "parameterlist")
    .filter((list) => list["@_kind"] === "param");
  const parameters = parameterLists.flatMap((list) => asArray(list.parameteritem));
  if (parameters.length > 0) {
    output.push("**Parameters**", "", "| Name | Description |", "| --- | --- |");
    for (const parameter of parameters) {
      const names = asArray(parameter.parameternamelist?.parametername)
        .map(cleanText)
        .filter(Boolean)
        .map((name) => `\`${name}\``)
        .join(", ");
      output.push(`| ${names} | ${escapeTable(cleanText(parameter.parameterdescription))} |`);
    }
    output.push("");
  }

  const returns = findNodes(member.detaileddescription, "simplesect")
    .filter((section) => section["@_kind"] === "return")
    .map(cleanText)
    .filter(Boolean);
  if (returns.length > 0) {
    output.push("**Returns**", "", returns.join(" "), "");
  }
  return output;
}

function escapeTable(value) {
  return value.replaceAll("|", "\\|").replaceAll("\n", " ");
}

function codeBlock(code) {
  return `\n\`\`\`c\n${code.trim()}\n\`\`\`\n`;
}

function sourceDeclaration(compound) {
  const file = compoundFile(compound);
  const start = Number(compound.location?.["@_bodystart"]);
  const end = Number(compound.location?.["@_bodyend"]);
  if (!file || !Number.isInteger(start) || !Number.isInteger(end) || start <= 0 || end < start) {
    return "";
  }
  if (!sourceLines.has(file)) {
    sourceLines.set(file, readFileSync(path.join(repositoryRoot, file), "utf8").split("\n"));
  }
  return sourceLines.get(file).slice(start - 1, end).join("\n");
}

function memberSignature(member) {
  const definition = cleanText(member.definition);
  const args = cleanText(member.argsstring);
  const initializer = cleanText(member.initializer);

  if (member["@_kind"] === "define") {
    return `#define ${cleanText(member.name)}${args}${initializer ? ` ${initializer}` : ""}`;
  }
  if (member["@_kind"] === "variable") {
    const type = cleanText(member.type);
    return `${type}${type ? " " : ""}${cleanText(member.name)}${args}${initializer ? ` ${initializer}` : ""};`;
  }
  return `${definition}${args}${definition || args ? ";" : ""}`;
}

function renderEnum(member) {
  const name = cleanText(member.name);
  const values = asArray(member.enumvalue);
  const lines = [`enum ${name} {`];
  for (const value of values) {
    const initializer = cleanText(value.initializer);
    lines.push(`    ${cleanText(value.name)}${initializer ? ` ${initializer}` : ""},`);
  }
  lines.push("};");

  const output = [`### \`${name}\``, codeBlock(lines.join("\n"))];
  const docs = description(member);
  if (docs) {
    output.push(docs, "");
  }

  const documentedValues = values.filter((value) => description(value));
  if (documentedValues.length > 0) {
    output.push("| Value | Description |", "| --- | --- |");
    for (const value of documentedValues) {
      output.push(`| \`${cleanText(value.name)}\` | ${escapeTable(description(value))} |`);
    }
    output.push("");
  }
  return output;
}

function renderMember(member) {
  if (member["@_kind"] === "enum") {
    return renderEnum(member);
  }

  const name = cleanText(member.name);
  const output = [`### \`${name}\``, codeBlock(memberSignature(member))];
  const docs = description(member);
  if (docs) {
    output.push(docs, "");
  }
  if (member["@_kind"] === "function") {
    output.push(...renderFunctionDetails(member));
  }
  return output;
}

function renderStruct(compound) {
  const name = cleanText(compound.compoundname);
  const fields = asArray(compound.sectiondef)
    .flatMap((section) => asArray(section.memberdef))
    .filter((member) => member["@_kind"] === "variable");
  const keyword = compound["@_kind"] === "union" ? "union" : "struct";
  const lines = [`${keyword} ${name} {`];
  for (const field of fields) {
    lines.push(`    ${memberSignature(field)}`);
  }
  lines.push("};");
  const declaration = sourceDeclaration(compound) || lines.join("\n");

  const output = [`### \`${name}\``, codeBlock(declaration)];
  const docs = description(compound);
  if (docs) {
    output.push(docs, "");
  }

  const documentedFields = fields.filter((field) => description(field));
  if (documentedFields.length > 0) {
    output.push("| Field | Description |", "| --- | --- |");
    for (const field of documentedFields) {
      output.push(`| \`${cleanText(field.name)}\` | ${escapeTable(description(field))} |`);
    }
    output.push("");
  }
  return output;
}

function isHeaderGuard(member) {
  if (member["@_kind"] !== "define" || cleanText(member.initializer)) {
    return false;
  }
  return /(?:_H|_H_)$/.test(cleanText(member.name));
}

function readCompounds() {
  orderedElements.clear();
  return readdirSync(xmlRoot)
    .filter((name) => name.endsWith(".xml") && name !== "index.xml")
    .map((name) => {
      const xml = readFileSync(path.join(xmlRoot, name), "utf8");
      indexOrderedElements(orderedParser.parse(xml));
      const document = parser.parse(xml);
      return document.doxygen?.compounddef;
    })
    .filter(Boolean);
}

function compoundFile(compound) {
  return normalizedPath(compound.location?.["@_file"] ?? "");
}

function renderHeader(header, compounds) {
  const fileCompound = compounds.find(
    (compound) => compound["@_kind"] === "file" && compoundFile(compound) === header,
  );
  const typeCompounds = compounds.filter(
    (compound) => ["struct", "union"].includes(compound["@_kind"]) && compoundFile(compound) === header,
  );
  const members = asArray(fileCompound?.sectiondef)
    .flatMap((section) => asArray(section.memberdef))
    .filter((member) => !isHeaderGuard(member));

  const categories = [
    ["Macros", members.filter((member) => member["@_kind"] === "define")],
    ["Types", members.filter((member) => ["typedef", "enum"].includes(member["@_kind"]))],
    ["Data Structures", typeCompounds],
    ["Functions", members.filter((member) => member["@_kind"] === "function")],
    ["Variables", members.filter((member) => member["@_kind"] === "variable")],
  ];

  const output = [`## \`${path.basename(header)}\``, "", `Source: \`${header}\``, ""];
  const fileDocs = description(fileCompound ?? {});
  if (fileDocs) {
    output.push(fileDocs, "");
  }

  for (const [title, items] of categories) {
    if (items.length === 0) {
      continue;
    }
    output.push(`### ${title}`, "");
    for (const item of items) {
      const rendered = ["struct", "union"].includes(item["@_kind"])
        ? renderStruct(item)
        : renderMember(item);
      rendered[0] = rendered[0].replace(/^### /, "#### ");
      output.push(...rendered);
    }
  }
  return output;
}

function renderPackage(source, compounds) {
  const headers = sourceHeaders(source);
  const output = [];

  output.push(
    "> [!NOTE] 生产 Public API",
    ">",
    "> 本页由下列生产 Public Headers 的 Doxygen XML 自动生成。声明与 API 注释的 source of truth 均为项目中的头文件。",
    "",
  );

  for (const header of headers) {
    output.push(...renderHeader(header, compounds));
  }
  return `${output.join("\n").trim()}\n`;
}

if (process.env.GUIDES_API_SKIP_DOXYGEN !== "1") {
  runDoxygen();
}

if (!existsSync(path.join(xmlRoot, "index.xml"))) {
  throw new Error(`Doxygen XML 不存在：${path.join(xmlRoot, "index.xml")}`);
}

const compounds = readCompounds();
rmSync(markdownRoot, { recursive: true, force: true });
mkdirSync(markdownRoot, { recursive: true });

for (const source of apiSources) {
  writeFileSync(path.join(markdownRoot, `${source.id}.md`), renderPackage(source, compounds));
}

console.log(`Generated ${apiSources.length} API reference pages in guides/.generated/api.`);
