import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createAssert, finish, requireExe, run } from './commons.mjs';

function wavSilence(rate, frames) {
  const data = frames * 2;
  const buf = Buffer.alloc(44 + data);
  buf.write('RIFF', 0);
  buf.writeUInt32LE(36 + data, 4);
  buf.write('WAVE', 8);
  buf.write('fmt ', 12);
  buf.writeUInt32LE(16, 16);
  buf.writeUInt16LE(1, 20);
  buf.writeUInt16LE(1, 22);
  buf.writeUInt32LE(rate, 24);
  buf.writeUInt32LE(rate * 2, 28);
  buf.writeUInt16LE(2, 32);
  buf.writeUInt16LE(16, 34);
  buf.write('data', 36);
  buf.writeUInt32LE(data, 40);
  return buf;
}

const checks = requireExe(createAssert());
const dir = mkdtempSync(join(tmpdir(), 'pm-audio-filter-'));
const src = join(dir, 'in.wav');
const dst = join(dir, 'out.wav');
writeFileSync(src, wavSilence(16000, 320));

const decoded = run(['decode', '--src', src, '--dst', dst, '--rate', '16000']);
checks.assert(decoded.status === 0 && decoded.json?.ok === true, 'decode writes a wav');
checks.assert(decoded.json?.frames === 320, 'decode keeps the frame count');
checks.assert(readFileSync(dst).subarray(0, 4).toString() === 'RIFF', 'destination is a RIFF file');

const missing = run(['decode', '--src', join(dir, 'missing.wav'), '--dst', dst]);
checks.assert(missing.status === 1 && missing.json?.ok === false, 'missing source fails');

const info = run(['runtimes']);
checks.assert(info.status === 0 && info.json?.ok === true, 'runtimes reports json');
checks.assert(typeof info.json?.tanit_root === 'string' && info.json.tanit_root.length > 0, 'tanit root is set');
checks.assert(info.json?.onnx === true && String(info.json.onnx_root).includes('onnxruntime'), 'onnx points at Tanit third_party');

rmSync(dir, { recursive: true, force: true });
finish(checks);
