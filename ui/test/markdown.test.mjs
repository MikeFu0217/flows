import assert from 'node:assert/strict';
import { mkdtemp, rm, writeFile } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { build } from 'esbuild';
import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';

// Compile the real component, with the same React package as the SSR renderer.
// Native requests are not invoked during rendering; a browser-free window only
// makes the existing native transport's feature detection available.
const directory = await mkdtemp(join(dirname(fileURLToPath(import.meta.url)), '.markdown-test-'));
globalThis.window = {};
try {
  const output = join(directory, 'Markdown.mjs');
  const bundle = await build({
    entryPoints: [fileURLToPath(new URL('../src/Markdown.tsx', import.meta.url))],
    outfile: output, write: false, bundle: true, packages: 'external',
    platform: 'node', format: 'esm', target: 'node18', loader: { '.css': 'empty' },
  });
  await writeFile(output, bundle.outputFiles[0].text);
  const { Markdown, safeMarkdownUrl } = await import(pathToFileURL(output).href);
  const render = text => renderToStaticMarkup(createElement(Markdown, { text }));

  const standard = render('# Signal flow\n\n**Bass** and *motion* with `key.pitch`.\n\n> Observe before changing.\n\n1. Play\n2. Inspect\n\n- Low pass\n- ~~Disabled~~\n\n---');
  for (const expected of ['<h1>Signal flow</h1>', '<strong>Bass</strong>', '<em>motion</em>', '<code>key.pitch</code>', '<blockquote>', '<ol>', '<ul>', '<del>Disabled</del>', '<hr/>']) assert.ok(standard.includes(expected), expected);
  const table = render('| Signal | Role |\n| --- | --- |\n| raw | audio |\n| cutoff | control |');
  for (const expected of ['markdown-table-scroll', 'aria-label="Markdown table"', '<table>', '<thead>', '<th>Signal</th>', '<td>audio</td>']) assert.ok(table.includes(expected), expected);
  const tasks = render('- [x] Compiled\n- [ ] Listen');
  assert.ok(tasks.includes('type="checkbox"'));
  assert.ok(tasks.includes('checked=""'));
  assert.ok(tasks.includes('disabled=""'));

  const code = render('```flows\n# bass\nraw = osc(pitch: key.pitch, level: -6db)\nreturn raw * key.gate\n```');
  for (const expected of ['markdown-code-block', 'title="Copy code"', '<pre>', '<code class="language-flows">', 'markdown-token-comment', 'markdown-token-function', 'markdown-token-number', 'markdown-token-keyword']) assert.ok(code.includes(expected), expected);
  assert.ok(render('```html\n<script>alert("example")</script>\n```').includes('&lt;script&gt;'));
  assert.ok(render('```unknown\nraw < output\n```').includes('raw &lt; output'));
  assert.ok(render('```flows\nraw = osc(').includes('markdown-code-block'), 'An incomplete streamed fence remains readable.');

  for (const href of ['https://example.com/path?q=1#value', 'http://example.com/', 'HTTPS://EXAMPLE.COM/']) {
    assert.ok(safeMarkdownUrl(href));
    const link = render(`[Reference](${href})`);
    assert.ok(link.includes('href="http'));
    assert.ok(link.includes('target="_blank"'));
    assert.ok(link.includes('rel="noopener noreferrer"'));
  }
  assert.ok(render('https://example.com').includes('<a href="https://example.com/"'), 'GFM automatic URL links work.');
  const rejected = ['javascript:alert(1)', 'data:text/html;base64,PHNjcmlwdD4=', 'file:///etc/passwd', 'ftp://example.com/', 'codex://settings', '//example.com/path', '/local', '#signal', 'https://user:password@example.com/', 'https://example.com/\nunsafe', 'https:\\example.com', 'https://'];
  for (const href of rejected) assert.equal(safeMarkdownUrl(href), '', href);
  const unsafe = render('[script](javascript:alert%281%29) [data](data:text/html;base64,eA==) [local](file:///etc/passwd) [relative](/assets)');
  assert.ok(!unsafe.includes('href='), 'Unsafe and relative destinations never create clickable links.');
  const raw = render('<script>alert(1)</script>\n\n<img src="https://tracker.example/pixel" onerror="alert(1)">\n\n<iframe src="https://example.com"></iframe>\n\n<style>body{display:none}</style>');
  for (const tag of ['<script', '<img', '<iframe', '<style']) assert.ok(!raw.includes(tag), tag);
  const image = render('![Spectrum](https://tracker.example/pixel)\n\n![Payload](data:image/svg+xml;base64,PHN2Zz4=)');
  assert.ok(!image.includes('<img'), 'Images must not make automatic remote requests.');
  assert.ok(!image.includes('src='));
  assert.ok(image.includes('Image: Spectrum'));
  assert.ok(image.includes('href="https://tracker.example/pixel"'));
  assert.ok(!image.includes('data:image'));
  console.log('Markdown rendering: CommonMark/GFM, code, streaming fences, safe links, HTML and remote image handling passed.');
} finally {
  delete globalThis.window;
  await rm(directory, { recursive: true, force: true });
}
