import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createAssert, finish, requireExe, run } from './commons.mjs';

const checks = requireExe(createAssert());

const inline = run(['speak', '--text', '# Hello\n\nSay **this**.\n\n```\nskip me\n```\n']);
checks.assert(inline.status === 0 && inline.json?.ok === true, 'speak prepares markdown');
checks.assert(inline.json?.preparser === 'markdown', 'markdown preparser ran');
checks.assert(String(inline.json?.text).includes('Hello'), 'heading is spoken');
checks.assert(String(inline.json?.text).includes('this'), 'bold text is spoken');
checks.assert(!String(inline.json?.text).includes('skip'), 'code block is not spoken');

const plain = run(['speak', '--text', 'just words']);
checks.assert(plain.status === 0 && plain.json?.text === 'just words', 'plain text stays plain');
checks.assert(plain.json?.preparser === '', 'plain text skips the preparser');

const dir = mkdtempSync(join(tmpdir(), 'pm-audio-tts-'));
const file = join(dir, 'note.md');
try {
  writeFileSync(file, '---\ntitle: hidden\n---\n# Title\n\nBody.\n');
  const fromFile = run(['speak', '--src', file]);
  checks.assert(fromFile.status === 0 && fromFile.json?.from_file === true, 'markdown file loads');
  checks.assert(String(fromFile.json?.text).includes('Title'), 'file heading is spoken');
  checks.assert(!String(fromFile.json?.text).includes('hidden'), 'front matter is dropped');
} finally {
  rmSync(dir, { recursive: true, force: true });
}

const empty = run(['speak', '--text', '   ']);
checks.assert(empty.status === 1 && empty.json?.ok === false, 'empty text fails');

const moss = run(['moss']);
checks.assert(moss.status === 0 && moss.json?.loaded === false, 'moss stays unloaded without a model');

finish(checks);
