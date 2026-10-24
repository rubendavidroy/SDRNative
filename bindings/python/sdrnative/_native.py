# SDRNative
# Python native binding
#
# Copyright (c) 2026 Ruben Roy
# SPDX-License-Identifier: MPL-2.0
from __future__ import annotations

import ctypes as C
import atexit
from contextlib import ExitStack
from importlib.resources import as_file, files
from dataclasses import dataclass
from pathlib import Path
import os
import platform
from typing import Iterable, Iterator
from enum import IntEnum, IntFlag


class ErrorDomain(IntEnum):
    NONE = 0
    ARGUMENT = 1
    STATE = 2
    USB = 3
    TUNER = 4
    DEMOD = 5
    STREAM = 6
    INTERNAL = 7


class ErrorFlags(IntFlag):
    NONE = 0
    TERMINAL_STREAM = 1 << 0
    DEVICE_GONE = 1 << 1


class TunerFamily(IntEnum):
    UNKNOWN = 0
    R820T_FAMILY = 1


class DemodFamily(IntEnum):
    UNKNOWN = 0
    RTL2832 = 1


class AsyncOverflowPolicy(IntEnum):
    DROP_OLDEST = 0
    DROP_NEWEST = 1


ASYNC_MIN_TRANSFER_SIZE = 4 * 1024
ASYNC_MAX_TRANSFER_SIZE = 1024 * 1024
ASYNC_MIN_TRANSFER_COUNT = 2
ASYNC_MAX_TRANSFER_COUNT = 32
ASYNC_MIN_QUEUE_DEPTH = 2
ASYNC_MAX_QUEUE_DEPTH = 128
ASYNC_MAX_QUEUE_BYTES = 64 * 1024 * 1024
ASYNC_MAX_TRANSFER_POOL_BYTES = 32 * 1024 * 1024
ASYNC_DEFAULT_TRANSFER_SIZE = 64 * 1024
ASYNC_DEFAULT_TRANSFER_COUNT = 8
ASYNC_DEFAULT_QUEUE_DEPTH = 16
ASYNC_WAIT_INFINITE = (1 << 32) - 1


@dataclass(frozen=True)
class ErrorInfo:
    status: int
    domain: ErrorDomain
    native_code: int
    flags: ErrorFlags
    message: str


class SDRNativeError(RuntimeError):
    def __init__(self, status: int, message: str, info: ErrorInfo | None = None):
        super().__init__(message)
        self.status = status
        self.info = info
        self.domain = info.domain if info else ErrorDomain.NONE
        self.native_code = info.native_code if info else 0


class SDRNativeArgumentError(SDRNativeError):
    pass


class SDRNativeStateError(SDRNativeError):
    pass


class SDRNativeUSBError(SDRNativeError):
    pass


class SDRNativeTunerError(SDRNativeError):
    pass


class SDRNativeDemodError(SDRNativeError):
    pass


class SDRNativeStreamError(SDRNativeError):
    pass


class SDRNativeInternalError(SDRNativeError):
    pass


class _DeviceInfoC(C.Structure):
    _fields_ = [
        ("vid", C.c_uint16),
        ("pid", C.c_uint16),
        ("bus", C.c_uint8),
        ("address", C.c_uint8),
        ("manufacturer", C.c_char * 128),
        ("product", C.c_char * 128),
        ("serial", C.c_char * 128),
    ]


class _CapabilitiesC(C.Structure):
    _fields_ = [
        ("flags", C.c_uint32),
        ("dvbt_bandwidth_mask", C.c_uint32),
        ("max_pid_filters", C.c_uint32),
        ("tuner_xtal_hz", C.c_uint32),
        ("demod_xtal_hz", C.c_uint32),
    ]


class _FrequencyRangeC(C.Structure):
    _fields_ = [("min_hz", C.c_uint64), ("max_hz", C.c_uint64)]


class _RateRangeC(C.Structure):
    _fields_ = [("min_hz", C.c_uint32), ("max_hz", C.c_uint32)]


class _DeviceProfileC(C.Structure):
    _fields_ = [
        ("profile_version", C.c_uint32),
        ("feature_flags", C.c_uint32),
        ("tuner_family", C.c_int32),
        ("demod_family", C.c_int32),
        ("max_pid_filters", C.c_uint32),
        ("tuner_xtal_hz", C.c_uint32),
        ("demod_xtal_hz", C.c_uint32),
        ("rf_range_count", C.c_uint32),
        ("rf_ranges", _FrequencyRangeC * 4),
        ("iq_sample_rate_range_count", C.c_uint32),
        ("iq_sample_rate_ranges", _RateRangeC * 4),
        ("iq_bandwidth_range_count", C.c_uint32),
        ("iq_bandwidth_ranges", _RateRangeC * 4),
        ("dvbt_bandwidth_count", C.c_uint32),
        ("dvbt_bandwidth_hz", C.c_uint32 * 8),
        ("reserved", C.c_uint32 * 8),
    ]


class _TuneInfoC(C.Structure):
    _fields_ = [
        ("rf_hz", C.c_uint64),
        ("bandwidth_hz", C.c_uint32),
        ("if_hz", C.c_uint32),
        ("lo_hz", C.c_uint64),
        ("tuner_pll_locked", C.c_uint8),
        ("filter_cal_code", C.c_uint8),
        ("reserved", C.c_uint8 * 6),
    ]


class _IQTuneInfoC(C.Structure):
    _fields_ = [
        ("rf_hz", C.c_uint64),
        ("requested_sample_rate_hz", C.c_uint32),
        ("actual_sample_rate_hz", C.c_uint32),
        ("bandwidth_hz", C.c_uint32),
        ("if_hz", C.c_uint32),
        ("lo_hz", C.c_uint64),
        ("tuner_pll_locked", C.c_uint8),
        ("reserved", C.c_uint8 * 7),
    ]


class _StreamInfoC(C.Structure):
    _fields_ = [
        ("format", C.c_int),
        ("sample_rate_hz", C.c_uint32),
        ("packet_size_bytes", C.c_uint32),
        ("bytes_per_complex_sample", C.c_uint32),
    ]


class _SignalQualityC(C.Structure):
    _fields_ = [
        ("fsm_stage", C.c_uint8),
        ("snr_valid", C.c_uint8),
        ("ber_valid", C.c_uint8),
        ("reserved0", C.c_uint8),
        ("strength_percent", C.c_int32),
        ("snr_db", C.c_double),
        ("ber", C.c_uint32),
    ]


class _USBStatsC(C.Structure):
    _fields_ = [
        ("control_transfers", C.c_uint64),
        ("control_errors", C.c_uint64),
        ("control_pipe_events", C.c_uint64),
        ("session_recovery_attempts", C.c_uint64),
        ("session_recovery_successes", C.c_uint64),
        ("session_recovery_failures", C.c_uint64),
        ("bulk_reads", C.c_uint64),
        ("bulk_timeouts", C.c_uint64),
        ("bulk_errors", C.c_uint64),
        ("bulk_bytes", C.c_uint64),
    ]


class _USBRecoveryStatsC(C.Structure):
    _fields_ = [
        ("struct_size", C.c_uint32),
        ("stats_version", C.c_uint32),
        ("control_pipe_events", C.c_uint64),
        ("session_reopen_attempts", C.c_uint64),
        ("session_reopen_successes", C.c_uint64),
        ("session_reopen_failures", C.c_uint64),
        ("transaction_retry_attempts", C.c_uint64),
        ("transaction_retry_successes", C.c_uint64),
        ("transaction_retry_failures", C.c_uint64),
        ("reserved", C.c_uint64 * 4),
    ]


class _AsyncConfigC(C.Structure):
    _fields_ = [
        ("struct_size", C.c_uint32),
        ("config_version", C.c_uint32),
        ("transfer_size_bytes", C.c_uint32),
        ("transfer_count", C.c_uint32),
        ("queue_depth", C.c_uint32),
        ("transfer_timeout_ms", C.c_uint32),
        ("overflow_policy", C.c_int32),
        ("reserved", C.c_uint32 * 9),
    ]


class _AsyncStatsC(C.Structure):
    _fields_ = [
        ("struct_size", C.c_uint32),
        ("stats_version", C.c_uint32),
        ("running", C.c_uint32),
        ("queue_current", C.c_uint32),
        ("queue_high_water", C.c_uint32),
        ("reserved0", C.c_uint32),
        ("transfer_callbacks", C.c_uint64),
        ("transfers_completed", C.c_uint64),
        ("transfer_bytes", C.c_uint64),
        ("transfer_timeouts", C.c_uint64),
        ("transfer_errors", C.c_uint64),
        ("buffers_enqueued", C.c_uint64),
        ("buffers_dequeued", C.c_uint64),
        ("buffers_dropped_oldest", C.c_uint64),
        ("buffers_dropped_newest", C.c_uint64),
        ("consumer_reads", C.c_uint64),
        ("consumer_bytes", C.c_uint64),
        ("buffers_discarded_on_stop", C.c_uint64),
        ("terminal_status", C.c_int32),
        ("terminal_native_code", C.c_int32),
        ("reserved", C.c_uint64 * 4),
    ]


class _ErrorInfoC(C.Structure):
    _fields_ = [
        ("status", C.c_int32),
        ("domain", C.c_int32),
        ("native_code", C.c_int32),
        ("flags", C.c_uint32),
        ("reserved", C.c_uint32 * 4),
        ("message", C.c_char * 256),
    ]


class _ABIInfoC(C.Structure):
    _fields_ = [
        ("struct_size", C.c_uint32),
        ("info_version", C.c_uint32),
        ("abi_version", C.c_uint32),
        ("flags", C.c_uint32),
        ("pointer_size", C.c_uint32),
        ("status_size", C.c_uint32),
        ("log_level_size", C.c_uint32),
        ("mode_size", C.c_uint32),
        ("state_size", C.c_uint32),
        ("error_domain_size", C.c_uint32),
        ("stream_format_size", C.c_uint32),
        ("tuner_family_size", C.c_uint32),
        ("demod_family_size", C.c_uint32),
        ("device_info_size", C.c_uint32),
        ("device_info_align", C.c_uint32),
        ("capabilities_size", C.c_uint32),
        ("capabilities_align", C.c_uint32),
        ("error_info_size", C.c_uint32),
        ("error_info_align", C.c_uint32),
        ("frequency_range_size", C.c_uint32),
        ("frequency_range_align", C.c_uint32),
        ("rate_range_size", C.c_uint32),
        ("rate_range_align", C.c_uint32),
        ("device_profile_size", C.c_uint32),
        ("device_profile_align", C.c_uint32),
        ("dvbt_tune_info_size", C.c_uint32),
        ("dvbt_tune_info_align", C.c_uint32),
        ("iq_tune_info_size", C.c_uint32),
        ("iq_tune_info_align", C.c_uint32),
        ("stream_info_size", C.c_uint32),
        ("stream_info_align", C.c_uint32),
        ("signal_quality_size", C.c_uint32),
        ("signal_quality_align", C.c_uint32),
        ("usb_stats_size", C.c_uint32),
        ("usb_stats_align", C.c_uint32),
        ("reserved", C.c_uint32 * 8),
    ]


def _text(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("utf-8", "replace")


@dataclass(frozen=True)
class ABIInfo:
    abi_version: int
    info_version: int
    flags: int
    pointer_size: int
    device_profile_size: int
    error_info_size: int
    usb_stats_size: int


@dataclass(frozen=True)
class DeviceInfo:
    vid: int
    pid: int
    bus: int
    address: int
    manufacturer: str
    product: str
    serial: str


@dataclass(frozen=True)
class Capabilities:
    flags: int
    dvbt_bandwidths_mhz: tuple[int, ...]
    max_pid_filters: int
    tuner_xtal_hz: int
    demod_xtal_hz: int


@dataclass(frozen=True)
class FrequencyRange:
    min_hz: int
    max_hz: int


@dataclass(frozen=True)
class RateRange:
    min_hz: int
    max_hz: int


@dataclass(frozen=True)
class DeviceProfile:
    profile_version: int
    feature_flags: int
    tuner_family: TunerFamily
    demod_family: DemodFamily
    max_pid_filters: int
    tuner_xtal_hz: int
    demod_xtal_hz: int
    rf_ranges: tuple[FrequencyRange, ...]
    iq_sample_rate_ranges: tuple[RateRange, ...]
    iq_bandwidth_ranges: tuple[RateRange, ...]
    dvbt_bandwidths_hz: tuple[int, ...]


@dataclass(frozen=True)
class DVBTTuneInfo:
    rf_hz: int
    bandwidth_hz: int
    if_hz: int
    lo_hz: int
    tuner_pll_locked: bool
    filter_cal_code: int


@dataclass(frozen=True)
class IQTuneInfo:
    rf_hz: int
    requested_sample_rate_hz: int
    actual_sample_rate_hz: int
    bandwidth_hz: int
    if_hz: int
    lo_hz: int
    tuner_pll_locked: bool


@dataclass(frozen=True)
class StreamInfo:
    format: int
    sample_rate_hz: int
    packet_size_bytes: int
    bytes_per_complex_sample: int


@dataclass(frozen=True)
class SignalQuality:
    fsm_stage: int
    strength_percent: int
    snr_db: float | None
    ber: int | None
    ber_raw: int


@dataclass(frozen=True)
class USBStats:
    control_transfers: int
    control_errors: int
    control_pipe_events: int
    session_recovery_attempts: int
    session_recovery_successes: int
    session_recovery_failures: int
    bulk_reads: int
    bulk_timeouts: int
    bulk_errors: int
    bulk_bytes: int


@dataclass(frozen=True)
class USBRecoveryStats:
    stats_version: int
    control_pipe_events: int
    session_reopen_attempts: int
    session_reopen_successes: int
    session_reopen_failures: int
    transaction_retry_attempts: int
    transaction_retry_successes: int
    transaction_retry_failures: int


@dataclass(frozen=True)
class AsyncConfig:
    transfer_size_bytes: int = ASYNC_DEFAULT_TRANSFER_SIZE
    transfer_count: int = ASYNC_DEFAULT_TRANSFER_COUNT
    queue_depth: int = ASYNC_DEFAULT_QUEUE_DEPTH
    transfer_timeout_ms: int = 0
    overflow_policy: AsyncOverflowPolicy = AsyncOverflowPolicy.DROP_OLDEST

    def __post_init__(self) -> None:
        try:
            policy = AsyncOverflowPolicy(int(self.overflow_policy))
        except (TypeError, ValueError) as exc:
            raise ValueError("overflow_policy must be AsyncOverflowPolicy.DROP_OLDEST or DROP_NEWEST") from exc
        object.__setattr__(self, "overflow_policy", policy)

        if not ASYNC_MIN_TRANSFER_SIZE <= int(self.transfer_size_bytes) <= ASYNC_MAX_TRANSFER_SIZE:
            raise ValueError(
                f"transfer_size_bytes must be {ASYNC_MIN_TRANSFER_SIZE}..{ASYNC_MAX_TRANSFER_SIZE}"
            )
        if not ASYNC_MIN_TRANSFER_COUNT <= int(self.transfer_count) <= ASYNC_MAX_TRANSFER_COUNT:
            raise ValueError(
                f"transfer_count must be {ASYNC_MIN_TRANSFER_COUNT}..{ASYNC_MAX_TRANSFER_COUNT}"
            )
        if not ASYNC_MIN_QUEUE_DEPTH <= int(self.queue_depth) <= ASYNC_MAX_QUEUE_DEPTH:
            raise ValueError(
                f"queue_depth must be {ASYNC_MIN_QUEUE_DEPTH}..{ASYNC_MAX_QUEUE_DEPTH}"
            )
        if not 0 <= int(self.transfer_timeout_ms) <= ASYNC_WAIT_INFINITE:
            raise ValueError("transfer_timeout_ms must fit uint32")
        if int(self.transfer_size_bytes) * int(self.queue_depth) > ASYNC_MAX_QUEUE_BYTES:
            raise ValueError(f"async queue exceeds {ASYNC_MAX_QUEUE_BYTES} bytes")
        if int(self.transfer_size_bytes) * int(self.transfer_count) > ASYNC_MAX_TRANSFER_POOL_BYTES:
            raise ValueError(
                f"async transfer pool payload exceeds {ASYNC_MAX_TRANSFER_POOL_BYTES} bytes"
            )


@dataclass(frozen=True)
class AsyncBlock:
    data: bytes
    sequence: int

    def __len__(self) -> int:
        return len(self.data)

    @property
    def nbytes(self) -> int:
        # n of payload bytes in this producer block/read fragment
        return len(self.data)

    def __bytes__(self) -> bytes:
        return self.data


@dataclass(frozen=True)
class AsyncStats:
    stats_version: int
    running: bool
    queue_current: int
    queue_high_water: int
    transfer_callbacks: int
    transfers_completed: int
    transfer_bytes: int
    transfer_timeouts: int
    transfer_errors: int
    buffers_enqueued: int
    buffers_dequeued: int
    buffers_dropped_oldest: int
    buffers_dropped_newest: int
    consumer_reads: int
    consumer_bytes: int
    buffers_discarded_on_stop: int
    terminal_status: int
    terminal_native_code: int

    @property
    def dropped_buffers(self) -> int:
        # total queue overflow drops across both configured policies
        return self.buffers_dropped_oldest + self.buffers_dropped_newest

    @property
    def terminal(self) -> bool:
        # whether the native async engine recorded a terminal status
        return self.terminal_status != 0


_LOG_CALLBACK = C.CFUNCTYPE(None, C.c_void_p, C.c_int, C.c_char_p)


_resource_stack = ExitStack()
atexit.register(_resource_stack.close)


def _candidate_library_paths() -> list[Path]:
    explicit = os.environ.get("SDRNATIVE_LIBRARY")
    if explicit:
        return [Path(explicit).expanduser()]

    here = Path(__file__).resolve()
    repo = here.parents[3]
    system = platform.system()
    if system == "Darwin":
        names = ["libsdrnative.dylib", "libsdrnative.0.dylib"]
        dirs = [repo / "build", repo / "build" / "lib", repo / "dist" / "lib"]
    elif system == "Windows":
        names = ["sdrnative.dll", "libsdrnative.dll"]
        dirs = [
            repo / "build-win" / "Release",
            repo / "build-win" / "Debug",
            repo / "build",
            repo / "build" / "Release",
            repo / "build" / "Debug",
            repo / "dist" / "bin",
        ]
    else:
        names = ["libsdrnative.so", "libsdrnative.so.0"]
        dirs = [repo / "build", repo / "build" / "lib", repo / "dist" / "lib"]

    candidates: list[Path] = []
    for name in names:
        resource = files("sdrnative").joinpath(name)
        if resource.is_file():
            candidates.append(Path(_resource_stack.enter_context(as_file(resource))))
    for d in dirs:
        for name in names:
            candidates.append(d / name)
    return candidates


def _load_library() -> C.CDLL:
    attempts: list[str] = []
    for path in _candidate_library_paths():
        attempts.append(str(path))
        if path.exists():
            return C.CDLL(str(path))

    # last fallback asks dynamic linker by soname
    sonames = {
        "Darwin": ["libsdrnative.dylib"],
        "Windows": ["sdrnative.dll"],
    }.get(platform.system(), ["libsdrnative.so"])
    for name in sonames:
        attempts.append(name)
        try:
            return C.CDLL(name)
        except OSError:
            pass

    raise OSError(
        "SDRNative library not found; build it or set "
        "SDRNATIVE_LIBRARY. Searched:\n  " + "\n  ".join(attempts)
    )


_lib = _load_library()


def native_library_path() -> str:
    # return the path/name used to load the canonical SDRNative C library
    return str(_lib._name)


# native signatures
_lib.sdrn_version_string.restype = C.c_char_p
try:
    _lib.sdrn_abi_version.restype = C.c_uint32
    _lib.sdrn_get_abi_info.argtypes = [C.POINTER(_ABIInfoC)]
    _lib.sdrn_get_abi_info.restype = C.c_int
except AttributeError as exc:
    raise OSError(
        "loaded SDRNative library lacks ABI v1 metadata; "
        "check the installed library version"
    ) from exc
_lib.sdrn_status_string.argtypes = [C.c_int]
_lib.sdrn_status_string.restype = C.c_char_p
_lib.sdrn_last_global_error.restype = C.c_char_p
_lib.sdrn_get_last_global_error_info.argtypes = [C.POINTER(_ErrorInfoC)]
_lib.sdrn_get_last_global_error_info.restype = C.c_int
_lib.sdrn_list_devices.argtypes = [C.POINTER(_DeviceInfoC), C.c_size_t, C.POINTER(C.c_size_t)]
_lib.sdrn_list_devices.restype = C.c_int
_lib.sdrn_open.argtypes = [C.c_size_t, C.POINTER(C.c_void_p)]
_lib.sdrn_open.restype = C.c_int
_lib.sdrn_close.argtypes = [C.c_void_p]
_lib.sdrn_last_error.argtypes = [C.c_void_p]
_lib.sdrn_last_error.restype = C.c_char_p
_lib.sdrn_get_last_error_info.argtypes = [C.c_void_p, C.POINTER(_ErrorInfoC)]
_lib.sdrn_get_last_error_info.restype = C.c_int
_lib.sdrn_set_log_callback.argtypes = [C.c_void_p, _LOG_CALLBACK, C.c_void_p, C.c_int]
_lib.sdrn_get_device_info.argtypes = [C.c_void_p, C.POINTER(_DeviceInfoC)]
_lib.sdrn_get_device_info.restype = C.c_int
_lib.sdrn_get_capabilities.argtypes = [C.c_void_p, C.POINTER(_CapabilitiesC)]
_lib.sdrn_get_capabilities.restype = C.c_int
_lib.sdrn_get_device_profile.argtypes = [C.c_void_p, C.POINTER(_DeviceProfileC)]
_lib.sdrn_get_device_profile.restype = C.c_int
_lib.sdrn_tuner_family_string.argtypes = [C.c_int]
_lib.sdrn_tuner_family_string.restype = C.c_char_p
_lib.sdrn_demod_family_string.argtypes = [C.c_int]
_lib.sdrn_demod_family_string.restype = C.c_char_p
_lib.sdrn_get_mode.argtypes = [C.c_void_p]
_lib.sdrn_get_mode.restype = C.c_int
_lib.sdrn_get_state.argtypes = [C.c_void_p]
_lib.sdrn_get_state.restype = C.c_int
_lib.sdrn_get_usb_stats.argtypes = [C.c_void_p, C.POINTER(_USBStatsC)]
_lib.sdrn_get_usb_stats.restype = C.c_int
_lib.sdrn_get_usb_recovery_stats.argtypes = [C.c_void_p, C.POINTER(_USBRecoveryStatsC)]
_lib.sdrn_get_usb_recovery_stats.restype = C.c_int
_lib.sdrn_reset_usb_stats.argtypes = [C.c_void_p]
_lib.sdrn_reset_usb_stats.restype = C.c_int
_lib.sdrn_stop.argtypes = [C.c_void_p]
_lib.sdrn_stop.restype = C.c_int
_lib.sdrn_get_stream_info.argtypes = [C.c_void_p, C.POINTER(_StreamInfoC)]
_lib.sdrn_get_stream_info.restype = C.c_int
_lib.sdrn_async_config_init.argtypes = [C.POINTER(_AsyncConfigC)]
_lib.sdrn_async_config_init.restype = None
_lib.sdrn_async_start.argtypes = [C.c_void_p, C.POINTER(_AsyncConfigC)]
_lib.sdrn_async_start.restype = C.c_int
_lib.sdrn_async_read.argtypes = [
    C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t), C.POINTER(C.c_uint64), C.c_uint32
]
_lib.sdrn_async_read.restype = C.c_int
_lib.sdrn_async_stop.argtypes = [C.c_void_p]
_lib.sdrn_async_stop.restype = C.c_int
_lib.sdrn_async_is_running.argtypes = [C.c_void_p]
_lib.sdrn_async_is_running.restype = C.c_int
_lib.sdrn_async_get_stats.argtypes = [C.c_void_p, C.POINTER(_AsyncStatsC)]
_lib.sdrn_async_get_stats.restype = C.c_int
_lib.sdrn_async_reset_stats.argtypes = [C.c_void_p]
_lib.sdrn_async_reset_stats.restype = C.c_int
_lib.sdrn_iq_start.argtypes = [C.c_void_p, C.c_uint64, C.c_uint32, C.c_uint32, C.POINTER(_IQTuneInfoC)]
_lib.sdrn_iq_start.restype = C.c_int
_lib.sdrn_iq_tune.argtypes = [C.c_void_p, C.c_uint64, C.POINTER(_IQTuneInfoC)]
_lib.sdrn_iq_tune.restype = C.c_int
_lib.sdrn_iq_set_sample_rate.argtypes = [C.c_void_p, C.c_uint32, C.POINTER(C.c_uint32)]
_lib.sdrn_iq_set_sample_rate.restype = C.c_int
_lib.sdrn_iq_set_bandwidth.argtypes = [C.c_void_p, C.c_uint32, C.POINTER(_IQTuneInfoC)]
_lib.sdrn_iq_set_bandwidth.restype = C.c_int
_lib.sdrn_dvbt_start.argtypes = [C.c_void_p, C.c_uint64, C.c_uint32, C.POINTER(_TuneInfoC)]
_lib.sdrn_dvbt_start.restype = C.c_int
_lib.sdrn_dvbt_tune.argtypes = [C.c_void_p, C.c_uint64, C.c_uint32, C.POINTER(_TuneInfoC)]
_lib.sdrn_dvbt_tune.restype = C.c_int
_lib.sdrn_dvbt_wait_lock.argtypes = [C.c_void_p, C.c_uint32, C.POINTER(C.c_uint8)]
_lib.sdrn_dvbt_wait_lock.restype = C.c_int
_lib.sdrn_dvbt_get_signal_quality.argtypes = [C.c_void_p, C.POINTER(_SignalQualityC)]
_lib.sdrn_dvbt_get_signal_quality.restype = C.c_int
_lib.sdrn_stream_read.argtypes = [C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t), C.c_uint32]
_lib.sdrn_stream_read.restype = C.c_int
_lib.sdrn_dvbt_set_pid_filter.argtypes = [C.c_void_p, C.POINTER(C.c_uint16), C.c_size_t]
_lib.sdrn_dvbt_set_pid_filter.restype = C.c_int
_lib.sdrn_dvbt_disable_pid_filter.argtypes = [C.c_void_p]
_lib.sdrn_dvbt_disable_pid_filter.restype = C.c_int
_lib.sdrn_control_read.argtypes = [C.c_void_p, C.c_uint16, C.c_uint16, C.c_void_p, C.c_size_t]
_lib.sdrn_control_read.restype = C.c_int
_lib.sdrn_control_write.argtypes = [C.c_void_p, C.c_uint16, C.c_uint16, C.c_void_p, C.c_size_t]
_lib.sdrn_control_write.restype = C.c_int
_lib.sdrn_reg_read.argtypes = [C.c_void_p, C.c_uint16, C.c_void_p, C.c_size_t]
_lib.sdrn_reg_read.restype = C.c_int
_lib.sdrn_reg_write.argtypes = [C.c_void_p, C.c_uint16, C.c_void_p, C.c_size_t]
_lib.sdrn_reg_write.restype = C.c_int
_lib.sdrn_demod_read.argtypes = [C.c_void_p, C.c_uint8, C.c_uint8, C.c_void_p, C.c_size_t]
_lib.sdrn_demod_read.restype = C.c_int
_lib.sdrn_demod_write.argtypes = [C.c_void_p, C.c_uint8, C.c_uint8, C.c_void_p, C.c_size_t]
_lib.sdrn_demod_write.restype = C.c_int
_lib.sdrn_i2c_read_reg.argtypes = [C.c_void_p, C.c_uint8, C.c_uint8, C.c_void_p, C.c_size_t]
_lib.sdrn_i2c_read_reg.restype = C.c_int
_lib.sdrn_i2c_write_reg.argtypes = [C.c_void_p, C.c_uint8, C.c_uint8, C.c_void_p, C.c_size_t]
_lib.sdrn_i2c_write_reg.restype = C.c_int
_lib.sdrn_i2c_read_direct.argtypes = [C.c_void_p, C.c_uint8, C.c_void_p, C.c_size_t]
_lib.sdrn_i2c_read_direct.restype = C.c_int
_lib.sdrn_i2c_write_direct.argtypes = [C.c_void_p, C.c_uint8, C.c_void_p, C.c_size_t]
_lib.sdrn_i2c_write_direct.restype = C.c_int
_lib.sdrn_i2c_gate_open.argtypes = [C.c_void_p]
_lib.sdrn_i2c_gate_open.restype = C.c_int
_lib.sdrn_i2c_gate_close.argtypes = [C.c_void_p]
_lib.sdrn_i2c_gate_close.restype = C.c_int


_ABI_EXPECTED_VERSION = 1


def _read_abi_info_c() -> _ABIInfoC:
    info = _ABIInfoC()
    rc = int(_lib.sdrn_get_abi_info(C.byref(info)))
    if rc != 0:
        raise OSError(f"sdrn_get_abi_info failed with status {rc}")
    return info


def _validate_abi() -> None:
    info = _read_abi_info_c()
    failures: list[str] = []

    native_abi = int(_lib.sdrn_abi_version())
    if native_abi != _ABI_EXPECTED_VERSION:
        failures.append(f"ABI generation {native_abi} != expected {_ABI_EXPECTED_VERSION}")
    if int(info.abi_version) != _ABI_EXPECTED_VERSION:
        failures.append(
            f"ABI info generation {int(info.abi_version)} != expected {_ABI_EXPECTED_VERSION}"
        )
    if int(info.struct_size) != C.sizeof(_ABIInfoC):
        failures.append(
            f"sdrn_abi_info size {int(info.struct_size)} != ctypes {C.sizeof(_ABIInfoC)}"
        )
    if int(info.pointer_size) != C.sizeof(C.c_void_p):
        failures.append(
            f"pointer size {int(info.pointer_size)} != ctypes {C.sizeof(C.c_void_p)}"
        )

    enum_fields = (
        "status_size", "log_level_size", "mode_size", "state_size",
        "error_domain_size", "stream_format_size", "tuner_family_size",
        "demod_family_size",
    )
    enum_size = C.sizeof(C.c_int)
    for field in enum_fields:
        native_size = int(getattr(info, field))
        if native_size != enum_size:
            failures.append(f"{field} {native_size} != ctypes int {enum_size}")

    layouts = (
        ("device_info", _DeviceInfoC),
        ("capabilities", _CapabilitiesC),
        ("error_info", _ErrorInfoC),
        ("frequency_range", _FrequencyRangeC),
        ("rate_range", _RateRangeC),
        ("device_profile", _DeviceProfileC),
        ("dvbt_tune_info", _TuneInfoC),
        ("iq_tune_info", _IQTuneInfoC),
        ("stream_info", _StreamInfoC),
        ("signal_quality", _SignalQualityC),
        ("usb_stats", _USBStatsC),
    )
    for field, ctype in layouts:
        native_size = int(getattr(info, f"{field}_size"))
        native_align = int(getattr(info, f"{field}_align"))
        py_size = C.sizeof(ctype)
        py_align = C.alignment(ctype)
        if native_size != py_size or native_align != py_align:
            failures.append(
                f"{field} layout native={native_size}/{native_align} "
                f"ctypes={py_size}/{py_align}"
            )

    if failures:
        raise OSError("C/Python ABI mismatch: " + "; ".join(failures))


_validate_abi()


def _validate_async_contract() -> None:
    config = _AsyncConfigC()
    _lib.sdrn_async_config_init(C.byref(config))
    failures: list[str] = []
    if int(config.struct_size) != C.sizeof(_AsyncConfigC):
        failures.append(
            f"sdrn_async_config size {int(config.struct_size)} != ctypes {C.sizeof(_AsyncConfigC)}"
        )
    if int(config.config_version) != 1:
        failures.append(f"async config version {int(config.config_version)} != expected 1")
    if int(config.transfer_size_bytes) != ASYNC_DEFAULT_TRANSFER_SIZE:
        failures.append("async default transfer size does not match the public async contract")
    if int(config.transfer_count) != ASYNC_DEFAULT_TRANSFER_COUNT:
        failures.append("async default transfer count does not match the public async contract")
    if int(config.queue_depth) != ASYNC_DEFAULT_QUEUE_DEPTH:
        failures.append("async default queue depth does not match the public async contract")
    if int(config.overflow_policy) != int(AsyncOverflowPolicy.DROP_OLDEST):
        failures.append("async default overflow policy does not match the public async contract")
    if failures:
        raise OSError("C/Python async ABI mismatch: " + "; ".join(failures))


_validate_async_contract()


def version() -> str:
    return _lib.sdrn_version_string().decode()


def abi_version() -> int:
    return int(_lib.sdrn_abi_version())


def abi_info() -> ABIInfo:
    info = _read_abi_info_c()
    return ABIInfo(
        abi_version=int(info.abi_version),
        info_version=int(info.info_version),
        flags=int(info.flags),
        pointer_size=int(info.pointer_size),
        device_profile_size=int(info.device_profile_size),
        error_info_size=int(info.error_info_size),
        usb_stats_size=int(info.usb_stats_size),
    )


def _status_name(status: int) -> str:
    return _lib.sdrn_status_string(status).decode()


def _domain(value: int) -> ErrorDomain:
    try:
        return ErrorDomain(value)
    except ValueError:
        return ErrorDomain.NONE


def _fallback_domain(status: int) -> ErrorDomain:
    if status == -1:
        return ErrorDomain.ARGUMENT
    if status == -9:
        return ErrorDomain.STATE
    if status in (-3, -4, -5, -6):
        return ErrorDomain.USB
    if status == -10:
        return ErrorDomain.TUNER
    if status in (-11, -12):
        return ErrorDomain.DEMOD
    if status in (-2, -13):
        return ErrorDomain.INTERNAL
    return ErrorDomain.NONE


def _read_error_info(handle: C.c_void_p | None, status: int) -> ErrorInfo:
    c = _ErrorInfoC()
    rc = -1
    if handle and handle.value:
        rc = _lib.sdrn_get_last_error_info(handle, C.byref(c))
    else:
        rc = _lib.sdrn_get_last_global_error_info(C.byref(c))

    if rc == 0:
        domain = _domain(int(c.domain))
        return ErrorInfo(
            status=int(c.status) if int(c.status) != 0 else int(status),
            domain=domain if domain != ErrorDomain.NONE else _fallback_domain(status),
            native_code=int(c.native_code),
            flags=ErrorFlags(int(c.flags)),
            message=_text(bytes(c.message)),
        )
    return ErrorInfo(int(status), _fallback_domain(status), 0, ErrorFlags.NONE, "")


_ERROR_TYPES = {
    ErrorDomain.ARGUMENT: SDRNativeArgumentError,
    ErrorDomain.STATE: SDRNativeStateError,
    ErrorDomain.USB: SDRNativeUSBError,
    ErrorDomain.TUNER: SDRNativeTunerError,
    ErrorDomain.DEMOD: SDRNativeDemodError,
    ErrorDomain.STREAM: SDRNativeStreamError,
    ErrorDomain.INTERNAL: SDRNativeInternalError,
}


def _check(status: int, handle: C.c_void_p | None = None) -> None:
    if status == 0:
        return
    info = _read_error_info(handle, status)
    message = _status_name(status)
    if info.message:
        message += f": {info.message}"
    if info.native_code:
        message += f" [native={info.native_code}]"
    exc_type = _ERROR_TYPES.get(info.domain, SDRNativeError)
    raise exc_type(status, message, info)


def last_global_error_info() -> ErrorInfo:
    return _read_error_info(None, 0)


def _info(c: _DeviceInfoC) -> DeviceInfo:
    return DeviceInfo(
        vid=c.vid,
        pid=c.pid,
        bus=c.bus,
        address=c.address,
        manufacturer=_text(bytes(c.manufacturer)),
        product=_text(bytes(c.product)),
        serial=_text(bytes(c.serial)),
    )


def list_devices() -> list[DeviceInfo]:
    count = C.c_size_t()
    _check(_lib.sdrn_list_devices(None, 0, C.byref(count)))
    if not count.value:
        return []
    array = (_DeviceInfoC * count.value)()
    _check(_lib.sdrn_list_devices(array, count.value, C.byref(count)))
    return [_info(array[i]) for i in range(count.value)]


def tuner_family_name(family: TunerFamily | int) -> str:
    return _lib.sdrn_tuner_family_string(int(family)).decode()


def demod_family_name(family: DemodFamily | int) -> str:
    return _lib.sdrn_demod_family_string(int(family)).decode()


def _enum_or_unknown(enum_type, value):
    try:
        return enum_type(value)
    except ValueError:
        return enum_type.UNKNOWN


def _async_config_c(config: AsyncConfig) -> _AsyncConfigC:
    c = _AsyncConfigC()
    _lib.sdrn_async_config_init(C.byref(c))
    c.transfer_size_bytes = int(config.transfer_size_bytes)
    c.transfer_count = int(config.transfer_count)
    c.queue_depth = int(config.queue_depth)
    c.transfer_timeout_ms = int(config.transfer_timeout_ms)
    c.overflow_policy = int(config.overflow_policy)
    return c


def _async_stats(c: _AsyncStatsC) -> AsyncStats:
    if int(c.struct_size) != C.sizeof(_AsyncStatsC) or int(c.stats_version) != 1:
        raise OSError(
            "unsupported async stats layout: "
            f"version={int(c.stats_version)} size={int(c.struct_size)}"
        )
    return AsyncStats(
        stats_version=int(c.stats_version),
        running=bool(c.running),
        queue_current=int(c.queue_current),
        queue_high_water=int(c.queue_high_water),
        transfer_callbacks=int(c.transfer_callbacks),
        transfers_completed=int(c.transfers_completed),
        transfer_bytes=int(c.transfer_bytes),
        transfer_timeouts=int(c.transfer_timeouts),
        transfer_errors=int(c.transfer_errors),
        buffers_enqueued=int(c.buffers_enqueued),
        buffers_dequeued=int(c.buffers_dequeued),
        buffers_dropped_oldest=int(c.buffers_dropped_oldest),
        buffers_dropped_newest=int(c.buffers_dropped_newest),
        consumer_reads=int(c.consumer_reads),
        consumer_bytes=int(c.consumer_bytes),
        buffers_discarded_on_stop=int(c.buffers_discarded_on_stop),
        terminal_status=int(c.terminal_status),
        terminal_native_code=int(c.terminal_native_code),
    )


def _async_timeout(timeout_ms: int | None) -> int:
    if timeout_ms is None:
        return ASYNC_WAIT_INFINITE
    timeout = int(timeout_ms)
    if not 0 <= timeout <= ASYNC_WAIT_INFINITE:
        raise ValueError("timeout_ms must be None or fit uint32")
    return timeout


class Device:
    def __init__(self, handle: C.c_void_p):
        self._handle = handle
        self._log_callback = None

    @classmethod
    def open(cls, index: int = 0) -> "Device":
        handle = C.c_void_p()
        status = _lib.sdrn_open(index, C.byref(handle))
        _check(status)
        return cls(handle)

    @property
    def closed(self) -> bool:
        return not bool(self._handle and self._handle.value)

    def _require(self) -> C.c_void_p:
        if self.closed:
            raise RuntimeError("device is closed")
        return self._handle

    def close(self) -> None:
        if not self.closed:
            _lib.sdrn_close(self._handle)
            self._handle = C.c_void_p()
            self._log_callback = None

    def __enter__(self) -> "Device":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()

    def set_logger(self, callback=None, *, level: int = 3) -> None:
        handle = self._require()
        if callback is None:
            self._log_callback = None
            # ctypes CFUNCTYPE slot cannot receive None
            # use no op callback instead
            callback = lambda lvl, msg: None

        def bridge(_user, lvl, message):
            callback(lvl, message.decode("utf-8", "replace"))

        c_cb = _LOG_CALLBACK(bridge)
        self._log_callback = c_cb
        _lib.sdrn_set_log_callback(handle, c_cb, None, int(level))

    @property
    def info(self) -> DeviceInfo:
        handle = self._require()
        c = _DeviceInfoC()
        _check(_lib.sdrn_get_device_info(handle, C.byref(c)), handle)
        return _info(c)

    @property
    def capabilities(self) -> Capabilities:
        handle = self._require()
        c = _CapabilitiesC()
        _check(_lib.sdrn_get_capabilities(handle, C.byref(c)), handle)
        bands = tuple(mhz for mhz in (6, 7, 8) if c.dvbt_bandwidth_mask & (1 << mhz))
        return Capabilities(c.flags, bands, c.max_pid_filters, c.tuner_xtal_hz, c.demod_xtal_hz)

    @property
    def profile(self) -> DeviceProfile:
        handle = self._require()
        c = _DeviceProfileC()
        _check(_lib.sdrn_get_device_profile(handle, C.byref(c)), handle)
        rf_count = min(int(c.rf_range_count), 4)
        sample_count = min(int(c.iq_sample_rate_range_count), 4)
        bandwidth_count = min(int(c.iq_bandwidth_range_count), 4)
        dvbt_count = min(int(c.dvbt_bandwidth_count), 8)
        return DeviceProfile(
            profile_version=int(c.profile_version),
            feature_flags=int(c.feature_flags),
            tuner_family=_enum_or_unknown(TunerFamily, int(c.tuner_family)),
            demod_family=_enum_or_unknown(DemodFamily, int(c.demod_family)),
            max_pid_filters=int(c.max_pid_filters),
            tuner_xtal_hz=int(c.tuner_xtal_hz),
            demod_xtal_hz=int(c.demod_xtal_hz),
            rf_ranges=tuple(
                FrequencyRange(int(c.rf_ranges[i].min_hz), int(c.rf_ranges[i].max_hz))
                for i in range(rf_count)
            ),
            iq_sample_rate_ranges=tuple(
                RateRange(
                    int(c.iq_sample_rate_ranges[i].min_hz),
                    int(c.iq_sample_rate_ranges[i].max_hz),
                )
                for i in range(sample_count)
            ),
            iq_bandwidth_ranges=tuple(
                RateRange(
                    int(c.iq_bandwidth_ranges[i].min_hz),
                    int(c.iq_bandwidth_ranges[i].max_hz),
                )
                for i in range(bandwidth_count)
            ),
            dvbt_bandwidths_hz=tuple(int(c.dvbt_bandwidth_hz[i]) for i in range(dvbt_count)),
        )

    @property
    def mode(self) -> int:
        return int(_lib.sdrn_get_mode(self._require()))

    @property
    def state(self) -> int:
        return int(_lib.sdrn_get_state(self._require()))

    @property
    def last_error_info(self) -> ErrorInfo:
        return _read_error_info(self._require(), 0)

    @property
    def usb_stats(self) -> USBStats:
        h = self._require()
        c = _USBStatsC()
        _check(_lib.sdrn_get_usb_stats(h, C.byref(c)), h)
        return USBStats(
            control_transfers=c.control_transfers,
            control_errors=c.control_errors,
            control_pipe_events=c.control_pipe_events,
            session_recovery_attempts=c.session_recovery_attempts,
            session_recovery_successes=c.session_recovery_successes,
            session_recovery_failures=c.session_recovery_failures,
            bulk_reads=c.bulk_reads,
            bulk_timeouts=c.bulk_timeouts,
            bulk_errors=c.bulk_errors,
            bulk_bytes=c.bulk_bytes,
        )

    @property
    def usb_recovery_stats(self) -> USBRecoveryStats:
        h = self._require()
        c = _USBRecoveryStatsC()
        _check(_lib.sdrn_get_usb_recovery_stats(h, C.byref(c)), h)
        if int(c.stats_version) != 1 or int(c.struct_size) != C.sizeof(_USBRecoveryStatsC):
            raise OSError(
                "unsupported USB recovery stats layout: "
                f"version={int(c.stats_version)} size={int(c.struct_size)}"
            )
        return USBRecoveryStats(
            stats_version=int(c.stats_version),
            control_pipe_events=int(c.control_pipe_events),
            session_reopen_attempts=int(c.session_reopen_attempts),
            session_reopen_successes=int(c.session_reopen_successes),
            session_reopen_failures=int(c.session_reopen_failures),
            transaction_retry_attempts=int(c.transaction_retry_attempts),
            transaction_retry_successes=int(c.transaction_retry_successes),
            transaction_retry_failures=int(c.transaction_retry_failures),
        )

    def reset_usb_stats(self) -> None:
        h = self._require()
        _check(_lib.sdrn_reset_usb_stats(h), h)

    def stop(self) -> None:
        h = self._require()
        _check(_lib.sdrn_stop(h), h)

    @property
    def stream_info(self) -> StreamInfo:
        h = self._require()
        c = _StreamInfoC()
        _check(_lib.sdrn_get_stream_info(h, C.byref(c)), h)
        return StreamInfo(c.format, c.sample_rate_hz, c.packet_size_bytes, c.bytes_per_complex_sample)

    def async_start(self, config: AsyncConfig | None = None) -> None:
        h = self._require()
        if config is None:
            _check(_lib.sdrn_async_start(h, None), h)
            return
        if not isinstance(config, AsyncConfig):
            raise TypeError("config must be AsyncConfig or None")
        c = _async_config_c(config)
        _check(_lib.sdrn_async_start(h, C.byref(c)), h)

    def async_read(
        self,
        size: int = ASYNC_DEFAULT_TRANSFER_SIZE,
        timeout_ms: int | None = 1000,
    ) -> AsyncBlock | None:
        if size <= 0:
            raise ValueError("size must be positive")
        h = self._require()
        buf = (C.c_uint8 * int(size))()
        got = C.c_size_t()
        sequence = C.c_uint64()
        status = int(
            _lib.sdrn_async_read(
                h, buf, int(size), C.byref(got), C.byref(sequence), _async_timeout(timeout_ms)
            )
        )
        # consumer timeout is a normal empty read
        # do not route it through terminal error state
        if status == -6:
            return None
        _check(status, h)
        return AsyncBlock(bytes(buf[: got.value]), int(sequence.value))

    def async_stop(self) -> None:
        h = self._require()
        _check(_lib.sdrn_async_stop(h), h)

    @property
    def async_running(self) -> bool:
        return bool(_lib.sdrn_async_is_running(self._require()))

    @property
    def async_stats(self) -> AsyncStats:
        h = self._require()
        c = _AsyncStatsC()
        _check(_lib.sdrn_async_get_stats(h, C.byref(c)), h)
        return _async_stats(c)

    def reset_async_stats(self) -> None:
        h = self._require()
        _check(_lib.sdrn_async_reset_stats(h), h)

    def async_stream(
        self,
        config: AsyncConfig | None = None,
        *,
        read_size: int | None = None,
        timeout_ms: int | None = 1000,
    ) -> "AsyncStream":
        if config is not None and not isinstance(config, AsyncConfig):
            raise TypeError("config must be AsyncConfig or None")
        if read_size is None:
            read_size = config.transfer_size_bytes if config is not None else ASYNC_DEFAULT_TRANSFER_SIZE
        return AsyncStream(self, config=config, read_size=read_size, timeout_ms=timeout_ms)

    @staticmethod
    def _iq_tune(c: _IQTuneInfoC) -> IQTuneInfo:
        return IQTuneInfo(
            rf_hz=c.rf_hz,
            requested_sample_rate_hz=c.requested_sample_rate_hz,
            actual_sample_rate_hz=c.actual_sample_rate_hz,
            bandwidth_hz=c.bandwidth_hz,
            if_hz=c.if_hz,
            lo_hz=c.lo_hz,
            tuner_pll_locked=bool(c.tuner_pll_locked),
        )

    def iq_start(
        self,
        frequency_hz: int,
        sample_rate_hz: int = 2_400_000,
        bandwidth_hz: int = 0,
    ) -> IQTuneInfo:
        h = self._require()
        c = _IQTuneInfoC()
        _check(
            _lib.sdrn_iq_start(
                h, int(frequency_hz), int(sample_rate_hz), int(bandwidth_hz), C.byref(c)
            ),
            h,
        )
        return self._iq_tune(c)

    def iq_tune(self, frequency_hz: int) -> IQTuneInfo:
        h = self._require()
        c = _IQTuneInfoC()
        _check(_lib.sdrn_iq_tune(h, int(frequency_hz), C.byref(c)), h)
        return self._iq_tune(c)

    def iq_set_sample_rate(self, sample_rate_hz: int) -> int:
        h = self._require()
        actual = C.c_uint32()
        _check(_lib.sdrn_iq_set_sample_rate(h, int(sample_rate_hz), C.byref(actual)), h)
        return int(actual.value)

    def iq_set_bandwidth(self, bandwidth_hz: int) -> IQTuneInfo:
        h = self._require()
        c = _IQTuneInfoC()
        _check(_lib.sdrn_iq_set_bandwidth(h, int(bandwidth_hz), C.byref(c)), h)
        return self._iq_tune(c)

    @staticmethod
    def _tune(c: _TuneInfoC) -> DVBTTuneInfo:
        return DVBTTuneInfo(
            rf_hz=c.rf_hz,
            bandwidth_hz=c.bandwidth_hz,
            if_hz=c.if_hz,
            lo_hz=c.lo_hz,
            tuner_pll_locked=bool(c.tuner_pll_locked),
            filter_cal_code=c.filter_cal_code,
        )

    def dvbt_start(self, frequency_hz: int, bandwidth_hz: int) -> DVBTTuneInfo:
        h = self._require()
        c = _TuneInfoC()
        _check(_lib.sdrn_dvbt_start(h, int(frequency_hz), int(bandwidth_hz), C.byref(c)), h)
        return self._tune(c)

    def dvbt_tune(self, frequency_hz: int, bandwidth_hz: int) -> DVBTTuneInfo:
        h = self._require()
        c = _TuneInfoC()
        _check(_lib.sdrn_dvbt_tune(h, int(frequency_hz), int(bandwidth_hz), C.byref(c)), h)
        return self._tune(c)

    def wait_lock(self, timeout_ms: int = 4000) -> int:
        h = self._require()
        stage = C.c_uint8()
        _check(_lib.sdrn_dvbt_wait_lock(h, timeout_ms, C.byref(stage)), h)
        return int(stage.value)

    def signal_quality(self) -> SignalQuality:
        h = self._require()
        c = _SignalQualityC()
        _check(_lib.sdrn_dvbt_get_signal_quality(h, C.byref(c)), h)
        return SignalQuality(
            fsm_stage=c.fsm_stage,
            strength_percent=c.strength_percent,
            snr_db=c.snr_db if c.snr_valid else None,
            ber=c.ber if c.ber_valid else None,
            ber_raw=c.ber,
        )

    def read(self, size: int = 256 * 1024, timeout_ms: int = 1000) -> bytes:
        if size <= 0:
            raise ValueError("size must be positive")
        h = self._require()
        buf = (C.c_uint8 * size)()
        got = C.c_size_t()
        _check(_lib.sdrn_stream_read(h, buf, size, C.byref(got), timeout_ms), h)
        return bytes(buf[: got.value])

    def blocks(self, size: int = 256 * 1024, timeout_ms: int = 1000) -> Iterator[bytes]:
        while not self.closed:
            block = self.read(size=size, timeout_ms=timeout_ms)
            if block:
                yield block

    def set_pid_filter(self, pids: Iterable[int]) -> None:
        values = [int(p) for p in pids]
        if not values:
            raise ValueError("at least one PID is required")
        array = (C.c_uint16 * len(values))(*values)
        h = self._require()
        _check(_lib.sdrn_dvbt_set_pid_filter(h, array, len(values)), h)

    def disable_pid_filter(self) -> None:
        h = self._require()
        _check(_lib.sdrn_dvbt_disable_pid_filter(h), h)

    # low level hardware access only
    def control_read(self, value: int, index: int, length: int) -> bytes:
        if length <= 0:
            raise ValueError("length must be positive")
        h = self._require()
        buf = (C.c_uint8 * length)()
        _check(_lib.sdrn_control_read(h, value, index, buf, length), h)
        return bytes(buf)

    def control_write(self, value: int, index: int, data: bytes) -> None:
        raw = bytes(data)
        if not raw:
            raise ValueError("data must not be empty")
        h = self._require()
        buf = (C.c_uint8 * len(raw)).from_buffer_copy(raw)
        _check(_lib.sdrn_control_write(h, value, index, buf, len(raw)), h)

    def reg_read(self, reg: int, length: int = 1) -> bytes:
        h = self._require()
        buf = (C.c_uint8 * length)()
        _check(_lib.sdrn_reg_read(h, reg, buf, length), h)
        return bytes(buf)

    def reg_write(self, reg: int, data: bytes) -> None:
        h = self._require()
        raw = bytes(data)
        buf = (C.c_uint8 * len(raw)).from_buffer_copy(raw)
        _check(_lib.sdrn_reg_write(h, reg, buf, len(raw)), h)

    def demod_read(self, page: int, reg: int, length: int = 1) -> bytes:
        h = self._require()
        buf = (C.c_uint8 * length)()
        _check(_lib.sdrn_demod_read(h, page, reg, buf, length), h)
        return bytes(buf)

    def demod_write(self, page: int, reg: int, data: bytes) -> None:
        h = self._require()
        raw = bytes(data)
        buf = (C.c_uint8 * len(raw)).from_buffer_copy(raw)
        _check(_lib.sdrn_demod_write(h, page, reg, buf, len(raw)), h)

    def i2c_read_reg(self, address: int, reg: int, length: int = 1) -> bytes:
        h = self._require()
        buf = (C.c_uint8 * length)()
        _check(_lib.sdrn_i2c_read_reg(h, address, reg, buf, length), h)
        return bytes(buf)

    def i2c_write_reg(self, address: int, reg: int, data: bytes) -> None:
        h = self._require()
        raw = bytes(data)
        buf = (C.c_uint8 * len(raw)).from_buffer_copy(raw)
        _check(_lib.sdrn_i2c_write_reg(h, address, reg, buf, len(raw)), h)

    def i2c_read_direct(self, address: int, length: int = 1) -> bytes:
        if length <= 0:
            raise ValueError("length must be positive")
        h = self._require()
        buf = (C.c_uint8 * length)()
        _check(_lib.sdrn_i2c_read_direct(h, address, buf, length), h)
        return bytes(buf)

    def i2c_write_direct(self, address: int, data: bytes) -> None:
        raw = bytes(data)
        if not raw:
            raise ValueError("data must not be empty")
        h = self._require()
        buf = (C.c_uint8 * len(raw)).from_buffer_copy(raw)
        _check(_lib.sdrn_i2c_write_direct(h, address, buf, len(raw)), h)

    def i2c_gate(self, open_: bool) -> None:
        h = self._require()
        fn = _lib.sdrn_i2c_gate_open if open_ else _lib.sdrn_i2c_gate_close
        _check(fn(h), h)


class AsyncStream:
    # single consumer iterator over native async queue

    def __init__(
        self,
        device: Device,
        *,
        config: AsyncConfig | None = None,
        read_size: int = ASYNC_DEFAULT_TRANSFER_SIZE,
        timeout_ms: int | None = 1000,
    ):
        if read_size <= 0:
            raise ValueError("read_size must be positive")
        _async_timeout(timeout_ms)
        self.device = device
        self.config = config
        self.read_size = int(read_size)
        self.timeout_ms = timeout_ms

    @property
    def running(self) -> bool:
        return False if self.device.closed else self.device.async_running

    @property
    def stats(self) -> AsyncStats:
        return self.device.async_stats

    def start(self) -> "AsyncStream":
        # native state machine owns active stream validation
        self.device.async_start(self.config)
        return self

    def stop(self) -> None:
        if not self.device.closed:
            # terminal producer error stops running but still owns queue.
            self.device.async_stop()

    def read(self, *, size: int | None = None, timeout_ms: int | None = None) -> AsyncBlock | None:
        if size is None:
            size = self.read_size
        if timeout_ms is None:
            timeout_ms = self.timeout_ms
        return self.device.async_read(size=size, timeout_ms=timeout_ms)

    def read_many(self, count: int) -> list[AsyncBlock]:
        # read count non timeout blocks using stream defaults
        if count < 0:
            raise ValueError("count must be non-negative")
        return [next(self) for _ in range(count)]

    def blocks(self) -> Iterator[AsyncBlock]:
        # return this stream's single consumer iterator
        return iter(self)

    def __iter__(self) -> "AsyncStream":
        return self

    def __next__(self) -> AsyncBlock:
        # stopped producer may still have queued data or a terminal error
        while not self.device.closed:
            try:
                block = self.device.async_read(size=self.read_size, timeout_ms=self.timeout_ms)
            except SDRNativeStateError:
                # stop from another thread wakes blocked consumer
                if self.device.closed or not self.device.async_running:
                    raise StopIteration from None
                raise
            if block is not None:
                return block
        raise StopIteration

    def __enter__(self) -> "AsyncStream":
        return self.start()

    def __exit__(self, exc_type, exc, tb) -> None:
        if exc_type is None:
            self.stop()
            return
        # keep original exception when teardown also fails
        try:
            self.stop()
        except SDRNativeError:
            pass