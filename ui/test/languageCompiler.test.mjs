import assert from 'node:assert/strict';
import { copyFileSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import ts from 'typescript';
const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const text = ts.transpileModule(readFileSync(join(root, 'ui/src/languageCatalog.ts'), 'utf8'), { compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.ESNext } }).outputText;
const { libraries, functions } = await import(`data:text/javascript;base64,${Buffer.from(text).toString('base64')}`);
const cli = process.argv[2];
assert.ok(cli, 'Pass the built Flows compiler');
const workspace = mkdtempSync(join(tmpdir(), 'flows-language-catalog-'));
let count = 0;
try {
  mkdirSync(join(workspace, 'Samples'));
  copyFileSync(join(root, 'test/fixtures/samples/flows-kick.wav'), join(workspace, 'Samples/kick.wav'));
  const compile = source => {
    const path = join(workspace, 'catalog.flows'); writeFileSync(path, source);
    try { const result = execFileSync(cli, ['compile', path, '--asset-root', workspace], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] }); assert.ok(JSON.parse(result).nodes.length > 0); ++count; }
    catch (error) { throw new Error(`Completion offered a compiler-incompatible call:\n${source}\n${error.stderr || error.message}`); }
  };
  for (const [module, classes] of Object.entries(libraries)) for (const [name, callable] of Object.entries(classes)) {
    const header = `import standard as std\nfrom lib.${module} import ${name}\nInst.sigstream(std.event.keys.poly()) as key:\n    unit = ${name}.make()\n`;
    const required = name === 'Phasor' ? { freq: '440hz' } : name === 'ClassicADSR' ? { gate: 'key.gate' } : name === 'ClassicSampler' ? { file: '"Samples/kick.wav"', trigger: 'key.trigger' } : name === 'ClassicFilt' || name === 'ClassicDistortion' ? { input: 'key.gate' } : name === 'ClassicLFO' ? {} : { pitch: 'key.pitch' };
    for (const p of callable.ports) for (const value of p.options || [p.fallback || required[p.name] || (p.name === 'sync' ? '1/4' : undefined)]) {
      assert.ok(value, `No compiler fixture for ${name}.${p.name}`);
      const args = { ...required, [p.name]: value };
      if (name === 'ClassicLFO' && p.name === 'mode' && ['retrigger', 'one_shot'].includes(value)) args.trigger = 'key.trigger';
      if (name === 'ClassicLFO' && p.name === 'mode' && value === 'transport') args.sync = '1/4';
      if (name === 'ClassicSampler' && p.name === 'gate') args.mode = 'loop';
      compile(header + `    raw = unit(${Object.entries(args).map(([n, v]) => `${n}: ${v}`).join(', ')})\n    return raw * key.gate\n`);
    }
  }
  for (const name of Object.keys(functions)) {
    const analysis = ['power', 'spec', 'spectrum', 'mel_spec'].includes(name);
    compile('import standard as std\nfrom lib.osc import ClassicSine\nInst.sigstream(std.event.keys.poly()) as key:\n    osc = ClassicSine.make()\n    raw = osc(pitch: key.pitch)\n    p = probe(raw)\n' + `    tested = ${name}(${name === 'stereo' ? 'raw, raw' : name === 'pitch_hz' ? 'key.pitch' : name === 'db_gain' ? '-6db' : analysis ? 'p' : 'raw'})\n    return raw * key.gate\n`);
  }
  console.log(`Language catalogue: ${count} offered port/enum/function calls compiled and prepared by the real compiler.`);
} finally { rmSync(workspace, { recursive: true, force: true }); }
