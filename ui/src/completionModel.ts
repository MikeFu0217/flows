import { Callable, functions, keyFields, knobRegistration, libraries, policies, Port, portTypes, units } from './languageCatalog';

export interface Suggestion { label: string; type: string; detail?: string; info?: string; insert?: string; snippet?: string; boost?: number }
export interface Suggestions { from: number; to: number; options: Suggestion[]; signature?: string; argument?: string }
interface Token { text: string; from: number; to: number; kind: 'word' | 'string' | 'comment' | 'number' | 'punct' }
interface Block { kind: string; name: string; from: number; headerEnd: number; end: number; indent: number; header: Token[] }
interface Symbol { name: string; type: string; callable?: Callable }
const identifier = /^[A-Za-z_]\w*$/;

// Completion runs on unfinished source. This scanner deliberately tolerates
// missing delimiters and strings, while preserving UTF-16 editor positions.
function scan(source: string): Token[] {
  const result: Token[] = [];
  const pattern = /#[^\r\n]*|"(?:[^"\\\r\n]|\\.)*(?:"|(?=\r?\n)|$)|'(?:[^'\\\r\n]|\\.)*(?:'|(?=\r?\n)|$)|[A-G]#\d+|[A-Za-z_]\w*|(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?[A-Za-z]*|[^\s]/g;
  for (const match of source.matchAll(pattern)) {
    const text = match[0], from = match.index!;
    result.push({ text, from, to: from + text.length, kind: text[0] === '#' ? 'comment' : /^["']/.test(text) ? 'string' : identifier.test(text) ? 'word' : /^\d|^\.\d/.test(text) ? 'number' : 'punct' });
  }
  return result;
}
function lineStart(source: string, at: number) { return source.lastIndexOf('\n', at - 1) + 1; }
function indentAt(source: string, at: number) { return source.slice(lineStart(source, at), at).match(/^ */)![0].length; }
function splitArgs(tokens: Token[]): Token[][] {
  const parts: Token[][] = [[]]; let depth = 0;
  for (const token of tokens) {
    if (token.text === ',' && depth === 0) parts.push([]);
    else { parts[parts.length - 1].push(token); if (token.text === '(') ++depth; if (token.text === ')') --depth; }
  }
  return parts;
}
function blocks(source: string, tokens: Token[]): Block[] {
  const result: Block[] = [];
  for (let i = 0; i < tokens.length; ++i) {
    const start = tokens[i], kind = start.text;
    if (!['class', 'def', 'Inst'].includes(kind) || source.slice(lineStart(source, start.from), start.from).trim()) continue;
    let depth = 0, end = i + 1;
    for (; end < tokens.length; ++end) {
      const t = tokens[end].text;
      if (t === '(') ++depth; else if (t === ')') --depth;
      if (t === ':' && depth === 0) break;
      if (depth === 0 && source.slice(tokens[end - 1].to, tokens[end].from).includes('\n')) break;
    }
    if (tokens[end]?.text !== ':' || depth !== 0) continue;
    const headerEnd = tokens[end].to, indent = indentAt(source, start.from);
    let blockEnd = source.length + 1;
    for (let j = end + 1; j < tokens.length; ++j) {
      const t = tokens[j]; if (t.kind === 'comment') continue;
      if (source.slice(tokens[j - 1].to, t.from).includes('\n') && indentAt(source, t.from) <= indent) { blockEnd = lineStart(source, t.from); break; }
    }
    result.push({ kind, name: tokens[i + 1]?.text || '', from: start.from, headerEnd, end: blockEnd, indent, header: tokens.slice(i, end) });
  }
  return result;
}
function typedPorts(header: Token[]): Port[] {
  const start = header.findIndex(t => t.text === '('); if (start < 0) return [];
  const args = splitArgs(header.slice(start + 1, header.map(t => t.text).lastIndexOf(')')));
  return args.filter(part => part[1]?.text === ':').map(part => {
    const eq = part.findIndex(t => t.text === '=');
    return { name: part[0].text, type: part[2]?.text || 'scalar', detail: 'Authored class port', required: eq < 0, fallback: eq < 0 ? undefined : part.slice(eq + 1).map(t => t.text).join('') };
  });
}
function registeredPorts(tokens: Token[], block: Block, ports: Port[]): Port[] {
  const result = ports.map(p => ({ ...p }));
  for (let i = 0; i < tokens.length; ++i) {
    if (tokens[i].text !== 'register_knob' || tokens[i].from <= block.headerEnd || tokens[i].from >= block.end || tokens[i + 1]?.text !== '(') continue;
    let end = i + 2, depth = 1;
    for (; end < tokens.length && depth; ++end) { if (tokens[end].text === '(') ++depth; else if (tokens[end].text === ')') --depth; }
    if (depth) continue;
    const parts = splitArgs(tokens.slice(i + 2, end - 1)), target = parts[0]?.filter(t => t.kind === 'word').pop()?.text;
    const port = result.find(p => p.name === target); if (!port) continue;
    for (const part of parts.slice(1)) {
      if (part[1]?.text !== '=') continue;
      const value = part.slice(2).map(t => t.text).join('');
      if (part[0]?.text === 'default') { port.fallback = value; port.required = false; }
      else if (['range', 'step', 'scale', 'update', 'smoothing'].includes(part[0]?.text)) port.detail += ` · ${part[0].text}=${value}`;
    }
  }
  return result;
}
function description(port: Port) { return `${port.type}${port.required ? ' · required' : port.fallback !== undefined ? ` · default ${port.fallback}` : ''}`; }
function item(name: string, type: string, detail?: string, info?: string): Suggestion { return { label: name, type, detail, info }; }
function signature(call: Callable) { return `${call.name}(${call.ports.map(p => `${p.name}: ${p.type}${p.fallback !== undefined ? ` = ${p.fallback}` : ''}`).join(', ')})`; }

export function completeFlows(source: string, pos: number, explicit = false, samples: readonly { path: string }[] = []): Suggestions | null {
  pos = Math.max(0, Math.min(source.length, pos));
  const all = scan(source), containing = all.find(t => t.from < pos && pos <= t.to);
  if (containing?.kind === 'comment') return null;
  const tokens = all.filter(t => t.kind !== 'comment'), prior = tokens.filter(t => t.to <= pos);
  const bs = blocks(source, tokens);
  const owner = (at: number) => [...bs].reverse().find(b => b.kind !== 'class' && b.headerEnd <= at && at < b.end);
  const current = owner(pos), classBlock = bs.find(b => b.kind === 'class' && b.from < pos && pos < b.end);
  const imports = new Map<string, Callable>(), standards = new Set<string>(), symbols = new Map<string, Symbol>();
  const masked = source.replace(/#[^\r\n]*|"(?:[^"\\\r\n]|\\.)*"|'(?:[^'\\\r\n]|\\.)*'/g, match => ' '.repeat(match.length));
  for (const match of masked.slice(0, pos).matchAll(/^ *import +standard +as +(\w+)/gm)) standards.add(match[1]);
  for (const match of masked.slice(0, pos).matchAll(/^ *from +lib\.(\w+) +import +([^\r\n]*)/gm)) {
    for (const part of match[2].split(',')) {
      const names = part.trim().match(/^(\w+)(?: +as +(\w+))?/), call = names && libraries[match[1]]?.[names[1]];
      if (names && call) imports.set(names[2] || names[1], { ...call, name: names[2] || names[1] });
    }
  }
  for (const b of bs.filter(b => b.kind === 'class' && b.headerEnd < pos)) {
    const base = b.header.findIndex(t => t.text === '(');
    const inherited = base < 0 ? undefined : imports.get(b.header[base + 1]?.text);
    const signal = bs.find(child => child.kind === 'def' && child.name === 'signal' && b.headerEnd < child.from && child.from < b.end);
    const ports = registeredPorts(tokens, b, inherited?.ports || (signal ? typedPorts(signal.header) : []));
    imports.set(b.name, { ...(inherited || { ports }), ports, name: b.name, detail: inherited ? `Subclass of ${inherited.name}` : 'Authored signal composition' });
  }
  let key = '';
  if (current?.kind === 'Inst') {
    const as = current.header.findIndex(t => t.text === 'as'); key = current.header[as + 1]?.text || '';
  }
  if (current?.kind === 'def' && current.name === 'signal') for (const p of typedPorts(current.header)) symbols.set(p.name, { name: p.name, type: p.type });
  for (let i = 0; i + 1 < prior.length; ++i) {
    const t = prior[i]; if (t.kind !== 'word' || prior[i + 1].text !== '=' || source.slice(lineStart(source, t.from), t.from).trim()) continue;
    const scope = owner(t.from); if (scope !== current && scope !== undefined) continue;
    if (scope === undefined && current && !standards.size) continue;
    const type = imports.get(prior[i + 2]?.text);
    const make = prior[i + 3]?.text === '.' && prior[i + 4]?.text === 'make';
    const expression = prior[i + 2]?.text;
    const input = symbols.get(prior[i + 4]?.text || '');
    const analysisType = expression === 'probe' ? 'waveform' : ['spec', 'spectrum', 'mel_spec'].includes(expression) ? 'spectrum' : expression === 'power' ? 'analysis' : input?.type;
    symbols.set(t.text, { name: t.text, type: type && make ? 'instance' : prior[i + 2]?.kind === 'string' ? 'sample' : analysisType || 'signal', callable: type && make ? { ...type, name: t.text } : undefined });
  }
  const prefix = source.slice(0, pos).match(/[A-Za-z_]\w*$/)?.[0] || '';
  const from = pos - prefix.length;
  const to = containing?.kind === 'word' ? containing.to : pos;
  const existingCall = /^\s*\(/.test(source.slice(to));
  const tail = source.slice(lineStart(source, pos), pos);
  const finish = (options: Suggestion[], extra: Partial<Suggestions> = {}): Suggestions => ({ from, to, options, ...extra });

  // Library navigation and imports never suggest unsupported modules.
  const module = tail.match(/^\s*from\s+lib\.([A-Za-z_]\w*)?$/);
  if (module) return finish(Object.keys(libraries).map(n => item(n, 'namespace', 'DSP library')));
  const importNames = tail.match(/^\s*from\s+lib\.(\w+)\s+import\s+([^\r\n]*)$/);
  if (importNames && /\bas\s+\w*$/.test(importNames[2])) return null;
  if (importNames && !/\bas\s+\w*$/.test(importNames[2])) {
    const used = new Set(importNames[2].split(',').slice(0, -1).map(p => p.trim().split(/\s/)[0]));
    return finish(Object.values(libraries[importNames[1]] || {}).filter(c => !used.has(c.name)).map(c => item(c.name, 'class', c.detail, signature(c))));
  }
  if (/^\s*import\s+\w*$/.test(tail)) return finish([item('standard', 'namespace', 'Key event policies', 'import standard as std')]);
  if (/^\s*from\s+[\w.]*$/.test(tail) && !tail.includes('lib.')) return finish([item('lib', 'namespace', 'Built-in DSP libraries')]);

  // An unfinished argument list is still enough to resolve its callable.
  const stack: { open: Token; target: string; parts: Token[] }[] = [];
  for (let i = 0; i < prior.length; ++i) {
    const token = prior[i];
    if (token.text === '(') {
      let j = i - 1, target = '';
      if (prior[j]?.kind === 'word') {
        target = prior[j--].text;
        while (j > 0 && prior[j].text === '.' && prior[j - 1].kind === 'word') { target = prior[j - 1].text + '.' + target; j -= 2; }
      }
      stack.push({ open: token, target, parts: [] });
    } else if (token.text === ')') { stack.pop(); }
    for (const c of stack) if (token.from > c.open.from) c.parts.push(token);
  }
  const call = [...stack].reverse().find(c => !!c.target && c.target !== 'if');
  let callable: Callable | undefined;
  if (call) {
    callable = symbols.get(call.target)?.callable || functions[call.target];
    if (call.target.endsWith('.make') && imports.has(call.target.slice(0, -5))) callable = { name: call.target, detail: 'No arguments; create independent state', ports: [] };
    const policy = call.target.match(/^(\w+)\.event\.keys\.(poly|mono)$/);
    if (policy && standards.has(policy[1])) callable = policies[policy[2]];
    if (call.target === 'self.register_knob' && classBlock) callable = knobRegistration;
  }
  const parts = call ? splitArgs(call.parts) : [], last = parts[parts.length - 1] || [];
  const named = last[1]?.text === ':' || last[1]?.text === '=' ? last[0]?.text : undefined;
  const argument = named ? callable?.ports.find(p => p.name === named) : callable?.ports[parts.length - 1];
  const extra = callable ? { signature: signature(callable), argument: argument?.name } : {};

  if (containing?.kind === 'string') {
    if (argument?.type !== 'sample') return null;
    const quote = containing.text[0], closed = containing.text.length > 1 && containing.text.endsWith(quote);
    if (closed && pos === containing.to) return null;
    return { from: containing.from + 1, to: containing.to - (closed ? 1 : 0), options: samples.map(s => ({ ...item(s.path, 'text', 'Audio sample'), insert: s.path.split('\\').join('\\\\').split(quote).join('\\' + quote) + (closed ? '' : quote) })), ...extra };
  }
  const member = source.slice(0, from).match(/([A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*)\.$/);
  if (member) {
    const target = member[1]; let options: Suggestion[] = [];
    if (key && target === key) options = keyFields.map(p => item(p.name, 'property', p.type, p.detail));
    else if (standards.has(target)) options = [item('event', 'namespace', 'Event sources')];
    else if ([...standards].some(s => target === `${s}.event`)) options = [item('keys', 'namespace', 'Keyboard/MIDI note policies')];
    else if ([...standards].some(s => target === `${s}.event.keys`)) options = Object.values(policies).map(c => ({ ...item(c.name, 'function', c.detail, signature(c)), ...(existingCall ? { insert: c.name } : { snippet: `${c.name}(\$\{\})` }) }));
    else if (imports.has(target)) options = [{ ...item('make', 'function', 'Create independent state per voice'), insert: existingCall ? 'make' : 'make()' }];
    else if (target === 'Inst') options = [{ ...item('sigstream', 'function', 'Instrument signal kernel'), ...(existingCall ? { insert: 'sigstream' } : { snippet: `sigstream(\$\{${[...standards][0] || 'std'}.event.keys.poly()\}) as \$\{key\}:\n    \$\{\}` }) }];
    else if (target === 'self' && classBlock && current?.name === 'init') options = [{ ...item('register_knob', 'function', knobRegistration.detail), ...(existingCall ? { insert: 'register_knob' } : { snippet: 'register_knob(\$\{name\}, default=\$\{0\}, range=(\$\{0\}, \$\{1\}), step=\$\{0.01\})' }) }];
    return finish(options, extra);
  }
  // Unit completion replaces only the suffix, never the numeric literal.
  const numberUnit = source.slice(0, pos).match(/(?:\d+(?:\.\d*)?|\.\d+)([a-zA-Z]*)$/);
  if (containing?.kind === 'number' && numberUnit && (explicit || numberUnit[1])) return finish(units.map(n => item(n, 'unit', 'Numeric unit')), { from: pos - numberUnit[1].length, to: containing.to, ...extra });
  if (/^\s*def\s+signal\([^)]*\w+\s*:\s*\w*$/.test(tail) || (call?.target === 'signal' && classBlock && /^\s*def\s+signal/.test(source.slice(lineStart(source, call.open.from), call.open.from)) && last[1]?.text === ':')) return finish(portTypes.map(n => item(n, 'type', 'Class port type')));
  if (/^\s*def\s+\w*$/.test(tail) && classBlock) return finish([
    { ...item('init', 'function', 'Class control metadata'), snippet: 'init:\n    self.register_knob(\$\{name\}, default=\$\{0\}, range=(\$\{0\}, \$\{1\}))' },
    ...(/\(/.test(source.slice(classBlock.from, classBlock.headerEnd)) ? [] : [{ ...item('signal', 'function', 'Typed signal composition'), snippet: 'signal(\$\{input\}: \$\{audio\}):\n    return \$\{input\}' }]),
  ]);
  if (argument?.options && named) return finish(argument.options.map(n => item(n, 'enum', argument.detail)), extra);

  if (callable && !functions[call!.target] && !named) {
    const used = new Set(parts.slice(0, -1).filter(p => p[1]?.text === ':' || p[1]?.text === '=').map(p => p[0].text));
    if (callable.positional && parts[0]?.length && parts[0][1]?.text !== ':') used.add(callable.ports[0].name);
    const names = callable.ports.filter(p => !used.has(p.name) && !(call!.target !== 'self.register_knob' && ((p.name === 'sync' && used.has('rate')) || (p.name === 'rate' && used.has('sync')))));
    const separator = call!.target === 'self.register_knob' ? '=' : ': ';
    const options: Suggestion[] = names.map(p => ({ ...item(p.name, 'property', description(p), p.detail), insert: p.name + (/^\s*[:=]/.test(source.slice(to)) ? '' : separator), boost: p.required ? 10 : 0 }));
    // At an empty positional slot, signals and named arguments are both useful.
    if (!callable.positional && call!.target !== 'self.register_knob') return finish(options, extra);
    if (call!.target === 'self.register_knob' && parts.length === 1) {
      const own = classBlock && imports.get(classBlock.name);
      return finish((own?.ports || []).filter(p => !['enum', 'sample'].includes(p.type) && !(own?.positional && p.name === 'input')).map(p => item(p.name, 'property', p.type, p.detail)), extra);
    }
    if (call!.target === 'self.register_knob') return finish(options, extra);
    if (prefix || explicit) options.push(...[...symbols.values()].filter(s => !s.callable).map(s => item(s.name, 'variable', s.type)));
    return finish(options, extra);
  }
  if (!explicit && !prefix && !named && !callable) return null;
  const options: Suggestion[] = [];
  if (key) {
    if (argument?.type !== 'sample') options.push(...keyFields.map(p => item(`${key}.${p.name}`, 'property', p.type, p.detail)));
    options.push(item(key, 'variable', 'Key control signals'));
  }
  options.push(...[...symbols.values()].filter(s => argument?.type === 'sample' ? s.type === 'sample' : argument?.type === 'waveform' ? s.type === 'waveform' : argument?.type === 'signal' ? !s.callable && !['waveform', 'analysis', 'spectrum', 'sample'].includes(s.type) : true).map(s => item(s.name, s.callable ? 'function' : 'variable', s.callable ? s.callable.detail : s.type, s.callable ? signature(s.callable) : undefined)));
  if (argument?.type === 'sample') options.push(...samples.map(s => ({ ...item(s.path, 'text', 'Audio sample'), insert: JSON.stringify(s.path) })));
  if (!argument || !['sample', 'pitch', 'hz', 'seconds', 'waveform', 'signal'].includes(argument.type)) options.push(item('true', 'constant', 'Boolean'), item('false', 'constant', 'Boolean'));
  if (!callable || !functions[call!.target]) options.push(...Object.values(functions).map(c => ({ ...item(c.name, 'function', c.detail, signature(c)), ...(existingCall ? { insert: c.name } : { snippet: `${c.name}(\$\{\})` }) })));
  if (!named) {
    options.push(...[...imports].map(([name, c]) => item(name, 'class', c.detail, `${name}.make()`)), ...[...standards].map(s => item(s, 'namespace', 'Standard event sources')));
    options.push(item('Inst', 'class', 'Instrument definition'));
    for (const word of ['import', 'from', 'as', 'return', 'if', 'else', 'class', 'def']) options.push(item(word, 'keyword'));
    options.push({ label: 'instrument', type: 'text', detail: 'Playable sine instrument', snippet: 'import standard as std\nfrom lib.osc import ClassicSine\n\nInst.sigstream(std.event.keys.poly()) as key:\n    osc = ClassicSine.make()\n    raw = osc(pitch: key.pitch, level: -6db)\n    return raw * key.gate * key.velocity\n' });
  }
  return finish([...new Map(options.map(o => [o.label, o])).values()], extra);
}
