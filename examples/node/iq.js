/*
 * SDRNative
 * IQ capture example
 *
 * Copyright (c) 2026 Ruben Roy
 * SPDX-License-Identifier: MPL-2.0
 */
'use strict';

const fs = require('node:fs');
const { Device } = require('sdrnative');

const frequencyMHz = Number(process.argv[2] ?? 100.7);
const output = process.argv[3] ?? 'capture.iq';
if (!(frequencyMHz > 0)) throw new Error('frequency must be positive');

const device = Device.open(0);
try {
  device.iqStart(Math.round(frequencyMHz * 1e6), 2_400_000);
  fs.writeFileSync(output, device.read(256 * 1024));
  console.log(`saved ${output}`);
} finally {
  device.close();
}
