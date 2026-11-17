// VE.Direct text-protocol parser (src/vedirect.cpp), compiled for the host
// like payload_roundtrip.test.js: checksum handling, interleaved HEX
// messages, partial blocks and "---" values. Skipped without a C++ compiler.
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');

const root = path.join(__dirname, '..');
const exe = path.join(root, '.vedirect-test');
process.on('exit', () => { try { fs.unlinkSync(exe); } catch (_) {} });
let compiled = false;
for (const cxx of [process.env.CXX, 'c++', 'g++', 'clang++'].filter(Boolean)) {
  try {
    execFileSync(cxx, ['-std=c++11', '-Wall', '-Werror', '-I', path.join(root, 'src'),
      path.join(root, 'src', 'vedirect.cpp'), path.join(__dirname, 'vedirect_parser.cpp'),
      '-o', exe], { stdio: 'pipe' });
    compiled = true;
    break;
  } catch (e) {
    if (e.code !== 'ENOENT') throw new Error(`${cxx} failed:\n${e.stderr}`);
  }
}
if (!compiled) {
  console.log('SKIPPED VE.Direct parser test: no C++ compiler found.');
  process.exit(0);
}

// Builds a block "\r\nLABEL\tVALUE...\r\nChecksum\t<byte>" with a correct
// (or deliberately wrong) checksum byte.
function block(fields, corrupt = false) {
  let s = '';
  for (const [k, v] of fields) s += `\r\n${k}\t${v}`;
  s += '\r\nChecksum\t';
  const bytes = Buffer.from(s, 'latin1');
  let sum = 0;
  for (const b of bytes) sum = (sum + b) & 0xFF;
  const check = (256 - sum) & 0xFF;
  return Buffer.concat([bytes, Buffer.from([corrupt ? (check + 1) & 0xFF : check])]);
}

const run = (buf) => JSON.parse(execFileSync(exe, { input: buf, encoding: 'utf8' }));
const eq = (a, e, m) => assert.strictEqual(a, e, `${m}: got ${a}, expected ${e}`);

const mppt = [['PID', '0xA053'], ['FW', '159'], ['SER#', 'HQ2132ABCDE'], ['V', '13280'], ['I', '-1230'],
              ['VPV', '37500'], ['PPV', '45'], ['CS', '3'], ['MPPT', '2'], ['OR', '0x00000000'],
              ['ERR', '0'], ['LOAD', 'ON'], ['IL', '300'], ['H19', '1234'], ['H20', '12'],
              ['H21', '98'], ['H22', '15'], ['H23', '102'], ['HSDS', '17']];

// 1) Reception starting mid-stream: the remains of the interrupted block
//    spoil the checksum of the first block, the next one is valid again.
let d = run(Buffer.concat([Buffer.from('PV\t37500\r\nPPV\t4', 'latin1'), block(mppt), block(mppt)]));
eq(d.blocks, 1, 'only the first block after a mid-stream start is lost');
eq(d.errors, 1, 'first block after a mid-stream start');
eq(d.battV, 13.28, 'battery V');
eq(d.battA, -1.23, 'battery A (negative)');
eq(d.pvV, 37.5, 'PV V');
eq(d.pvW, 45, 'PV W');
eq(d.yield, 0.12, 'yield today');
eq(d.cs, 3, 'charge state');
eq(d.err, 0, 'error');
eq(d.soc, null, 'no SOC on an MPPT');
eq(d.load, 0.3, 'MPPT load output current (IL)');

// 2) Corrupted checksum: values stay unset.
d = run(block(mppt, true));
eq(d.blocks, 0, 'corrupt block rejected');
eq(d.errors, 1, 'corrupt block counted');
eq(d.battV, null, 'no values from a corrupt block');

// 3) HEX messages between and inside blocks are skipped (not in the sum).
const b = block(mppt);
const mid = b.indexOf(Buffer.from('\r\nCS', 'latin1'));
d = run(Buffer.concat([Buffer.from(':A0102000543\n', 'latin1'), b.subarray(0, mid),
                       Buffer.from(':7F0ED0071\n', 'latin1'), b.subarray(mid)]));
eq(d.blocks, 1, 'block with interleaved HEX messages');
eq(d.cs, 3, 'value after a HEX message');

// 4) BMV: two blocks per second, each updates only its own labels; "---"
//    (not available) leaves the value unset.
d = run(Buffer.concat([
  block([['PID', '0x203'], ['V', '12950'], ['I', '-2500'], ['P', '-32'], ['CE', '-1500'], ['SOC', '876'],
         ['TTG', '---'], ['Alarm', 'OFF'], ['AR', '0'], ['BMV', '700'], ['FW', '0308']]),
  block([['H1', '-5000'], ['H2', '-1500'], ['H17', '1200'], ['H18', '900']])]));
eq(d.blocks, 2, 'BMV blocks');
eq(d.battV, 12.95, 'BMV battery V');
eq(d.soc, 87.6, 'BMV SOC');
eq(d.pvW, null, 'BMV has no PV');

// 5) A label that becomes "---" later (BMV SOC while not synchronized) is
//    unavailable again, not stuck at its last number.
d = run(Buffer.concat([block([['V', '12950'], ['SOC', '876'], ['CS', '3']]),
                       block([['V', '12940'], ['SOC', '---'], ['CS', '---']])]));
eq(d.blocks, 2, 'blocks with "---"');
eq(d.battV, 12.94, 'battery V still updated');
eq(d.soc, null, 'SOC "---" clears the old value');
eq(d.cs, -1, 'CS "---" clears the old value');

console.log('VE.Direct parser tests passed.');
