/*
 * SDRNative
 * Node.js binding tests
 *
 * Copyright (c) 2026 Ruben Roy
 * SPDX-License-Identifier: MPL-2.0
 */
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const s = require('..');

test('runtime and ABI', () => {
  assert.equal(s.version(), '0.1.0');
  assert.equal(s.abiVersion(), 1);
});

test('async defaults and limits', () => {
  assert.deepEqual(s.ASYNC_DEFAULTS, {
    transferSizeBytes: 65536,
    transferCount: 8,
    queueDepth: 16,
    transferTimeoutMs: 0,
    overflowPolicy: 0,
  });
  assert.equal(s.ASYNC_MIN_TRANSFER_SIZE, 4096);
  assert.equal(s.ASYNC_MAX_TRANSFER_SIZE, 1024 * 1024);
  assert.equal(s.ASYNC_MAX_QUEUE_BYTES, 64 * 1024 * 1024);
});

test('async config validates geometry before entering C', () => {
  const fake = Object.create(s.Device.prototype);
  fake.closed = false;
  fake._handle = {};
  assert.throws(() => fake.stream({ transferSizeBytes: 1 }), /transferSizeBytes/);
  assert.throws(() => fake.stream({ queueDepth: 129 }), /queueDepth/);
  assert.throws(() => fake.stream({ transferSizeBytes: 1024 * 1024, queueDepth: 128 }), /exceeds 64 MiB/);
});

test('native errors retain SDRNative metadata', () => {
  try {
    s.listDevices();
  } catch (error) {
    assert.ok(error instanceof s.SDRNativeError);
    assert.equal(typeof error.status, 'number');
    assert.equal(typeof error.domain, 'number');
    assert.equal(typeof error.nativeCode, 'number');
    assert.equal(typeof error.deviceGone, 'boolean');
  }
});

test('public async stream is an async iterator', () => {
  assert.equal(typeof s.AsyncStream.prototype[Symbol.asyncIterator], 'function');
  assert.equal(typeof s.AsyncStream.prototype.next, 'function');
  assert.equal(typeof s.AsyncStream.prototype.return, 'function');
});

test('AsyncStream skips consumer timeouts and preserves bigint sequence', async () => {
  const dev = Object.create(s.Device.prototype);
  dev.closed = false;
  let started = 0, stopped = 0, reads = 0;
  dev.asyncStart = () => { started++; };
  dev.asyncRead = async () => {
    reads++;
    if (reads === 1) return null;
    return { data: Buffer.from([1, 2, 3]), sequence: 7n };
  };
  dev.asyncStop = () => { stopped++; };
  Object.defineProperty(dev, 'asyncRunning', { get: () => true });
  Object.defineProperty(dev, 'asyncStats', { get: () => ({ terminal: false }) });
  const stream = new s.AsyncStream(dev);
  assert.equal(started, 1);
  const result = await stream.next();
  assert.equal(result.done, false);
  assert.equal(result.value.sequence, 7n);
  assert.deepEqual([...result.value.data], [1, 2, 3]);
  await stream.return();
  assert.equal(stopped, 1);
});

test('AsyncStream readMany uses one consumer and ignores timeout nulls', async () => {
  const dev = Object.create(s.Device.prototype);
  dev.closed = false;
  const queue = [null, { data: Buffer.from([1]), sequence: 1n }, null, { data: Buffer.from([2]), sequence: 2n }];
  dev.asyncStart = () => {};
  dev.asyncRead = async () => queue.shift() ?? null;
  dev.asyncStop = () => {};
  Object.defineProperty(dev, 'asyncRunning', { get: () => true });
  Object.defineProperty(dev, 'asyncStats', { get: () => ({ terminal: false }) });
  const stream = new s.AsyncStream(dev);
  const blocks = await stream.readMany(2);
  assert.deepEqual(blocks.map((b) => b.sequence), [1n, 2n]);
  stream.close();
});

test('AsyncStream cleans up stopped producers that still own native state', () => {
  const dev = Object.create(s.Device.prototype);
  dev.closed = false;
  let stopped = 0;
  dev.asyncStart = () => {};
  dev.asyncStop = () => { stopped++; };
  Object.defineProperty(dev, 'asyncRunning', { get: () => false });
  const stream = new s.AsyncStream(dev);
  stream.close();
  stream.close();
  assert.equal(stopped, 1);
});