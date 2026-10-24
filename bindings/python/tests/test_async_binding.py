# SDRNative
# Python binding tests
#
# Copyright (c) 2026 Ruben Roy
# SPDX-License-Identifier: MPL-2.0
import ctypes as C
import unittest

import sdrnative
import sdrnative._native as native


class AsyncBindingContractTests(unittest.TestCase):
    def test_runtime_and_abi(self):
        self.assertEqual(sdrnative.version(), "0.1.0")
        self.assertEqual(sdrnative.abi_version(), 1)

    def test_dvbt_tune_info_converter_is_instance_callable(self):
        c = native._TuneInfoC()
        c.rf_hz = 557_500_000
        c.bandwidth_hz = 7_000_000
        c.if_hz = 4_570_000
        c.lo_hz = 562_070_000
        c.tuner_pll_locked = 1
        c.filter_cal_code = 7

        # tune info conversion must stay callable through device instance
        dev = native.Device(C.c_void_p(1))
        tune = dev._tune(c)
        self.assertEqual(tune.rf_hz, 557_500_000)
        self.assertEqual(tune.bandwidth_hz, 7_000_000)
        self.assertEqual(tune.if_hz, 4_570_000)
        self.assertEqual(tune.lo_hz, 562_070_000)
        self.assertTrue(tune.tuner_pll_locked)
        self.assertEqual(tune.filter_cal_code, 7)

    def test_async_defaults(self):
        cfg = sdrnative.AsyncConfig()
        self.assertEqual(cfg.transfer_size_bytes, 64 * 1024)
        self.assertEqual(cfg.transfer_count, 8)
        self.assertEqual(cfg.queue_depth, 16)
        self.assertEqual(cfg.transfer_timeout_ms, 0)
        self.assertEqual(cfg.overflow_policy, sdrnative.AsyncOverflowPolicy.DROP_OLDEST)

        c = native._async_config_c(cfg)
        self.assertEqual(c.struct_size, C.sizeof(native._AsyncConfigC))
        self.assertEqual(c.config_version, 1)
        self.assertEqual(c.transfer_size_bytes, cfg.transfer_size_bytes)
        self.assertEqual(c.transfer_count, cfg.transfer_count)
        self.assertEqual(c.queue_depth, cfg.queue_depth)
        self.assertEqual(c.overflow_policy, int(cfg.overflow_policy))

    def test_geometry_validation(self):
        bad = (
            {"transfer_size_bytes": sdrnative.ASYNC_MIN_TRANSFER_SIZE - 1},
            {"transfer_size_bytes": sdrnative.ASYNC_MAX_TRANSFER_SIZE + 1},
            {"transfer_count": sdrnative.ASYNC_MIN_TRANSFER_COUNT - 1},
            {"transfer_count": sdrnative.ASYNC_MAX_TRANSFER_COUNT + 1},
            {"queue_depth": sdrnative.ASYNC_MIN_QUEUE_DEPTH - 1},
            {"queue_depth": sdrnative.ASYNC_MAX_QUEUE_DEPTH + 1},
            {"overflow_policy": 99},
        )
        for kwargs in bad:
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                sdrnative.AsyncConfig(**kwargs)

    def test_aggregate_caps(self):
        with self.assertRaises(ValueError):
            sdrnative.AsyncConfig(
                transfer_size_bytes=sdrnative.ASYNC_MAX_TRANSFER_SIZE,
                queue_depth=128,
            )
        cfg = sdrnative.AsyncConfig(
            transfer_size_bytes=sdrnative.ASYNC_MAX_TRANSFER_SIZE,
            transfer_count=sdrnative.ASYNC_MAX_TRANSFER_COUNT,
            queue_depth=16,
        )
        self.assertEqual(
            cfg.transfer_size_bytes * cfg.transfer_count,
            sdrnative.ASYNC_MAX_TRANSFER_POOL_BYTES,
        )

    def test_stream_stops_terminal_native_state(self):
        class DeviceDouble:
            closed = False
            async_running = False
            calls = 0

            def async_stop(self):
                self.calls += 1

        dev = DeviceDouble()
        stream = sdrnative.AsyncStream(dev)
        stream.stop()
        self.assertEqual(dev.calls, 1)

    def test_iterator_reads_terminal_queue_before_stopping(self):
        class DeviceDouble:
            closed = False
            async_running = False

            def async_read(self, **kwargs):
                raise RuntimeError("terminal stream failure")

        stream = sdrnative.AsyncStream(DeviceDouble())
        with self.assertRaisesRegex(RuntimeError, "terminal stream failure"):
            next(stream)

    def test_block(self):
        block = sdrnative.AsyncBlock(b"abc", (1 << 64) - 1)
        self.assertEqual(len(block), 3)
        self.assertEqual(block.data, b"abc")
        self.assertEqual(block.sequence, (1 << 64) - 1)

    def test_timeout_conversion(self):
        self.assertEqual(native._async_timeout(None), sdrnative.ASYNC_WAIT_INFINITE)
        self.assertEqual(native._async_timeout(0), 0)
        self.assertEqual(native._async_timeout(1000), 1000)
        with self.assertRaises(ValueError):
            native._async_timeout(-1)
        with self.assertRaises(ValueError):
            native._async_timeout(1 << 32)


if __name__ == "__main__":
    unittest.main()