import { spawnSync } from 'node:child_process';

const result = spawnSync(
  process.platform === 'win32' ? 'pnpm.cmd' : 'pnpm',
  ['exec', 'vite', 'build', '--config', 'vite.config.ts', '--base', './'],
  {
    cwd: new URL('..', import.meta.url),
    env: {
      ...process.env,
      PORT: process.env.PORT || '21230',
      BASE_PATH: './',
    },
    stdio: 'inherit',
  },
);

if (result.error) {
  throw result.error;
}

process.exit(result.status ?? 1);