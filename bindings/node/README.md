# SDRNative for Node.js

A library for direct control of SDR hardware, raw IQ sampling,
DVB-T transport stream reception, w/ low-level device access.
Provides tuning, synchronous and asynchronous capture.
TypeScript declarations are included.

Requires Node.js 18+ and an RTL2832U receiver with an R820T-family tuner.

## Installation

```sh
npm install sdrnative
```

A supported RTL2832U receiver and the appropriate USB driver or access
permissions are needed for hardware use. The package includes native binaries for Windows x64, Linux x64, macOS
Intel (14+), and macOS Apple Silicon (15+). No compiler is needed for
these installations.

## Basic Usage

Read raw IQ data:

```js
const fs = require('node:fs');
const { Device, listDevices } = require('sdrnative');

console.log(listDevices());

const dev = Device.open(0);
try {
  dev.iqStart(100_700_000, 2_400_000);
  fs.writeFileSync('capture.iq', dev.read(256 * 1024));
} finally {
  dev.close();
}
```

Receive DVB-T data using the asynchronous stream. Choose frequency with a
local multiplex and supported bandwidth (6, 7 or 8 MHz):

```js
const fs = require('node:fs');
const { Device } = require('sdrnative');

async function capture() {
  const dev = Device.open(0);
  try {
    dev.dvbtStart(557_500_000, 7_000_000);
    console.log('lock stage:', await dev.waitForLock(5000));

    const stream = dev.stream({ queueDepth: 16 });
    try {
      const blocks = await stream.readMany(8);
      fs.writeFileSync('capture.ts', Buffer.concat(blocks.map(b => Buffer.from(b.data))));
    } finally {
      stream.close();
    }
  } finally {
    dev.close();
  }
}

capture().catch(console.error);
```

`Device.read()` is synchronous; `Device.stream()` supplies an asynchronous
iterator and `readMany()`. Close streams before closing the device. Stream
sequence numbers and 64-bit counters use `bigint`. Native errors are reported
as `SDRNativeError` with status and error-domain fields. The addon has a single
active mode per device handle; call `stop()` before switching modes.

The [source repository](https://github.com/rubendavidroy/SDRNative) includes
additional programs under `examples/node/`.

MPL-2.0.