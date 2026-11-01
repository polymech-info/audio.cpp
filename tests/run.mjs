#!/usr/bin/env node
import { spawnSync } from 'node:child_process';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const dir = dirname(fileURLToPath(import.meta.url));
const files = ['test-selftest.mjs', 'test-match.mjs', 'test-subs.mjs', 'test-filter.mjs', 'test-stt.mjs', 'test-tts.mjs', 'test-voice.mjs', 'test-devices.mjs', 'test-record.mjs', 'test-trigger.mjs'];

let failed = 0;
for (const file of files) {
  console.log(`\n== ${file}`);
  const r = spawnSync(process.execPath, [join(dir, file)], { stdio: 'inherit' });
  if ((r.status ?? 1) !== 0) failed++;
}
process.exit(failed ? 1 : 0);
