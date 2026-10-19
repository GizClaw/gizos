import { readFileSync, writeFileSync, mkdirSync, copyFileSync } from 'node:fs';
import { basename, join, resolve } from 'node:path';
import { createRequire } from 'node:module';
const [codecPath, mapPath, sdkPath, protocolPath, outputPath, compilerPath, bindingPath, typesDir, licensePath, licenseOutput] = process.argv.slice(2);
const ts = createRequire(import.meta.url)(resolve(compilerPath));
const strip = path => ts.transpileModule(readFileSync(path, 'utf8'), {
  compilerOptions: {target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.ESNext},
}).outputText.replace(/^export\s+(?=(?:function|const|class|let|var)\b)/gm, '')
  .replace(/^export\s*\{\s*\};?\s*$/gm, '');
const exports = ['encodeRPCRequestPayload', 'decodeRPCResponsePayload',
  'encodeClientToolRequestPayload', 'decodeClientToolResponsePayload',
  'decodeClientHwdReadResponsePayload', 'encodeClientHwdWriteRequestPayload',
  'decodeClientHwdWriteResponsePayload', 'RPC_METHOD_IDS', 'CLIENT_TOOL_IDS',
  'CLIENT_TOOL_NAMES', 'CLIENT_HWD_IDS', 'CLIENT_HWD_NAMES', 'encodeBase64', 'decodeBase64',
  'hasClientHwdWriteRequest: hwd => Object.prototype.hasOwnProperty.call(HWD_WRITE_REQUEST_MESSAGES, String(hwd))'];
const identity = JSON.parse(readFileSync(protocolPath, 'utf8'));
const canonical = 'const codec = (() => {\n' + strip(codecPath) + '\n' + strip(mapPath) +
  '\nreturn {' + exports.join(',') + '};\n})();\n';
writeFileSync(outputPath, '// Built from the pinned canonical GizClaw codec and optional host SDK.\n' +
  'const protocol = ' + JSON.stringify(identity) + ';\n' + canonical + readFileSync(sdkPath, 'utf8'));
copyFileSync(licensePath, licenseOutput);
mkdirSync(typesDir, {recursive: true});
const options = {
  target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.ESNext,
  declaration: true, emitDeclarationOnly: true, outDir: resolve(typesDir),
  strict: true, types: [],
};
const host = ts.createCompilerHost(options);
// The upstream browser codec's optional Node base64 branch needs only this
// ambient Buffer surface. It never appears in the emitted public declarations.
const shimName = resolve(codecPath) + '.buffer.d.ts';
const shim = 'declare const Buffer: {from(value: Uint8Array | string, encoding?: string): Uint8Array & {toString(encoding: string): string}};';
const sourceFile = host.getSourceFile.bind(host);
const fileExists = host.fileExists.bind(host);
const readFile = host.readFile.bind(host);
host.getSourceFile = (file, language, ...rest) => resolve(file) === shimName
  ? ts.createSourceFile(file, shim, language, true) : sourceFile(file, language, ...rest);
host.fileExists = file => resolve(file) === shimName || fileExists(file);
host.readFile = file => resolve(file) === shimName ? shim : readFile(file);
const program = ts.createProgram([codecPath, mapPath, shimName], options, host);
const diagnostics = ts.getPreEmitDiagnostics(program).filter(d => d.category === ts.DiagnosticCategory.Error);
if (diagnostics.length) throw new Error(ts.formatDiagnostics(diagnostics, {
  getCanonicalFileName: file => file, getCurrentDirectory: () => process.cwd(), getNewLine: () => '\n',
}));
const emitted = program.emit();
if (emitted.emitSkipped) throw new Error('canonical SDK declarations were not emitted');
copyFileSync(bindingPath, join(typesDir, basename(bindingPath)));
