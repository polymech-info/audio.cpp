import { existsSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createAssert, finish, requireExe, run } from './commons.mjs';

const checks = requireExe(createAssert());
const dir = mkdtempSync(join(tmpdir(), 'pm-audio-subs-'));
const out = join(dir, 'clip.srt');
try {
  const written = run(['subs', '--format', 'srt', '--out', out, '--text', 'hello there', '--t0', '0', '--t1', '1200']);
  checks.assert(written.status === 0 && written.json && written.json.ok === true, 'srt write succeeds');
  checks.assert(written.json && written.json.cues === 1, 'one cue written');
  checks.assert(existsSync(out), 'srt file exists');
  const bad = run(['subs', '--format', 'all', '--out', out, '--text', 'x']);
  checks.assert(bad.status === 1 && bad.json && bad.json.code === 'EINVAL', 'all is not one format');
} finally {
  rmSync(dir, { recursive: true, force: true });
}
finish(checks);
