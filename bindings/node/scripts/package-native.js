#!/usr/bin/env node
'use strict';

const fs = require('node:fs');
const path = require('node:path');
const { execFileSync } = require('node:child_process');

const root = path.resolve(__dirname, '..');
const target = `${process.platform}-${process.arch}`;
const supported = new Set(['linux-x64', 'darwin-x64', 'darwin-arm64', 'win32-x64']);
if (!supported.has(target)) throw new Error(`unsupported release target: ${target}`);
const output = path.join(root, 'prebuilds', target);
fs.mkdirSync(output, { recursive: true });
const addon = path.join(output, 'sdrnative_node.node');
fs.copyFileSync(path.join(root, 'build', 'Release', 'sdrnative_node.node'), addon);

function run(command, args) {
  return execFileSync(command, args, { encoding: 'utf8' }).trim();
}

if (process.platform === 'win32') {
  fs.copyFileSync('C:/vcpkg/installed/x64-windows/bin/libusb-1.0.dll',
    path.join(output, 'libusb-1.0.dll'));
} else if (process.platform === 'darwin') {
  const prefix = run('brew', ['--prefix', 'libusb']);
  const lib = 'libusb-1.0.0.dylib';
  const bundled = path.join(output, lib);
  fs.copyFileSync(path.join(prefix, 'lib', lib), bundled);
  const dependencies = run('otool', ['-L', addon]).split('\n').slice(1);
  const dependency = dependencies.map(line => line.trim().split(/\s+/)[0])
    .find(name => name.includes('libusb') && name.endsWith('.dylib'));
  if (!dependency) throw new Error('libusb dependency not found in native addon');
  run('install_name_tool', ['-change', dependency, `@loader_path/${lib}`, addon]);
  run('install_name_tool', ['-id', `@loader_path/${lib}`, bundled]);
  // Re-sign the modified binaries to allow local loading on macOS.
  run('codesign', ['--force', '--sign', '-', bundled]);
  run('codesign', ['--force', '--sign', '-', addon]);
  if (run('otool', ['-L', addon]).includes(dependency)) {
    throw new Error('addon still links to the external libusb path');
  }
} else {
  const lib = 'libusb-1.0.so.0';
  const candidates = [
    `/usr/lib/${process.arch === 'x64' ? 'x86_64' : 'aarch64'}-linux-gnu/${lib}`,
    `/usr/lib64/${lib}`,
  ];
  const src = candidates.find(file => fs.existsSync(file));
  if (!src) throw new Error(`unable to find ${lib}`);
  fs.copyFileSync(src, path.join(output, lib));
  const dynamic = run('readelf', ['-d', addon]);
  if (!dynamic.includes('[$ORIGIN]')) {
    throw new Error('addon RUNPATH does not include $ORIGIN');
  }
}

console.log(`prepared ${target}`);