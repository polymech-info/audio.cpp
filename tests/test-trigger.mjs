import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import { createAssert, finish, requireExe, run } from './commons.mjs';

const checks = requireExe(createAssert());
const dir = mkdtempSync(join(tmpdir(), 'pm-audio-trigger-'));
try {
  const wav = join(dir, 'silence.wav');
  const wrote = run(['wav', '--dst', wav, '--frames', '16000', '--rate', '16000']);
  checks.assert(wrote.status === 0 && wrote.json?.ok === true, 'silence wav writes');

  const inspected = run(['sound-trigger', 'inspect', wav, '--json']);
  checks.assert(inspected.status === 0 && inspected.json?.schema === 'pm.sound.inspect/v1', 'inspect summary schema');
  checks.assert(inspected.json?.frames > 0, 'inspect emits hops');
  checks.assert(inspected.json?.whistle_frames === 0, 'silence is not a whistle');

  const plot = join(dir, 'take.svg');
  const plotted = run(['sound-trigger', 'inspect', wav, '--plot', plot]);
  checks.assert(plotted.status === 0, 'inspect plot succeeds');
  checks.assert(readFileSync(plot, 'utf8').includes('<svg'), 'plot is svg');

  const matched = run(['sound-trigger', 'match', wav, '--jsonl']);
  checks.assert(matched.status === 2 && matched.json?.schema === 'pm.sound.no-match/v1', 'silence does not match');

  const samples = Array.from({ length: 8 }, () => wav);
  const trained = run([
    'sound-trigger', 'train', '--id', 'clap', '--json',
    '--samples', ...samples,
    '--ambient', wav,
    '--out', join(dir, 'clap.json'),
  ]);
  checks.assert(trained.status === 2, 'silence takes fail training');
  checks.assert((trained.stderr || '').includes('fewer than 8 positive takes'), 'training names the quality check');
} finally {
  rmSync(dir, { recursive: true, force: true });
}
finish(checks);
