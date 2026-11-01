import { createAssert, finish, requireExe, run } from './commons.mjs';

const checks = requireExe(createAssert());
const listed = run(['devices']);
checks.assert(listed.status === 0 && listed.json?.ok === true, 'device list returns json');
checks.assert(typeof listed.json?.capture === 'number', 'capture count is a number');
checks.assert(typeof listed.json?.playback === 'number', 'playback count is a number');
finish(checks);
