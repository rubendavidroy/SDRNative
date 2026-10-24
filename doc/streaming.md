# Streaming and device lifetime

The interface is defined in [`include/sdrnative.h`](../include/sdrnative.h).
One `sdrn_device` handle owns one active mode (IQ or DVB-T). Open with
`sdrn_open()`, select a mode with `sdrn_iq_start()` or `sdrn_dvbt_start()`,
and stop with `sdrn_stop()`. `sdrn_close()` stops transfers and releases the
handle. Control operations and retuning must be serialized by the caller.

`sdrn_stream_read()` writes into a caller-owned buffer. A synchronous USB
read timeout normally returns `SDRN_OK` with a zero byte count.

Async transfer state is per handle. Start a mode before `sdrn_async_start()`;
initialize configuration using `sdrn_async_config_init()`. The USB backend
keeps transfers in flight and copies completions into a bounded native queue.
`sdrn_async_read()` copies from that queue. No user callback runs on the USB
event thread.

- Only one async consumer is supported per handle.
- A consumer wait timeout returns `SDRN_ERR_TIMEOUT`, unlike synchronous reads.
- Producer sequence numbers are assigned to nonempty transfer completions.
  Partial reads retain the same number; gaps can indicate dropped buffers.
  Sequence wraps modulo `2^64`.
- Overflow uses `SDRN_ASYNC_DROP_OLDEST` or `SDRN_ASYNC_DROP_NEWEST`; inspect
  `sdrn_async_get_stats()` for dropped-buffer counts.
- `sdrn_async_stop()` cancels/drains transfers and wakes a blocked consumer.
  Another thread may call `sdrn_async_stop()` or `sdrn_stop()` to interrupt a
  blocked read. Other concurrent operations on the handle require external
  synchronization.
- Synchronous reads and hardware control calls cannot be used while async
  transfers are active. Stop the engine before reconfiguring the device.

A producer error may be reported after already queued data is consumed.
A device-gone error requires closing the old handle and reopening after
re-enumeration. Inspect `sdrn_get_last_error_info()` and status/error flags,
not the diagnostic message text.

Public structs with reserved fields must not be modified; `sdrn_get_abi_info()`
reports the ABI layout for foreign-language bindings. The API header contains
the async sizing limits and the remaining interface definitions.