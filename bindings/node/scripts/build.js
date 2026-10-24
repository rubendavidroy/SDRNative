#!/usr/bin/env node
/*
 * SDRNative
 * Node.js build launcher
 *
 * Copyright (c) 2026 Ruben Roy
 * SPDX-License-Identifier: MPL-2.0
 */
'use strict';

const { spawnSync } = require('node:child_process');
const path = require('node:path');

const root = path.resolve(__dirname, '..');
const buildDir = path.join(root, 'build', 'cmake');

function run(args) {
  const result = spawnSync('cmake', args, { stdio: 'inherit' });
  if (result.error) {
    console.error(result.error.message);
    process.exit(1);
  }
  if (result.status !== 0) process.exit(result.status ?? 1);
}

const configureArgs = ['-S', root, '-B', buildDir, '-DCMAKE_BUILD_TYPE=Release'];
if (process.env.SDRNATIVE_NODE_REQUIRE_LIBUSB) {
  configureArgs.push(`-DSDRNATIVE_NODE_REQUIRE_LIBUSB=${process.env.SDRNATIVE_NODE_REQUIRE_LIBUSB}`);
}
for (const key of ['NODE_API_INCLUDE_DIR', 'CMAKE_TOOLCHAIN_FILE', 'VCPKG_TARGET_TRIPLET', 'CMAKE_OSX_DEPLOYMENT_TARGET']) {
  if (process.env[key]) configureArgs.push(`-D${key}=${process.env[key]}`);
}
if (process.env.NODE_LIBRARY) {
  configureArgs.push(`-DNODE_LIBRARY=${process.env.NODE_LIBRARY}`);
}
run(configureArgs);
run(['--build', buildDir, '--config', 'Release', '--parallel']);