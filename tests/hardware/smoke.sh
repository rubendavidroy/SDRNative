#!/usr/bin/env bash
# SDRNative
# Hardware smoke test
#
# Copyright (c) 2026 Ruben Roy
# SPDX-License-Identifier: MPL-2.0
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

FREQ_MHZ="${1:-557.5}"
BW_MHZ="${2:-7}"
SECONDS="${3:-6}"
OUT="${4:-sdrnative-smoke.ts}"

if [[ ! -x build/sdrn_probe || ! -x build/sdrn_dvbt_capture ]]; then
  echo "error: build the project first (for macOS: ./scripts/build_macos.sh)" >&2
  exit 2
fi

echo "device probe"
./build/sdrn_probe

echo
echo "DVB-T capture: ${FREQ_MHZ} MHz, ${BW_MHZ} MHz BW"
rm -f "$OUT"
./build/sdrn_dvbt_capture "$FREQ_MHZ" "$BW_MHZ" "$SECONDS" "$OUT"

echo
echo "TS framing"
python3 - "$OUT" <<'PYTEST'
from pathlib import Path
import sys
p = Path(sys.argv[1])
data = p.read_bytes()
print(f"capture: {p} ({len(data):,} bytes)")
if len(data) < 188 * 20:
    raise SystemExit("capture too small for TS sync check")

best = None
for offset in range(188):
    checks = min(250, (len(data) - offset) // 188)
    if checks <= 0:
        continue
    hits = sum(data[offset + i * 188] == 0x47 for i in range(checks))
    score = hits / checks
    if best is None or score > best[0]:
        best = (score, offset, hits, checks)

score, offset, hits, checks = best
print(f"best 188-byte sync: offset={offset}, hits={hits}/{checks} ({score:.1%})")
if hits < 20 or score < 0.90:
    raise SystemExit("no stable 188-byte TS sync")
print("TS framing: pass")
PYTEST

echo
echo "result: pass"