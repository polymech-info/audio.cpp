import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createAssert, finish, requireExe, run } from './commons.mjs';

const checks = requireExe(createAssert());

const de = run(['lang', '--text', 'deutsch']);
checks.assert(de.status === 0 && de.json?.language === 'de', 'deutsch maps to de');

const region = run(['lang', '--text', 'en-US']);
checks.assert(region.status === 0 && region.json?.language === 'en', 'en-US maps to en');

const unknown = run(['lang', '--text', 'not-a-language']);
checks.assert(unknown.status === 0 && unknown.json?.language === 'auto', 'unknown language becomes auto');

const dir = mkdtempSync(join(tmpdir(), 'pm-audio-stt-'));
const wav = join(dir, 'silence.wav');
try {
  const written = run(['wav', '--dst', wav, '--frames', '8', '--rate', '16000']);
  checks.assert(written.status === 0 && written.json?.frames === 8, 'wav write reports frames');
  checks.assert(readFileSync(wav).subarray(0, 4).toString() === 'RIFF', 'written file is RIFF');

  const read = run(['wav', '--src', wav]);
  checks.assert(read.status === 0 && read.json?.frames === 8 && read.json?.rate === 16000, 'wav read is 16 kHz');

  const missing = run(['wav', '--src', join(dir, 'missing.wav')]);
  checks.assert(missing.status === 1 && missing.json?.ok === false, 'missing wav fails');
} finally {
  rmSync(dir, { recursive: true, force: true });
}

const miss = run(['probe', '--model', 'definitely-missing-model-zz']);
checks.assert(miss.status === 1 && miss.json?.ok === false, 'missing model probe fails');
checks.assert(miss.json?.filename === 'ggml-definitely-missing-model-zz.bin', 'probe uses the ggml filename');

finish(checks);
