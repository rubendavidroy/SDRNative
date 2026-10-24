#!/usr/bin/env node
'use strict';
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const root = path.resolve(__dirname, '..');
const temp = fs.mkdtempSync(path.join(os.tmpdir(), 'sdrnative-npm-'));
const npm = process.platform === 'win32' ? 'npm.cmd' : 'npm';
function run(cmd, args, cwd = temp) {
  const result = spawnSync(cmd, args, {
    cwd,
    stdio: 'inherit',
    shell: process.platform === 'win32' && /\.cmd$/i.test(cmd),
  });
  if (result.error || result.status !== 0) throw result.error || new Error(`${cmd} failed`);
}
try {
  let archive = process.argv[2] ? path.resolve(process.argv[2]) : null;
  if (!archive) {
    run(npm, ['pack', '--ignore-scripts', '--pack-destination', temp], root);
    archive = path.join(temp, 'sdrnative-0.1.0.tgz');
  }
  run(npm, ['install', '--ignore-scripts', '--no-audit', '--no-fund', '--offline', archive]);
  run(process.execPath, ['-e', `
    const s = require('sdrnative');
    if (s.version() !== '0.1.0' || s.abiVersion() !== 1) process.exit(1);
    try {
      if (!Array.isArray(s.listDevices())) process.exit(1);
    } catch (error) {
      if (process.env.SDRNATIVE_ALLOW_NO_USB !== '1' || !/libusb.*not built/i.test(error.message)) throw error;
    }
    console.log('npm clean install OK');
  `]);
} finally {
  fs.rmSync(temp, { recursive: true, force: true, maxRetries: 5, retryDelay: 150 });
}
