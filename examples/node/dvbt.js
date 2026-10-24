/*
 * SDRNative
 * DVB-T capture example
 *
 * Copyright (c) 2026 Ruben Roy
 * SPDX-License-Identifier: MPL-2.0
 */
'use strict';

const fs = require('node:fs');
const { Device } = require('sdrnative');

const frequencyMHz = Number(process.argv[2] ?? 557.5);
const bandwidthMHz = Number(process.argv[3] ?? 7);
const output = process.argv[4] ?? 'capture.ts';
if (!(frequencyMHz > 0) || ![6, 7, 8].includes(bandwidthMHz)) {
  throw new Error('usage: node dvbt.js <frequency_mhz> <6|7|8> [output.ts]');
}

async function main() {
  const device = Device.open(0);
  try {
    device.dvbtStart(Math.round(frequencyMHz * 1e6), bandwidthMHz * 1_000_000);
    await device.waitForLock(5000);
    const stream = device.stream({ queueDepth: 16 });
    try {
      const blocks = await stream.readMany(8);
      fs.writeFileSync(output, Buffer.concat(blocks.map(block => Buffer.from(block.data))));
    } finally {
      stream.close();
    }
    console.log(`saved ${output}`);
  } finally {
    device.close();
  }
}

main().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
