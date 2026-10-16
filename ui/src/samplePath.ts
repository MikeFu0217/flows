// Replace the literal under the drop point, otherwise insert a quoted path.
// Comments and escaped quotes must not turn into accidental string boundaries.
export function samplePathEdit(source: string, position: number, path: string) {
  const insert = JSON.stringify(path);
  const point = Math.max(0, Math.min(source.length, position));
  let at = 0;
  while (at < source.length) {
    if (source[at] === '#') { const end = source.indexOf('\n', at); at = end < 0 ? source.length : end + 1; continue; }
    const quote = source[at];
    if (quote !== '"' && quote !== "'") { ++at; continue; }
    const from = at++;
    let closed = false;
    while (at < source.length && source[at] !== '\n') {
      if (source[at] === '\\') { at = Math.min(source.length, at + 2); continue; }
      if (source[at++] === quote) { closed = true; break; }
    }
    if (from <= point && (point < at || (!closed && point === at))) return { from, to: at, insert };
  }
  return { from: point, to: point, insert };
}
