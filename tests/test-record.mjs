import { mkdtempSync, rmSync, statSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import { createAssert, finish, requireExe, run } from './commons.mjs';

const checks = requireExe(createAssert());
const dir = mkdtempSync(join(tmpdir(), 'pm-audio-record-'));
try {
  const wav = join(dir, 'clip.wav');
  const wrote = run(['record', '--dst', wav]);
  checks.assert(wrote.status === 0 && wrote.json?.ok === true, 'wav record returns json');
  checks.assert(wrote.json?.frames === 480, 'wav holds 480 frames');
  checks.assert(wrote.json?.bytes === 1920, 'wav holds 1920 pcm bytes');
  checks.assert(wrote.json?.container === 'wav', 'wav container');
  checks.assert(statSync(wav).size === 44 + 1920, 'wav file is header plus pcm');

  const session = run(['record', '--session', dir]);
  checks.assert(session.status === 0 && session.json?.ok === true, 'session record returns json');
  checks.assert(session.json?.active === true, 'session stays active until release');
  checks.assert(session.json?.stop === true, 'stop flag is set');
  checks.assert(session.json?.samples === 480, 'session sample count');
  checks.assert(session.json?.device === 'test', 'session device name');
  checks.assert(typeof session.json?.aac === 'boolean', 'aac support is a boolean');
} finally {
  rmSync(dir, { recursive: true, force: true });
}
finish(checks);
