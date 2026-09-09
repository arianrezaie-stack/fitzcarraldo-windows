import { fileURLToPath } from 'node:url';

import { build } from 'vite';

process.env.PORT ||= '21230';
process.env.BASE_PATH = './';

await build({
  configFile: fileURLToPath(new URL('../vite.config.ts', import.meta.url)),
  base: './',
});
