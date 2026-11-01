import { createAssert, finish, requireExe, run } from './commons.mjs';

const checks = requireExe(createAssert());
const wake = run(['match', '--text', 'hey tanit what time', '--wake', 'tanit']);
checks.assert(wake.status === 0 && wake.json && wake.json.ok === true, 'match returns json');
checks.assert(wake.json && wake.json.matched === true, 'wake prefix matches');
checks.assert(wake.json && wake.json.command === 'what time', 'command text drops the wake prefix');
const cancel = run(['match', '--text', 'um stop', '--wake-off']);
checks.assert(cancel.status === 0 && cancel.json && cancel.json.control === 'cancel', 'stop is cancel');
finish(checks);
