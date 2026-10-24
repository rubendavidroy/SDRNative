/*
 * SDRNative
 * Node.js binding
 *
 * Copyright (c) 2026 Ruben Roy
 * SPDX-License-Identifier: MPL-2.0
 */
'use strict';

const path = require('node:path');
const native = require(path.join(__dirname, 'build', 'Release', 'sdrnative_node.node'));

const Mode = Object.freeze({ NONE: 0, DVBT: 1, IQ: 2 });
const State = Object.freeze({ INVALID: 0, IDLE: 1, DVBT: 2, IQ: 3 });
const StreamFormat = Object.freeze({ NONE: 0, MPEG_TS: 1, U8_IQ_INTERLEAVED: 2 });
const TunerFamily = Object.freeze({ UNKNOWN: 0, R820T_FAMILY: 1 });
const DemodFamily = Object.freeze({ UNKNOWN: 0, RTL2832: 1 });
const AsyncOverflowPolicy = Object.freeze({ DROP_OLDEST: 0, DROP_NEWEST: 1 });

const ASYNC_MIN_TRANSFER_SIZE = native.ASYNC_MIN_TRANSFER_SIZE;
const ASYNC_MAX_TRANSFER_SIZE = native.ASYNC_MAX_TRANSFER_SIZE;
const ASYNC_MIN_TRANSFER_COUNT = native.ASYNC_MIN_TRANSFER_COUNT;
const ASYNC_MAX_TRANSFER_COUNT = native.ASYNC_MAX_TRANSFER_COUNT;
const ASYNC_MIN_QUEUE_DEPTH = native.ASYNC_MIN_QUEUE_DEPTH;
const ASYNC_MAX_QUEUE_DEPTH = native.ASYNC_MAX_QUEUE_DEPTH;
const ASYNC_MAX_QUEUE_BYTES = native.ASYNC_MAX_QUEUE_BYTES;
const ASYNC_MAX_TRANSFER_POOL_BYTES = native.ASYNC_MAX_TRANSFER_POOL_BYTES;
const defaults = Object.freeze(native.asyncDefaults());

class SDRNativeError extends Error {}

function normalizeError(error) {
  if (error && error.name === 'SDRNativeError' && !(error instanceof SDRNativeError)) {
    Object.setPrototypeOf(error, SDRNativeError.prototype);
  }
  return error;
}

function call(fn, ...args) {
  try {
    return fn(...args);
  } catch (error) {
    throw normalizeError(error);
  }
}

async function callAsync(fn, ...args) {
  try {
    return await fn(...args);
  } catch (error) {
    throw normalizeError(error);
  }
}

function integer(name, value, min = 0, max = Number.MAX_SAFE_INTEGER) {
  if (!Number.isSafeInteger(value) || value < min || value > max) {
    throw new RangeError(`${name} must be an integer in ${min}..${max}`);
  }
  return value;
}

function frequency(name, value) {
  if (typeof value === 'bigint') {
    if (value < 0n) throw new RangeError(`${name} must be non-negative`);
    return value;
  }
  return integer(name, value, 0, Number.MAX_SAFE_INTEGER);
}

function normalizeAsyncConfig(config = {}) {
  if (config == null || typeof config !== 'object' || Array.isArray(config)) {
    throw new TypeError('async config must be an object');
  }
  const out = {
    transferSizeBytes: config.transferSizeBytes ?? defaults.transferSizeBytes,
    transferCount: config.transferCount ?? defaults.transferCount,
    queueDepth: config.queueDepth ?? defaults.queueDepth,
    transferTimeoutMs: config.transferTimeoutMs ?? defaults.transferTimeoutMs,
    overflowPolicy: config.overflowPolicy ?? defaults.overflowPolicy,
  };
  integer('transferSizeBytes', out.transferSizeBytes, ASYNC_MIN_TRANSFER_SIZE, ASYNC_MAX_TRANSFER_SIZE);
  integer('transferCount', out.transferCount, ASYNC_MIN_TRANSFER_COUNT, ASYNC_MAX_TRANSFER_COUNT);
  integer('queueDepth', out.queueDepth, ASYNC_MIN_QUEUE_DEPTH, ASYNC_MAX_QUEUE_DEPTH);
  integer('transferTimeoutMs', out.transferTimeoutMs, 0, 0xffffffff);
  if (out.overflowPolicy !== AsyncOverflowPolicy.DROP_OLDEST && out.overflowPolicy !== AsyncOverflowPolicy.DROP_NEWEST) {
    throw new RangeError('overflowPolicy must be AsyncOverflowPolicy.DROP_OLDEST or DROP_NEWEST');
  }
  if (out.transferSizeBytes * out.queueDepth > ASYNC_MAX_QUEUE_BYTES) {
    throw new RangeError('async queue exceeds 64 MiB');
  }
  if (out.transferSizeBytes * out.transferCount > ASYNC_MAX_TRANSFER_POOL_BYTES) {
    throw new RangeError('async transfer pool exceeds 32 MiB');
  }
  return out;
}

function augmentAsyncStats(stats) {
  return Object.freeze({
    ...stats,
    droppedBuffers: stats.buffersDroppedOldest + stats.buffersDroppedNewest,
    terminal: stats.terminalStatus !== 0,
  });
}

class AsyncStream {
  constructor(device, config = {}, options = {}) {
    if (!(device instanceof Device)) throw new TypeError('device must be a Device');
    this.device = device;
    this.config = normalizeAsyncConfig(config);
    this.readSize = integer('readSize', options.readSize ?? this.config.transferSizeBytes, 1, ASYNC_MAX_QUEUE_BYTES);
    this.timeoutMs = integer('timeoutMs', options.timeoutMs ?? 1000, 0, 0xffffffff);
    this.closed = false;
    device.asyncStart(this.config);
  }

  async read() {
    if (this.closed) return null;
    return this.device.asyncRead(this.readSize, this.timeoutMs);
  }

  async readMany(count) {
    integer('count', count, 0, 1_000_000);
    const blocks = [];
    while (!this.closed && blocks.length < count) {
      const block = await this.read();
      if (block !== null) blocks.push(block);
    }
    return blocks;
  }

  [Symbol.asyncIterator]() {
    return this;
  }

  async next() {
    while (!this.closed) {
      const block = await this.read();
      if (block !== null) return { value: block, done: false };
    }
    return { value: undefined, done: true };
  }

  async return() {
    this.close();
    return { value: undefined, done: true };
  }

  close() {
    if (this.closed) return;
    this.closed = true;
    // terminal producer is not running but native queue still needs stop !! imp
    if (!this.device.closed) this.device.asyncStop();
  }


  get stats() {
    return this.device.asyncStats;
  }
}

class Device {
  constructor(handle) {
    this._handle = handle;
    this.closed = false;
  }

  static open(index = 0) {
    integer('index', index, 0, Number.MAX_SAFE_INTEGER);
    return new Device(call(native.open, index));
  }

  _require() {
    if (this.closed) throw new Error('device is closed');
    return this._handle;
  }

  close() {
    if (this.closed) return;
    call(native.close, this._handle);
    this.closed = true;
  }


  get info() { return call(native.deviceInfo, this._require()); }
  get profile() { return call(native.deviceProfile, this._require()); }
  get mode() { return call(native.mode, this._require()); }
  get state() { return call(native.state, this._require()); }
  get streamInfo() { return call(native.streamInfo, this._require()); }
  get usbStats() { return call(native.usbStats, this._require()); }
  get usbRecoveryStats() { return call(native.usbRecoveryStats, this._require()); }
  get asyncRunning() { return call(native.asyncRunning, this._require()); }
  get asyncStats() { return augmentAsyncStats(call(native.asyncStats, this._require())); }

  resetUsbStats() { call(native.resetUsbStats, this._require()); }
  resetAsyncStats() { call(native.resetAsyncStats, this._require()); }
  stop() { call(native.stop, this._require()); }

  iqStart(frequencyHz, sampleRateHz = 2_400_000, bandwidthHz = 0) {
    frequency('frequencyHz', frequencyHz);
    integer('sampleRateHz', sampleRateHz, 1, 0xffffffff);
    integer('bandwidthHz', bandwidthHz, 0, 0xffffffff);
    return call(native.iqStart, this._require(), frequencyHz, sampleRateHz, bandwidthHz);
  }
  iqTune(frequencyHz) {
    frequency('frequencyHz', frequencyHz);
    return call(native.iqTune, this._require(), frequencyHz);
  }
  iqSetSampleRate(sampleRateHz) {
    integer('sampleRateHz', sampleRateHz, 1, 0xffffffff);
    return call(native.iqSetSampleRate, this._require(), sampleRateHz);
  }
  iqSetBandwidth(bandwidthHz) {
    integer('bandwidthHz', bandwidthHz, 1, 0xffffffff);
    return call(native.iqSetBandwidth, this._require(), bandwidthHz);
  }

  dvbtStart(frequencyHz, bandwidthHz) {
    frequency('frequencyHz', frequencyHz);
    integer('bandwidthHz', bandwidthHz, 1, 0xffffffff);
    return call(native.dvbtStart, this._require(), frequencyHz, bandwidthHz);
  }
  dvbtTune(frequencyHz, bandwidthHz) {
    frequency('frequencyHz', frequencyHz);
    integer('bandwidthHz', bandwidthHz, 1, 0xffffffff);
    return call(native.dvbtTune, this._require(), frequencyHz, bandwidthHz);
  }
  waitLock(timeoutMs = 4000) {
    integer('timeoutMs', timeoutMs, 0, 0xffffffff);
    return call(native.waitLock, this._require(), timeoutMs);
  }
  async waitForLock(timeoutMs = 5000) {
    integer('timeoutMs', timeoutMs, 0, 0xffffffff);
    return callAsync(native.waitForLock, this._require(), timeoutMs);
  }
  signalQuality() { return call(native.signalQuality, this._require()); }

  read(size = 256 * 1024, timeoutMs = 1000) {
    integer('size', size, 1, 0xffffffff);
    integer('timeoutMs', timeoutMs, 0, 0xffffffff);
    return call(native.streamRead, this._require(), size, timeoutMs);
  }

  setPidFilter(pids) {
    if (!Array.isArray(pids)) pids = Array.from(pids);
    for (const pid of pids) integer('PID', pid, 0, 0x1fff);
    call(native.setPidFilter, this._require(), pids);
  }
  disablePidFilter() { call(native.disablePidFilter, this._require()); }

  asyncStart(config = {}) { call(native.asyncStart, this._require(), normalizeAsyncConfig(config)); }
  asyncStop() { call(native.asyncStop, this._require()); }
  async asyncRead(size = defaults.transferSizeBytes, timeoutMs = 1000) {
    integer('size', size, 1, ASYNC_MAX_QUEUE_BYTES);
    integer('timeoutMs', timeoutMs, 0, 0xffffffff);
    return callAsync(native.asyncRead, this._require(), size, timeoutMs);
  }
  stream(config = {}, options = {}) { return new AsyncStream(this, config, options); }

  controlRead(value, index, length) { return call(native.controlRead, this._require(), value, index, length); }
  controlWrite(value, index, data) { call(native.controlWrite, this._require(), value, index, Buffer.from(data)); }
  regRead(reg, length = 1) { return call(native.regRead, this._require(), reg, length); }
  regWrite(reg, data) { call(native.regWrite, this._require(), reg, Buffer.from(data)); }
  demodRead(page, reg, length = 1) { return call(native.demodRead, this._require(), page, reg, length); }
  demodWrite(page, reg, data) { call(native.demodWrite, this._require(), page, reg, Buffer.from(data)); }
  i2cReadReg(address, reg, length = 1) { return call(native.i2cReadReg, this._require(), address, reg, length); }
  i2cWriteReg(address, reg, data) { call(native.i2cWriteReg, this._require(), address, reg, Buffer.from(data)); }
  i2cReadDirect(address, length = 1) { return call(native.i2cReadDirect, this._require(), address, length); }
  i2cWriteDirect(address, data) { call(native.i2cWriteDirect, this._require(), address, Buffer.from(data)); }
  i2cGate(open) { call(native.i2cGate, this._require(), Boolean(open)); }
}

function version() { return native.version(); }
function abiVersion() { return native.abiVersion(); }
function listDevices() { return call(native.listDevices); }

module.exports = {
  version,
  abiVersion,
  listDevices,
  Device,
  AsyncStream,
  SDRNativeError,
  Mode,
  State,
  StreamFormat,
  TunerFamily,
  DemodFamily,
  AsyncOverflowPolicy,
  ASYNC_MIN_TRANSFER_SIZE,
  ASYNC_MAX_TRANSFER_SIZE,
  ASYNC_MIN_TRANSFER_COUNT,
  ASYNC_MAX_TRANSFER_COUNT,
  ASYNC_MIN_QUEUE_DEPTH,
  ASYNC_MAX_QUEUE_DEPTH,
  ASYNC_MAX_QUEUE_BYTES,
  ASYNC_MAX_TRANSFER_POOL_BYTES,
  ASYNC_DEFAULTS: defaults,
};