/*
 * SDRNative
 * Node.js TypeScript declarations
 *
 * Copyright (c) 2026 Ruben Roy
 * SPDX-License-Identifier: MPL-2.0
 */
export declare const Mode: Readonly<{ NONE: 0; DVBT: 1; IQ: 2 }>;
export declare const State: Readonly<{ INVALID: 0; IDLE: 1; DVBT: 2; IQ: 3 }>;
export declare const StreamFormat: Readonly<{ NONE: 0; MPEG_TS: 1; U8_IQ_INTERLEAVED: 2 }>;
export declare const TunerFamily: Readonly<{ UNKNOWN: 0; R820T_FAMILY: 1 }>;
export declare const DemodFamily: Readonly<{ UNKNOWN: 0; RTL2832: 1 }>;
export declare const AsyncOverflowPolicy: Readonly<{ DROP_OLDEST: 0; DROP_NEWEST: 1 }>;

export declare const ASYNC_MIN_TRANSFER_SIZE: number;
export declare const ASYNC_MAX_TRANSFER_SIZE: number;
export declare const ASYNC_MIN_TRANSFER_COUNT: number;
export declare const ASYNC_MAX_TRANSFER_COUNT: number;
export declare const ASYNC_MIN_QUEUE_DEPTH: number;
export declare const ASYNC_MAX_QUEUE_DEPTH: number;
export declare const ASYNC_MAX_QUEUE_BYTES: number;
export declare const ASYNC_MAX_TRANSFER_POOL_BYTES: number;

export interface AsyncConfig {
  transferSizeBytes?: number;
  transferCount?: number;
  queueDepth?: number;
  transferTimeoutMs?: number;
  overflowPolicy?: 0 | 1;
}

export declare const ASYNC_DEFAULTS: Readonly<Required<AsyncConfig>>;

export interface DeviceInfo {
  vid: number;
  pid: number;
  bus: number;
  address: number;
  manufacturer: string;
  product: string;
  serial: string;
}

export interface FrequencyRange { minHz: number; maxHz: number; }
export interface RateRange { minHz: number; maxHz: number; }

export interface DeviceProfile {
  profileVersion: number;
  featureFlags: number;
  tunerFamily: number;
  demodFamily: number;
  maxPidFilters: number;
  tunerXtalHz: number;
  demodXtalHz: number;
  rfRanges: FrequencyRange[];
  iqSampleRateRanges: RateRange[];
  iqBandwidthRanges: RateRange[];
  dvbtBandwidthsHz: number[];
}

export interface IQTuneInfo {
  rfHz: number;
  requestedSampleRateHz: number;
  actualSampleRateHz: number;
  bandwidthHz: number;
  ifHz: number;
  loHz: number;
  tunerPllLocked: boolean;
}

export interface DVBTTuneInfo {
  rfHz: number;
  bandwidthHz: number;
  ifHz: number;
  loHz: number;
  tunerPllLocked: boolean;
  filterCalCode: number;
}

export interface StreamInfo {
  format: number;
  sampleRateHz: number;
  packetSizeBytes: number;
  bytesPerComplexSample: number;
}

export interface SignalQuality {
  fsmStage: number;
  strengthPercent: number;
  snrDb: number | null;
  ber: number | null;
  berRaw: number;
}

export interface USBStats {
  controlTransfers: bigint;
  controlErrors: bigint;
  controlPipeEvents: bigint;
  sessionRecoveryAttempts: bigint;
  sessionRecoverySuccesses: bigint;
  sessionRecoveryFailures: bigint;
  bulkReads: bigint;
  bulkTimeouts: bigint;
  bulkErrors: bigint;
  bulkBytes: bigint;
}

export interface USBRecoveryStats {
  statsVersion: number;
  controlPipeEvents: bigint;
  sessionReopenAttempts: bigint;
  sessionReopenSuccesses: bigint;
  sessionReopenFailures: bigint;
  transactionRetryAttempts: bigint;
  transactionRetrySuccesses: bigint;
  transactionRetryFailures: bigint;
}

export interface AsyncStats {
  statsVersion: number;
  running: boolean;
  queueCurrent: number;
  queueHighWater: number;
  transferCallbacks: bigint;
  transfersCompleted: bigint;
  transferBytes: bigint;
  transferTimeouts: bigint;
  transferErrors: bigint;
  buffersEnqueued: bigint;
  buffersDequeued: bigint;
  buffersDroppedOldest: bigint;
  buffersDroppedNewest: bigint;
  consumerReads: bigint;
  consumerBytes: bigint;
  buffersDiscardedOnStop: bigint;
  terminalStatus: number;
  terminalNativeCode: number;
  droppedBuffers: bigint;
  terminal: boolean;
}

export interface AsyncBlock {
  data: Uint8Array;
  sequence: bigint;
}

export interface AsyncStreamOptions {
  readSize?: number;
  timeoutMs?: number;
}

export class SDRNativeError extends Error {
  status: number;
  domain: number;
  nativeCode: number;
  flags: number;
  terminalStream: boolean;
  deviceGone: boolean;
}

export class AsyncStream implements AsyncIterableIterator<AsyncBlock> {
  readonly device: Device;
  readonly config: Required<AsyncConfig>;
  readonly readSize: number;
  readonly timeoutMs: number;
  closed: boolean;
  read(): Promise<AsyncBlock | null>;
  readMany(count: number): Promise<AsyncBlock[]>;
  next(): Promise<IteratorResult<AsyncBlock>>;
  return(): Promise<IteratorResult<AsyncBlock>>;
  close(): void;
  readonly stats: AsyncStats;
  [Symbol.asyncIterator](): AsyncStream;
}

export class Device {
  static open(index?: number): Device;
  readonly closed: boolean;
  readonly info: DeviceInfo;
  readonly profile: DeviceProfile;
  readonly mode: number;
  readonly state: number;
  readonly streamInfo: StreamInfo;
  readonly usbStats: USBStats;
  readonly usbRecoveryStats: USBRecoveryStats;
  readonly asyncRunning: boolean;
  readonly asyncStats: AsyncStats;

  close(): void;
  stop(): void;
  resetUsbStats(): void;
  resetAsyncStats(): void;

  iqStart(frequencyHz: number | bigint, sampleRateHz?: number, bandwidthHz?: number): IQTuneInfo;
  iqTune(frequencyHz: number | bigint): IQTuneInfo;
  iqSetSampleRate(sampleRateHz: number): number;
  iqSetBandwidth(bandwidthHz: number): IQTuneInfo;

  dvbtStart(frequencyHz: number | bigint, bandwidthHz: number): DVBTTuneInfo;
  dvbtTune(frequencyHz: number | bigint, bandwidthHz: number): DVBTTuneInfo;
  waitLock(timeoutMs?: number): number;
  waitForLock(timeoutMs?: number): Promise<number>;
  signalQuality(): SignalQuality;

  read(size?: number, timeoutMs?: number): Uint8Array;
  setPidFilter(pids: Iterable<number>): void;
  disablePidFilter(): void;

  asyncStart(config?: AsyncConfig): void;
  asyncStop(): void;
  asyncRead(size?: number, timeoutMs?: number): Promise<AsyncBlock | null>;
  stream(config?: AsyncConfig, options?: AsyncStreamOptions): AsyncStream;

  controlRead(value: number, index: number, length: number): Uint8Array;
  controlWrite(value: number, index: number, data: Uint8Array): void;
  regRead(reg: number, length?: number): Uint8Array;
  regWrite(reg: number, data: Uint8Array): void;
  demodRead(page: number, reg: number, length?: number): Uint8Array;
  demodWrite(page: number, reg: number, data: Uint8Array): void;
  i2cReadReg(address: number, reg: number, length?: number): Uint8Array;
  i2cWriteReg(address: number, reg: number, data: Uint8Array): void;
  i2cReadDirect(address: number, length?: number): Uint8Array;
  i2cWriteDirect(address: number, data: Uint8Array): void;
  i2cGate(open: boolean): void;
}

export function version(): string;
export function abiVersion(): number;
export function listDevices(): DeviceInfo[];
