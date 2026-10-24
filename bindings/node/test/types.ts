/*
 * SDRNative
 * Node.js type checks
 *
 * Copyright (c) 2026 Ruben Roy
 * SPDX-License-Identifier: MPL-2.0
 */
import {
  Device,
  AsyncOverflowPolicy,
  AsyncBlock,
  AsyncStats,
  version,
  abiVersion,
  listDevices,
} from '..';

const v: string = version();
const abi: number = abiVersion();
const devices = listDevices();
const firstProduct: string | undefined = devices[0]?.product;
void v; void abi; void firstProduct;

function useBlock(block: AsyncBlock, stats: AsyncStats): void {
  const seq: bigint = block.sequence;
  const data: Uint8Array = block.data;
  const drops: bigint = stats.droppedBuffers;
  void seq; void data; void drops;
}
void useBlock;

async function example(): Promise<void> {
  const dev = Device.open(0);
  try {
    dev.iqStart(100_700_000, 2_400_000);
    const stream = dev.stream({ overflowPolicy: AsyncOverflowPolicy.DROP_OLDEST });
    try {
      for await (const block of stream) {
        useBlock(block, stream.stats);
        break;
      }
    } finally {
      stream.close();
    }
  } finally {
    dev.close();
  }
}
void example;