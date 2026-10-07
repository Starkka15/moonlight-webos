# Moonlight WebOS

Moonlight game streaming client for HP TouchPad (webOS 3.0.5).

Streams games from your NVIDIA GameStream-compatible PC to your HP TouchPad.

## Building

Requires:

- The webOS PDK with its ARM toolchain (Sourcery G++ Lite 2011.03-41, GCC 4.5.2).
  Default location `/opt/PalmPDK`.
- A recursive checkout of [moonlight-embedded](https://github.com/moonlight-stream/moonlight-embedded)
  (for `libgamestream`, `moonlight-common-c` and `h264bitstream`).
  Default location `deps/moonlight-embedded`.
- A sysroot of static libraries cross-built with the same toolchain.
  Default location `sysroot/`. The 0.1.0 release was built against:
  - FFmpeg 4.4.4 (libavcodec, libavformat, libavutil)
  - curl 7.88.1
  - OpenSSL 1.1.1w
  - expat 2.5.0
  - opus 1.4

```bash
./build-deps.sh # fetches moonlight-embedded and cross-builds the sysroot
make            # or: make PDK=... SYSROOT=... MOONLIGHT_EMBEDDED=...
make package    # builds the .ipk (needs palm-package)
```

The headers in `include/` are patched copies of moonlight-common-c headers and
take precedence over the ones in the checkout.

## License

GPL-3.0, the same as moonlight-embedded. See [LICENSE](LICENSE).
