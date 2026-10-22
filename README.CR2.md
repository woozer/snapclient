# Speaker C-R2 ESP32-S3 Snapclient port

This fork preserves CarlosDerSeher/snapclient history at
`f13a7e437824ab7ef7bd4bfe482fcfeea8e74dbd`. The upstream README and licence remain.

Branches:

- `cr2/reference-100ms`: tested `c-r2-play-0.2.20-wait`, FLAC / 100 ms target,
  CamillaDSP 512 frames, DMA 2×1023, bounded DMA-aware queue wait (20 ms cap,
  5 ms reserve). Short silent tests passed; not a production qualification.
- `cr2/low-latency-experiments`: startup-deadline correction, geometry-aware DMA
  diagnostics, repeated waits and small-DMA experiments. **50 ms is not qualified.**
- `cr2/best-ap-100ms`: the reference audio implementation plus explicit
  all-channel / strongest-signal AP selection for networks sharing an SSID.

This is a custom C-R2 board port: ESP32-S3, 8 MB flash, octal PSRAM, TAS5825M,
BQ25798 charger, GPIO17 BCLK / GPIO15 LRCLK / GPIO16 DATA, no MCLK. It is not a
binary for an arbitrary Sonocotta board. Includes board power/charge/OTA guards,
HTTP controls, timing traces, receive/DMA diagnostics and unchanged-sample writes
through internal RAM. Wi-Fi credentials are provisioned locally, not in Git.

The two outputs drive separate woofer/tweeter channels. The current prototype
relies on protective host-side CamillaDSP crossover/high-pass filters. This
export does not implement a qualified onboard crossover. Keep charging and
amplifier output disarmed until your board/source/driver configuration is checked.

## Build

ESP-IDF **5.5.1**, image digest
`espressif/idf:v5.5.1@sha256:dfa2d076c796769c07c155eba6c672b9f395aec943b2ba3701b73379b5f9e884`.

```sh
git submodule update --init --recursive
python3 tools/prepare_cr2.py
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
```

The default Snapserver host is `snapserver.local`; configure the actual host in
menuconfig. The bootstrap generates per-installation random OTA and optional
music-service keys in ignored `*_secret.h` files; it never prints their values.
Preserve your existing keys locally for upgrades to an already deployed board.
The optional HTTP music service must use the same music key and run on port1781.
Neither that service nor any Spotify credentials are bundled in this client fork.

Bootstrap also applies `cr2/patches/udp_logger.CMakeLists.txt` to the pinned
logger submodule: disabled UDP logging registers an empty component. This is
an explicit local submodule edit, not a fabricated upstream submodule commit.
Upstream submodule revisions are retained. `cr2/source-export.json` records
source parity and the sanitized host substitution. Private headers, binaries,
raw PCAPs and provisioning data are excluded.

The HTTP controls are intended for a trusted local network. OTA requires a
local token and the existing power/disarmed checks; the ordinary control API
is not an authenticated Internet-facing service. This is bench firmware.

## Timing interpretation

A Snapcast buffer setting is a timestamp-to-playout target, not measured
end-to-end lip-sync. Software queue and DMA capacities overlap that target;
do not sum their maximum capacities as independent delays. I2S API timestamps
are not measured physical LRCLK or acoustic output. Keep the working reference
for comparison and read `cr2/50ms-feasibility.md` on the experimental branch.

## Experimental branch verification

This branch currently exports `c-r2-play-0.2.29-ap480`, DMA 2×480 with
strongest-AP selection. Run `python3 cr2/tests/run.py` for source-matched host
tests with AddressSanitizer/UndefinedBehaviorSanitizer. The original working
100 ms branch is preserved separately. See `cr2/50ms-feasibility.md` and its
sanitized measurements: five resyncs in the latest one-minute 50 ms trial.
The deployed board was restored to `cr2/best-ap-100ms`.
