// Builds the native audio addon for the host OS and, on Windows, rebuilds
// better-sqlite3 against Electron's ABI. macOS keeps its codesign step in
// scripts/build-native-audio.sh.
import { spawnSync } from 'node:child_process';
import { createRequire } from 'node:module';
import { copyFileSync, existsSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const run = (command, args, env = process.env) => {
  const result = spawnSync(command, args, { cwd: root, stdio: 'inherit', env, shell: false });
  if (result.error) {
    console.error(result.error.message);
    process.exit(1);
  }
  if (result.status !== 0) process.exit(result.status ?? 1);
};

if (process.platform === 'darwin') {
  run('bash', ['scripts/build-native-audio.sh']);
  process.exit(0);
}

if (process.platform !== 'win32') {
  console.log('Native audio bridge: skipping this platform');
  process.exit(0);
}

const require = createRequire(import.meta.url);
const electronVersion = require('electron/package.json').version;
const arch = process.arch;
const env = {
  ...process.env,
  npm_config_runtime: 'electron',
  npm_config_target: electronVersion,
  npm_config_arch: arch,
  npm_config_target_arch: arch,
  npm_config_disturl: 'https://electronjs.org/headers',
  npm_config_build_from_source: 'true'
};

const gyp = join(root, 'node_modules', 'node-gyp', 'bin', 'node-gyp.js');
run(process.execPath, [gyp, 'rebuild'], env);

const built = join(root, 'build', 'Release', 'babelvoice_native.node');
if (!existsSync(built)) {
  console.error(`Native bridge was not produced at ${built}`);
  process.exit(1);
}
const outDir = join(root, 'resources', 'native', `win32-${arch}`);
mkdirSync(outDir, { recursive: true });
copyFileSync(built, join(outDir, 'babelvoice_native.node'));
console.log(`Native bridge: copied to resources/native/win32-${arch}/babelvoice_native.node`);

const rebuild = join(root, 'node_modules', '@electron', 'rebuild', 'lib', 'cli.js');
if (existsSync(rebuild)) {
  run(process.execPath, [rebuild, '-f', '-w', 'better-sqlite3']);
} else {
  console.warn('better-sqlite3 was not rebuilt for Electron (@electron/rebuild missing)');
}
