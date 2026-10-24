# SDRNative
# Python API tests
#
# Copyright (c) 2026 Ruben Roy
# SPDX-License-Identifier: MPL-2.0
import unittest
from pathlib import Path

import sdrnative


class _FakeAsyncDevice:
    def __init__(self, reads=None):
        self.closed = False
        self._running = False
        self.reads = list(reads or [])
        self.starts = []
        self.stops = 0
        self._stats = sdrnative.AsyncStats(
            stats_version=1,
            running=False,
            queue_current=0,
            queue_high_water=0,
            transfer_callbacks=0,
            transfers_completed=0,
            transfer_bytes=0,
            transfer_timeouts=0,
            transfer_errors=0,
            buffers_enqueued=0,
            buffers_dequeued=0,
            buffers_dropped_oldest=0,
            buffers_dropped_newest=0,
            consumer_reads=0,
            consumer_bytes=0,
            buffers_discarded_on_stop=0,
            terminal_status=0,
            terminal_native_code=0,
        )

    @property
    def async_running(self):
        return self._running

    @property
    def async_stats(self):
        return self._stats

    def async_start(self, config=None):
        self.starts.append(config)
        self._running = True

    def async_stop(self):
        self.stops += 1
        self._running = False

    def async_read(self, *, size, timeout_ms):
        if not self._running:
            raise sdrnative.SDRNativeStateError(-3, "stopped")
        if not self.reads:
            self._running = False
            raise sdrnative.SDRNativeStateError(-3, "stopped")
        item = self.reads.pop(0)
        if isinstance(item, Exception):
            raise item
        return item


class PythonErgonomicsTests(unittest.TestCase):
    def test_native_library_path_is_exposed(self):
        path = sdrnative.native_library_path()
        self.assertIsInstance(path, str)
        self.assertTrue(path)

    def test_async_block_bytes_and_nbytes(self):
        block = sdrnative.AsyncBlock(b"abc", 42)
        self.assertEqual(block.nbytes, 3)
        self.assertEqual(bytes(block), b"abc")

    def test_async_stats_convenience_properties(self):
        stats = sdrnative.AsyncStats(
            stats_version=1,
            running=False,
            queue_current=0,
            queue_high_water=4,
            transfer_callbacks=9,
            transfers_completed=8,
            transfer_bytes=123,
            transfer_timeouts=0,
            transfer_errors=0,
            buffers_enqueued=8,
            buffers_dequeued=5,
            buffers_dropped_oldest=2,
            buffers_dropped_newest=1,
            consumer_reads=5,
            consumer_bytes=100,
            buffers_discarded_on_stop=0,
            terminal_status=0,
            terminal_native_code=0,
        )
        self.assertEqual(stats.dropped_buffers, 3)
        self.assertFalse(stats.terminal)

    def test_async_stats_terminal_property(self):
        fields = dict(
            stats_version=1, running=False, queue_current=0, queue_high_water=0,
            transfer_callbacks=0, transfers_completed=0, transfer_bytes=0,
            transfer_timeouts=0, transfer_errors=1, buffers_enqueued=0,
            buffers_dequeued=0, buffers_dropped_oldest=0, buffers_dropped_newest=0,
            consumer_reads=0, consumer_bytes=0, buffers_discarded_on_stop=0,
            terminal_status=-5, terminal_native_code=-9,
        )
        self.assertTrue(sdrnative.AsyncStats(**fields).terminal)

    def test_stream_is_single_consumer_iterator_and_skips_timeouts(self):
        fake = _FakeAsyncDevice([
            None,
            sdrnative.AsyncBlock(b"one", 7),
            None,
            sdrnative.AsyncBlock(b"two", 8),
        ])
        stream = sdrnative.AsyncStream(fake, read_size=4096, timeout_ms=5)
        stream.start()
        self.assertIs(iter(stream), stream)
        self.assertEqual(next(stream).sequence, 7)
        self.assertEqual(next(stream).data, b"two")
        stream.stop()

    def test_read_many_reads_non_timeout_blocks(self):
        fake = _FakeAsyncDevice([
            None,
            sdrnative.AsyncBlock(b"a", 10),
            None,
            sdrnative.AsyncBlock(b"b", 11),
        ])
        stream = sdrnative.AsyncStream(fake, read_size=4096, timeout_ms=5).start()
        blocks = stream.read_many(2)
        self.assertEqual([b.sequence for b in blocks], [10, 11])
        stream.stop()
        with self.assertRaises(ValueError):
            stream.read_many(-1)

    def test_context_manager_owns_start_and_stop(self):
        cfg = sdrnative.AsyncConfig(queue_depth=4)
        fake = _FakeAsyncDevice([sdrnative.AsyncBlock(b"x", 0)])
        with sdrnative.AsyncStream(fake, config=cfg) as stream:
            self.assertTrue(stream.running)
            self.assertIs(fake.starts[0], cfg)
            self.assertEqual(next(stream).data, b"x")
        self.assertFalse(fake.async_running)
        self.assertEqual(fake.stops, 1)

    def test_py_typed_marker_is_packaged_in_source_tree(self):
        marker = Path(sdrnative.__file__).with_name("py.typed")
        self.assertTrue(marker.is_file())


if __name__ == "__main__":
    unittest.main()