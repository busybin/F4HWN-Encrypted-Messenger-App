# NOTICE — SECMSG V24

This product includes work derived from the projects below.

## 1. F4HWN firmware overlay platform (Apache-2.0)

`app_api.h` (the overlay App ABI header, including the API-level-2 `uid_get()` addition of
the 8K overlay fork), `pack_app.py` (the FAP1 packer) and the base `app.ld` linker script in
this repository are derived from the F4HWN custom firmware:

- https://github.com/armel/uv-k1-k5v3-firmware-custom (Apache-2.0)
- https://github.com/armel/uv-k5-firmware-custom (Apache-2.0)
- https://github.com/egzumer/uv-k5-firmware-custom
- https://github.com/DualTachyon/uv-k5-firmware

The App targets the **8 KiB overlay window** fork published at
https://github.com/busybin/uv-k1-k5v3-firmware-f4hwn-8k (Apache-2.0) and calls `uid_get()`
at struct offset 276 to derive the device identity, so it requires that firmware (or any
firmware providing API level 2 with `uid_get` at that offset).

## 2. The App itself

The App (protocol, FSK modem configuration, crypto core, UI) is an original work by
busybin, published here under Apache-2.0 (see [LICENSE](LICENSE)). It incorporates no
third-party source code beyond the overlay platform listed above.

## 3. Third-party tooling

- `arm-none-eabi-gcc` (GPL with GCC Runtime Library Exception) — build only, not distributed.
- The host test suite uses only the C standard library.

## 4. Build tooling derived from upstream

`build.sh` and `pack_app.py` follow the layout of the upstream App build scripts
(Apache-2.0); they were adapted for a standalone repository (no `/home` paths, toolchain
locatable via `TC` or `PATH`).
