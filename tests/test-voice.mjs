import { createAssert, finish, requireExe, run } from './commons.mjs';

const checks = requireExe(createAssert());
const voice = run(['voice']);
checks.assert(voice.status === 0 && voice.json?.ok === true, 'voice pipeline routes a wake phrase');
checks.assert(voice.json?.replay_command === 'what time', 'replay command drops the wake prefix');
checks.assert(voice.json?.replay === true, 'replay fires the trigger');
finish(checks);
