// Documentation link check: every relative link and #anchor in the
// project's Markdown files (git-tracked and new, without third-party lib/)
// must resolve - Markdown links/images and HTML src/href attributes.
// Anchors follow GitHub's heading ids (lower case, punctuation removed,
// spaces -> '-', duplicates -1, -2 ...), like tools/help_md.py gh_slug().
const test = require('node:test');
const assert = require('node:assert');
const {execFileSync} = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');

const root = path.join(__dirname, '..');

function markdownFiles() {
  const out = execFileSync('git', ['ls-files', '--cached', '--others', '--exclude-standard', '*.md'], {cwd: root, encoding: 'utf8'});
  return out.split('\n').filter((f) => f && !f.startsWith('lib/') && fs.existsSync(path.join(root, f)));
}

// Lines outside fenced code blocks; keepCode = false also drops inline code.
function textLines(text, keepCode = false) {
  const lines = [];
  let fence = false;
  for (const line of text.split('\n')) {
    if (/^\s*```/.test(line)) { fence = !fence; continue; }
    if (!fence) lines.push(keepCode ? line : line.replace(/`[^`]*`/g, ''));
  }
  return lines;
}

const anchorCache = new Map();
function anchors(file) {
  if (!anchorCache.has(file)) {
    const seen = new Map(), ids = new Set();
    for (const line of textLines(fs.readFileSync(file, 'utf8'), true)) {
      const m = /^#{1,6}\s+(.*?)\s*#*\s*$/.exec(line);
      if (!m) continue;
      const slug = m[1].trim().toLowerCase().replace(/[^\p{L}\p{N}_\- ]/gu, '').replace(/ /g, '-');
      const n = seen.get(slug);
      seen.set(slug, n === undefined ? 0 : n + 1);
      ids.add(n === undefined ? slug : `${slug}-${n + 1}`);
    }
    for (const line of fs.readFileSync(file, 'utf8').split('\n'))   // explicit <a id="..."> / name=
      for (const m of line.matchAll(/<a\s+(?:id|name)="([^"]+)"/g)) ids.add(m[1]);
    anchorCache.set(file, ids);
  }
  return anchorCache.get(file);
}

function links(file) {
  const raw = fs.readFileSync(file, 'utf8');
  const lines = textLines(raw.replace(/<!--[\s\S]*?-->/g, (c) => c.replace(/[^\n]/g, '')));
  const out = [];
  lines.forEach((line, i) => {
    for (const m of line.matchAll(/!?\[[^\]]*\]\(\s*<?([^)\s>]+)>?(?:\s+"[^"]*")?\s*\)/g)) out.push({target: m[1], line: i + 1});
    for (const m of line.matchAll(/\b(?:src|href)="([^"]+)"/g)) out.push({target: m[1], line: i + 1});
  });
  return out;
}

test('relative links and anchors in the documentation resolve', () => {
  const broken = [];
  let checked = 0;
  for (const rel of markdownFiles()) {
    const file = path.join(root, rel);
    for (const {target} of links(file)) {
      if (/^[a-z][a-z0-9+.-]*:/i.test(target) || target.startsWith('//')) continue;   // http:, mailto:, ...
      checked++;
      const [p, frag] = target.split('#');
      const dest = p ? path.resolve(path.dirname(file), decodeURIComponent(p)) : file;
      if (!dest.startsWith(root) || !fs.existsSync(dest)) { broken.push(`${rel}: ${target} (no such file)`); continue; }
      if (frag !== undefined && dest.endsWith('.md') && !anchors(dest).has(decodeURIComponent(frag)))
        broken.push(`${rel}: ${target} (no heading #${frag} in ${path.relative(root, dest)})`);
    }
  }
  assert.ok(checked > 100, `only ${checked} links found - parser broken?`);
  assert.deepStrictEqual(broken, []);
});
