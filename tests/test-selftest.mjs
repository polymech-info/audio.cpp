import { createAssert, finish, requireExe, run } from './commons.mjs';

const checks = requireExe(createAssert());
const all = run(['selftest']);
checks.assert(all.status === 0 && all.json && all.json.ok === true, 'session selftest passes');
const unknown = run(['selftest', '--suite', 'nope']);
checks.assert(unknown.status === 1, 'unknown suite fails');
const usage = run(['selftest', '--nope']);
checks.assert(usage.status === 2, 'usage error exits 2');
finish(checks);
