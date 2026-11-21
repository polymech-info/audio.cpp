import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createAssert, finish, requireExe, run } from './commons.mjs';

function wav(samples, rate = 16000) {
  const data = Buffer.alloc(samples.length * 2);
  for (let i = 0; i < samples.length; i++) data.writeInt16LE(samples[i], i * 2);
  const header = Buffer.alloc(44);
  header.write('RIFF', 0);
  header.writeUInt32LE(36 + data.length, 4);
  header.write('WAVE', 8);
  header.write('fmt ', 12);
  header.writeUInt32LE(16, 16);
  header.writeUInt16LE(1, 20);
  header.writeUInt16LE(1, 22);
  header.writeUInt32LE(rate, 24);
  header.writeUInt32LE(rate * 2, 28);
  header.writeUInt16LE(2, 32);
  header.writeUInt16LE(16, 34);
  header.write('data', 36);
  header.writeUInt32LE(data.length, 40);
  return Buffer.concat([header, data]);
}

const checks = requireExe(createAssert());
const listed = run(['aec3', 'devices']);
checks.assert(listed.status === 0 && listed.json?.ok === true, 'aec3 devices returns json');
checks.assert(listed.json?.schema === 'pm.audio.aec3.devices/v1', 'devices schema');
checks.assert(typeof listed.json?.backend?.active === 'boolean', 'backend status is present');
checks.assert(Array.isArray(listed.json?.capture) && Array.isArray(listed.json?.playback), 'device arrays');

const dir = mkdtempSync(join(tmpdir(), 'aec3-'));
const capture = join(dir, 'mic.wav');
const render = join(dir, 'far.wav');
const session = join(dir, 'session');
const samples = new Array(330).fill(0).map((_, i) => (i % 20) * 100);
writeFileSync(capture, wav(samples));
writeFileSync(render, wav(samples.map((sample) => -sample)));

const replay = run(['aec3', 'replay', '--capture', capture, '--render', render, '--out', session, '--no-aec']);
checks.assert(replay.status === 0 && replay.json?.ok === true, 'offline replay without aec');
checks.assert(replay.json?.frames === 2, 'complete 10 ms frames only');
checks.assert(replay.json?.capture_remainder_samples === 10, 'tail samples stay out of the processed wav');
checks.assert(replay.json?.aec_active === false, 'no-aec reports the backend inactive');

const metrics = JSON.parse(readFileSync(join(session, 'metrics.json'), 'utf8'));
checks.assert(metrics.schema === 'pm.audio.aec3.metrics/v1', 'metrics schema');
checks.assert(metrics.frames === 2, 'metrics frame count');
const manifest = JSON.parse(readFileSync(join(session, 'manifest.json'), 'utf8'));
checks.assert(manifest.complete === true && manifest.schema === 'pm.audio.aec3.manifest/v1', 'complete manifest');

const inspected = run(['aec3', 'inspect', '--session', session]);
checks.assert(inspected.status === 0 && inspected.json?.manifest?.complete === true, 'inspect reads the manifest');

const usage = run(['aec3', 'replay', '--scenario', 'nope', '--capture', capture, '--no-aec']);
checks.assert(usage.status === 2 && usage.json?.code === 'EINVAL', 'unknown scenario is a usage error');

const desktop = join(dir, 'desktop.wav');
const desktopSamples = samples.map(() => 1000);
writeFileSync(desktop, wav(desktopSamples));
const mixed = run(['aec3', 'replay', '--source', 'mix', '--capture', capture, '--desktop-capture', desktop, '--render', render, '--out', join(dir, 'mix'), '--no-aec']);
checks.assert(mixed.status === 0 && mixed.json?.source === 'mix', 'mix replay accepts both legs');
checks.assert(mixed.json?.frames === 2, 'mix keeps complete frames from the microphone clock');
const mixedWav = readFileSync(join(dir, 'mix', 'capture-processed.wav'));
checks.assert(mixedWav.readInt16LE(46) === samples[1] + 1000, 'mix sums microphone and desktop after framing');

const badSource = run(['aec3', 'replay', '--source', 'camera', '--capture', capture, '--no-aec']);
checks.assert(badSource.status === 2 && badSource.json?.code === 'EINVAL', 'unknown source is a usage error');

if (!listed.json.backend.active) {
  const missing = run(['aec3', 'replay', '--capture', capture, '--render', render, '--out', join(dir, 'needs-aec')]);
  checks.assert(missing.status === 1 && missing.json?.code === 'ENOTSUP', 'aec replay reports a missing backend');
}

rmSync(dir, { recursive: true, force: true });
finish(checks);
