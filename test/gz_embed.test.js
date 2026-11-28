// Embedded web assets stored gzip-compressed (tools/gz_embed.py, src/gz_send.cpp):
// the bytes decompress to exactly the source, are deterministic, and the
// handlers send them with the right headers.
const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');
const zlib = require('node:zlib');
const {execFileSync} = require('node:child_process');
const root = path.join(__dirname, '..');
const py = process.env.PYTHON || 'python3';

function run(code) {
  return execFileSync(py, ['-B', '-c', code], {cwd: root, maxBuffer: 64 * 1024 * 1024, encoding: 'latin1'});
}
function headerBytes(header, name) {
  const m = header.match(new RegExp(name + '_GZ\\[\\] PROGMEM = \\{([\\s\\S]*?)\\};'));
  return Buffer.from([...m[1].matchAll(/0x([0-9a-f]{2})/g)].map((x) => parseInt(x[1], 16)));
}

test('dashboard: gzip bytes decompress to the page source, deterministic', () => {
  const header = run("import sys; sys.path.insert(0,'tools'); import gz_embed as g; from pathlib import Path; " +
    "print(g.c_header('CHART_PAGE', g.raw_string_body(Path('include/chart_page.h').read_text(encoding='utf-8'), 'HTML').encode('utf-8'), 'X'), end='')");
  const gz = headerBytes(header, 'CHART_PAGE');
  const src = fs.readFileSync(path.join(root, 'include/chart_page.h'), 'utf8');
  const body = src.slice(src.indexOf('R"HTML(') + 7, src.lastIndexOf(')HTML"'));
  const out = zlib.gunzipSync(gz);
  assert.ok(out.equals(Buffer.from(body, 'utf8')), 'not byte-identical');
  assert.ok(header.includes(`CHART_PAGE_LEN = ${Buffer.byteLength(body)};`) && header.includes(`CHART_PAGE_GZ_LEN = ${gz.length};`));
  assert.deepStrictEqual([...gz.subarray(0, 10)], [0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 2, 255]);   // no name, mtime 0, -9
  assert.ok(gz.length < body.length / 3, `compressed ${gz.length} of ${body.length}`);
  assert.ok(!body.includes('@@'), 'the page must not need server-side markers (it is a fixed file)');
});

test('help Markdown: gzip bytes decompress to the combined documentation', () => {
  const out = run("import sys; sys.path.insert(0,'tools'); import gz_embed as g, help_md as h; " +
    "md = h.build('.').rstrip('\\n').encode('utf-8'); hd = g.c_header('README_MD', md, 'X'); " +
    "import base64; print(base64.b64encode(md).decode()); print('@@@'); print(hd, end='')");
  const [b64, header] = out.split('\n@@@\n');
  const md = Buffer.from(b64.trim(), 'base64');
  const gz = headerBytes(header, 'README_MD');
  assert.ok(zlib.gunzipSync(gz).equals(md));
  assert.ok(header.includes(`README_MD_LEN = ${md.length};`));
});

test('handlers send the gzip assets with Content-Encoding, Vary and the Accept-Encoding header readable', () => {
  const send = fs.readFileSync(path.join(root, 'src/gz_send.cpp'), 'utf8');
  assert.match(send, /sendHeader\("Content-Encoding", "gzip"\)/);
  assert.match(send, /sendHeader\("Vary", "Accept-Encoding"\)/);
  assert.match(send, /send_P\(200, type, \(const char\*\)gz, gzLen\)/);          // Content-Length = compressed size
  assert.match(send, /tinfl_decompress\(/);                                      // fallback: ROM inflater
  assert.match(send, /send\(406,/);
  const web = fs.readFileSync(path.join(root, 'src/web_api.cpp'), 'utf8');
  assert.match(web, /sendGzipAsset\(server, CHART_PAGE_GZ, CHART_PAGE_GZ_LEN, CHART_PAGE_LEN, "text\/html; charset=utf-8", "no-store"\)/);
  assert.match(web, /sendGzipAsset\(server, README_MD_GZ, README_MD_GZ_LEN, README_MD_LEN, "text\/plain; charset=utf-8"\)/);
  assert.match(web, /^HttpServer server\(80\);/m);                              // esp_http_server: every header readable
  assert.ok(!/#include "chart_page\.h"/.test(web), 'the uncompressed page must not be linked');
  const portal = fs.readFileSync(path.join(root, 'src/portal.cpp'), 'utf8');
  assert.match(portal, /sendGzipAsset\(\*wm\.server, README_MD_GZ/);
  assert.match(portal, /collectHeaders\(hdrs, 1\)/);
});
