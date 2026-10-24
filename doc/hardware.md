# Hardware

Target: RTL2832U receivers with R820T/R820T2/R860-family tuners.

IQ output is unsigned interleaved 8-bit I/Q. Accepted sample rates are
225,001–300,000 or 900,001–3,200,000 samples/s. Tuner bandwidth:
225,000–8,000,000 Hz. R820T-family frequency limits: 24–1,766 MHz.
These are API limits, not guarantees of RF performance. Use the reported
actual sample rate rather than assuming the requested rate was selected.

DVB-T uses the RTL2832U demodulator, with 6, 7 or 8 MHz bandwidth and
188-byte MPEG transport-stream packets. The hardware PID filter accepts up
to 32 PIDs. Reception and lock depend on the local signal and antenna.
DVB-T2 and service/media decoding are not supported.