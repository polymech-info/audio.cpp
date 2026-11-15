import { createAssert, finish, requireExe, run } from './commons.mjs';

const checks = requireExe(createAssert());
const r = run(['selftest', '--suite', 'vad-gate']);
checks.assert(r.status === 0 && r.json && r.json.ok === true, 'vad-gate selftest passes');
finish(checks);