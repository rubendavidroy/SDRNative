#!/usr/bin/env node
'use strict';
const fs = require('node:fs');
const path = require('node:path');
const root = path.resolve(__dirname, '..');
const runtimes = {
  'darwin-arm64': 'libusb-1.0.0.dylib',
  'darwin-x64': 'libusb-1.0.0.dylib',
  'win32-x64': 'libusb-1.0.dll',
  'linux-x64': 'libusb-1.0.so.0',
};
for (const [target, lib] of Object.entries(runtimes)) {
  for (const file of ['sdrnative_node.node', lib]) {
    const pathname = path.join(root, 'prebuilds', target, file);
    if (!fs.statSync(pathname, { throwIfNoEntry: false })?.size) {
      throw new Error(`missing or empty: ${pathname}`);
    }
  }
}
console.log('all four prebuilds present');