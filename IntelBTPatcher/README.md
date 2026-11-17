# IntelBTPatcher

A Lilu plugin that lets macOS start Bluetooth LE advertising, which AirDrop needs, on an Intel Bluetooth controller.

**Lineage.** The HCI hook and the fixed random address are IntelBTPatcher's
(OpenIntelWireless/IntelBluetoothFirmware, GPL-3.0). The event pipe, the bluetoothd page
patches and the scan-park logic came from ForbiddenItlwm's `BTLEAdvFixup` 0.9.1, which this
kext replaced.

- **On by default.** `-ibtpoff` turns it off. Opt-in extras: `-ibtppark` (park scanning and
  advertising around LE Create Connection), `-ibtpwiden` (widen the initiating scan window),
  `-ibtpprivacy`, plus `-ibtpdbg` / `-ibtpbeta`.
- **What it changes:** it patches `/usr/sbin/bluetoothd` in memory as its pages are validated,
  so `bluetoothd` takes its "extended advertising supported" path. It also adjusts some
  outgoing HCI commands, such as the random address of each advertising set, and does
  IntelBTPatcher's fix-up (a fixed `LE Set Random Address` = `94 50 64 D0 78 6B`) before the
  first legacy scan. Nothing on disk changes.
- **One build per macOS version:** the Tahoe build's patterns match macOS 26.7.1 (25G241),
  26.7.3 (25G342), 26.7.2 (25G332) and 26.7.2's own extended-connection site; the Sequoia
  build (`--target sequoia`) matches 15.8.3 (24H241), 15.8 (24H23) and 15.8.2 (24H232); the
  Sonoma build (`--target sonoma`) matches 14.8.9 (23J631). On another build they don't
  match, and nothing is patched.
- **Before a macOS update, boot with `-ibtpoff`.** While the kext is active, reading
  `bluetoothd` returns the patched pages, and an incremental update that checks the file can
  fail.
- **Do not load the original IntelBTPatcher.kext beside this one.** Same bundle id
  (`com.zxystd.IntelBTPatcher`) and the same `IOUSBHostDevice::deviceRequest` hook: Lilu
  panics when two plugins hook one function. This kext already does that job.

Build it with `tools/build_intelbtpatcher.py` (see the repository README).

## License

GPL-3.0-only. The HCI hook follows IntelBTPatcher (OpenIntelWireless/IntelBluetoothFirmware,
GPL-3.0), and the page hook follows BlueToolFixup (acidanthera/BrcmPatchRAM). The full text
is in [LICENSE](LICENSE).
