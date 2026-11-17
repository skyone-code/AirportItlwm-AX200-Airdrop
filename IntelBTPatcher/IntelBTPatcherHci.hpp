// Decisions on outgoing HCI commands, kept free of kernel types for host tests.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
namespace Ibtp {
enum : uint16_t {
    kOpReset = 0x0c03, kOpLeEventMask = 0x2001, kOpLeRandomAddress = 0x2005,
    kOpSetRandomAddress = 0x2035, kOpExtAdvParams = 0x2036,
    kOpRemoveSet = 0x203c, kOpClearSets = 0x203d,
    kOpLeScanEnable = 0x200c, kOpLeCreateConn = 0x200d, kOpLeCreateConnCancel = 0x200e,
    // Opcodes per Linux's include/net/bluetooth/hci.h; the extended create was previously
    // mislabeled 0x203a here, which made every park miss — bluetoothd only sends the extended
    // form on BT 5 controllers.
    kOpLeExtCreateConn = 0x2043, kOpLeExtScanEnable = 0x2042,
    kOpLeAdvEnable = 0x200a, kOpLeExtAdvEnable = 0x2039,
    // Advertising payloads: legacy carries the data straight after the command length, the
    // extended form adds set handle, fragment operation and preference first. Both matter for
    // the Continuity witness — the Mac's 0x0D "Wi-Fi Settings" frame is the whole reason an
    // iPhone offers its hotspot at all.
    kOpLeAdvData = 0x2008, kOpLeExtAdvData = 0x2037,
    kOpLeAdvParams = 0x2006, kOpLeAcceptListSize = 0x200f,
    kOpLeAcceptListClear = 0x2010, kOpLeAcceptListAdd = 0x2011, kOpLeAcceptListRemove = 0x2012,
    kOpLeConnUpdate = 0x2013, kOpLeResolvingAdd = 0x2027, kOpLeResolvingRemove = 0x2028,
    kOpLeResolvingClear = 0x2029, kOpLeResolvingSize = 0x202d, kOpLeExtScanParams = 0x2038,
};
// Fingerprint of the connection path the host actually uses. The hot-spot failure comes down to
// which mechanism bluetoothd picks to reach the phone, and the three candidates look identical
// from the outside:
//   - the phone's address is handed to the controller outright (filter policy 0),
//   - the phone is put on the filter accept list and the controller matches it by address,
//   - the phone is put on the resolving list and the controller matches its resolvable private
//     address, which is the only one that works when the phone rotates its address every minute.
// Each costs a reboot to distinguish, so one bitmask is collected and read once.
enum : uint32_t {
    kFpLegacyCreate = 1U << 0, kFpExtCreate = 1U << 1,
    kFpAcceptListClear = 1U << 2, kFpAcceptListAdd = 1U << 3, kFpAcceptListRemove = 1U << 4,
    kFpResolvingAdd = 1U << 5, kFpResolvingClear = 1U << 6, kFpResolvingSize = 1U << 7,
    kFpAcceptListSize = 1U << 8, kFpExtAdvParams = 1U << 9, kFpExtAdvData = 1U << 10,
    kFpExtAdvEnable = 1U << 11, kFpAdvParams = 1U << 12, kFpAdvEnable = 1U << 13,
    kFpScanEnable = 1U << 14, kFpExtScanEnable = 1U << 15, kFpRandomAddress = 1U << 16,
    kFpAdvSetRandomAddress = 1U << 17, kFpReadRemoteFeatures = 1U << 18,
    kFpConnUpdate = 1U << 19, kFpCreateCancel = 1U << 20, kFpResolvingRemove = 1U << 21,
    kFpExtScanParams = 1U << 22, kFpScanParams = 1U << 23,
};
// Returns the bit for a command worth fingerprinting, or 0 for everything else.
inline uint32_t opFingerprint(uint16_t op) {
    switch (op) {
    case kOpLeCreateConn: return kFpLegacyCreate;
    case kOpLeExtCreateConn: return kFpExtCreate;
    case kOpLeAcceptListClear: return kFpAcceptListClear;
    case kOpLeAcceptListAdd: return kFpAcceptListAdd;
    case kOpLeAcceptListRemove: return kFpAcceptListRemove;
    case kOpLeResolvingAdd: return kFpResolvingAdd;
    case kOpLeResolvingRemove: return kFpResolvingRemove;
    case kOpLeResolvingClear: return kFpResolvingClear;
    case kOpLeResolvingSize: return kFpResolvingSize;
    case kOpLeAcceptListSize: return kFpAcceptListSize;
    case kOpExtAdvParams: return kFpExtAdvParams;
    case kOpLeExtAdvData: return kFpExtAdvData;
    case kOpLeExtAdvEnable: return kFpExtAdvEnable;
    case kOpLeAdvParams: return kFpAdvParams;
    case kOpLeAdvEnable: return kFpAdvEnable;
    case kOpLeScanEnable: return kFpScanEnable;
    case kOpLeExtScanEnable: return kFpExtScanEnable;
    case kOpLeRandomAddress: return kFpRandomAddress;
    case kOpSetRandomAddress: return kFpAdvSetRandomAddress;
    case kOpLeConnUpdate: return kFpConnUpdate;
    case kOpLeCreateConnCancel: return kFpCreateCancel;
    case 0x200b: return kFpScanParams;             // kOpLeScanParams, declared further down
    case kOpLeExtScanParams: return kFpExtScanParams;
    case 0x2016: return kFpReadRemoteFeatures;   // kOpLeReadRemoteFeatures, declared further down
    default: return 0;
    }
}
// The original IntelBTPatcher (OpenIntelWireless/IntelBluetoothFirmware) sends this fixed
// address before the first legacy scan, to work around a bluetoothd bug. Lilu panics if two
// plugins hook the same function, so this kext took that job over and the original is not
// loaded beside it. The address must never become an advertising address, because that would
// make the Mac trackable.
static const uint8_t kFixedPatcherAddress[6] = {0x94,0x50,0x64,0xd0,0x78,0x6b};
// address is the general fallback: bluetoothd's LE random address, or until that arrives, the
// last set address seen. setAddress is what bluetoothd chose for each set. scanEnable/
// scanFilterDup mirror the last LE Set Scan Enable the host commanded.
struct State {
    uint8_t address[6]; bool haveAddress; bool haveGeneralAddress;
    uint32_t addressedSets; bool legacyAddressSent;
    uint8_t setAddress[32][6]; uint32_t knownSetAddresses;
    uint8_t scanEnable, scanFilterDup;          // legacy LE Set Scan Enable (0x200c)
    uint8_t extScanEnable, extScanFilterDup;    // extended LE Set Extended Scan Enable (0x2042)
    bool legacyAdvEnabled;                      // legacy LE Set Advertising Enable (0x200a)
    bool extAdvEnabled;                         // extended LE Set Extended Advertising Enable (0x2039)
};
enum : uint16_t { kOpLeScanParams = 0x200b };
// Written on the USB workloop when an attempt-ending event arrives, read on the caller's
// thread when the next command goes out. Races are benign: a missed resume only leaves
// scanning/advertising parked until bluetoothd's next round enables them itself.
struct ScanResumeState {
    bool suspended;        // a create attempt is parked the scanning/advertising we switched off
    bool resumePending;    // the attempt ended; restore with the next command
    bool watchdogFired;    // the restore was forced by the stall counter, not by an event
    uint16_t pendingOpcode;  // the create opcode whose attempt is parked
    uint32_t stallGuard;   // host commands seen since the park, for the watchdog
    uint8_t enable, filterDup;        // legacy scan state to restore
    uint8_t extEnable, extFilterDup;  // extended scan state to restore
    bool parkedLegacyScan, parkedExtScan;    // what this park actually switched off
    bool parkedLegacyAdv, parkedExtAdv;
    // The last extended-advertising-enable packet, captured while mirroring the host and
    // replayed verbatim to restore the sets after a parked create.
    uint8_t extAdvReplay[72]; size_t extAdvReplayLength;
};
// An LE Create Connection while scanning never forms on this controller (also the documented
// Intel bug behind noble issue #165): the pending create is cancelled and bluetoothd times the
// request out — that is the Instant Hotspot credential link (rapportd, Central role, 6 s fast
// scan rounds then failure). bluetoothd on BT 5 controllers scans through the extended command
// (0x2042), not the legacy one (0x200c), and connects through the extended create (0x2043);
// both scan forms are parked with synthetic disables before a create goes out, and resumed
// once the attempt ends (see ScanResumeState). The same treatment now applies to the
// host's advertising sets: with host scanning parked and the create still failing on this
// controller, the remaining concurrency is sharingd's always-on advertising (AirDrop
// discovery) versus the create's own initiating scan — the parking here removes it too.
// Returns how many pre-commands (0..4) must go out before the real one.
//
// allowPark=false keeps this helper as a pure mirror (it still records every host scan/
// advertising enable, it just never synthesises a disable). Parking is OFF by default since
// 2.7.9: it was never observed to make a create succeed, while a field test showed it does
// kill Instant Hotspot outright — the phone can only offer the password share if it can see
// the Mac's advertising, and the park switched that advertising off (the Wi-Fi menu's
// same-Apple-ID hotspot entry vanished with it).
struct PreCommand { uint8_t data[72]; size_t length; };
inline int wantsPreScanOff(State &s, ScanResumeState &r, const uint8_t *hci, size_t length,
                           PreCommand *out, bool allowPark = true) {
    if (!hci || length < 3)
        return 0;
    const uint16_t op = uint16_t(hci[0] | hci[1] << 8);
    if (op == kOpLeScanEnable) {
        // The host re-asserted control over legacy scanning; forget any park we made.
        if (length >= 5) {
            s.scanFilterDup = hci[3];
            s.scanEnable = hci[4];
            if (r.suspended && s.scanEnable)
                r.parkedLegacyScan = false;
        }
        return 0;
    }
    if (op == kOpLeExtScanEnable) {
        // Same for extended scanning. Params: enable, filter policy (2 octets when disabling).
        if (length >= 5) {
            s.extScanFilterDup = hci[4];
            s.extScanEnable = hci[3];
            if (r.suspended && s.extScanEnable)
                r.parkedExtScan = false;
        }
        return 0;
    }
    if (op == kOpLeAdvEnable) {
        if (length >= 4) {
            s.legacyAdvEnabled = hci[3] != 0;
            if (r.suspended && s.legacyAdvEnabled)
                r.parkedLegacyAdv = false;
        }
        return 0;
    }
    if (op == kOpLeExtAdvEnable) {
        if (length >= 4) {
            s.extAdvEnabled = hci[3] != 0;
            if (s.extAdvEnabled && length <= sizeof(r.extAdvReplay)) {
                for (size_t i = 0; i < length; ++i)
                    r.extAdvReplay[i] = hci[i];
                r.extAdvReplayLength = length;
            }
            if (r.suspended && s.extAdvEnabled)
                r.parkedExtAdv = false;
        }
        return 0;
    }
    if (op != kOpLeCreateConn && op != kOpLeExtCreateConn)
        return 0;
    if (!allowPark)
        return 0;
    r.pendingOpcode = op;
    r.stallGuard = 0;
    r.watchdogFired = false;
    int n = 0;
    if (s.scanEnable) {
        r.enable = s.scanEnable; r.filterDup = s.scanFilterDup;
        s.scanEnable = 0;
        PreCommand &c = out[n++];
        c.data[0] = kOpLeScanEnable & 0xff; c.data[1] = kOpLeScanEnable >> 8; c.data[2] = 2;
        c.data[3] = 0; c.data[4] = 0;  // filter duplicates off, scanning off
        c.length = 5;
        r.parkedLegacyScan = true;
    }
    if (s.extScanEnable) {
        r.extEnable = s.extScanEnable; r.extFilterDup = s.extScanFilterDup;
        s.extScanEnable = 0;
        PreCommand &c = out[n++];
        c.data[0] = kOpLeExtScanEnable & 0xff; c.data[1] = kOpLeExtScanEnable >> 8; c.data[2] = 2;
        c.data[3] = 0; c.data[4] = 0;  // enable off, filter policy default (ignored when off)
        c.length = 5;
        r.parkedExtScan = true;
    }
    if (s.legacyAdvEnabled) {
        s.legacyAdvEnabled = 0;
        PreCommand &c = out[n++];
        c.data[0] = kOpLeAdvEnable & 0xff; c.data[1] = kOpLeAdvEnable >> 8; c.data[2] = 1;
        c.data[3] = 0;  // advertising off
        c.length = 4;
        r.parkedLegacyAdv = true;
    }
    if (s.extAdvEnabled) {
        s.extAdvEnabled = 0;
        PreCommand &c = out[n++];
        c.data[0] = kOpLeExtAdvEnable & 0xff; c.data[1] = kOpLeExtAdvEnable >> 8; c.data[2] = 2;
        c.data[3] = 0; c.data[4] = 0;  // enable off, zero sets
        c.length = 5;
        r.parkedExtAdv = true;
    }
    if (n)
        r.suspended = true;
    return n;
}
// LE meta subevents that end a connection attempt (success or error status), and a plain
// Disconnection Complete (event 0x05) for the legacy path. The LE meta event code (0x3e) is
// also spelled here; the DataLength section below declares its own copy.
// The command-level events close the loop the LE-meta events leave open: when the controller
// eats the create (noble #165), it may answer Command Status (0x0f) with an error, or swallow
// the attempt entirely — bluetoothd then cancels with LE Create Connection Cancel (0x200e)
// after its 6 s timeout, and the cancel's Command Status/Complete arrive without any LE-meta
// event. Without those, the parked scanning/advertising stayed off for good, which is what
// made the Instant Hotspot password share spin forever until another join attempt kicked
// bluetoothd into re-asserting the enables itself.
enum : uint8_t { kScanEvtLeMeta = 0x3e, kScanEvtLeMetaConnComplete = 0x01, kScanEvtDisconnComplete = 0x05,
                 kScanEvtLeMetaEnhancedConnComplete = 0x0a, kScanEvtLeMetaEnhancedConnCompleteV2 = 0x0b,
                 kScanEvtCmdStatus = 0x0f, kScanEvtCmdComplete = 0x0e };
// A parked attempt that outlives this many subsequent host commands without any ending event
// is force-resumed. bluetoothd's own cancel path normally restores first (~6 s); this is the
// belt-and-suspenders for an attempt whose every event the controller dropped.
constexpr uint32_t kResumeStallCommands = 128;
inline void onScanResumeEvent(ScanResumeState &s, const uint8_t *event, size_t length) {
    if (!event || length < 3 || !s.suspended || s.resumePending)
        return;
    bool attemptEnded = false;
    if (event[0] == kScanEvtLeMeta)
        attemptEnded = event[2] == kScanEvtLeMetaConnComplete ||
                       event[2] == kScanEvtLeMetaEnhancedConnComplete ||
                       event[2] == kScanEvtLeMetaEnhancedConnCompleteV2;
    else if (event[0] == kScanEvtDisconnComplete)
        attemptEnded = true;
    else if (event[0] == kScanEvtCmdStatus && length >= 6) {
        // Status(1) NumPackets(1) Opcode(2): status at [2], opcode at [4..5].
        const uint16_t op = uint16_t(event[4] | event[5] << 8);
        attemptEnded = (op == s.pendingOpcode && event[2] != 0) || op == kOpLeCreateConnCancel;
    } else if (event[0] == kScanEvtCmdComplete && length >= 6) {
        // NumPackets(1) Opcode(2) Status(1): opcode at [3..4], status at [5].
        const uint16_t op = uint16_t(event[3] | event[4] << 8);
        attemptEnded = (op == s.pendingOpcode && event[5] != 0) || op == kOpLeCreateConnCancel;
    }
    if (attemptEnded)
        s.resumePending = true;
}
// The pre-commands that bring scanning and advertising back after a parked create attempt;
// returns how many (0..4), exactly the forms this park switched off. Called before
// wantsPreScanOff, except for a create itself — resuming in front of a create would undo
// the park.
inline int wantsScanResume(ScanResumeState &s, PreCommand *out) {
    if (!s.suspended)
        return 0;
    // Watchdog: count the host commands that pass while parked, and force the restore when an
    // attempt-ending event never came. Legitimate attempts end via their cancel (~6 s) long
    // before this; the counter only climbs for an attempt the controller dropped entirely.
    if (!s.resumePending && ++s.stallGuard >= kResumeStallCommands)
        s.resumePending = s.watchdogFired = true;
    if (!s.resumePending)
        return 0;
    s.resumePending = false;
    s.suspended = false;
    s.pendingOpcode = 0;
    s.stallGuard = 0;
    int n = 0;
    if (s.parkedLegacyScan) {
        s.parkedLegacyScan = false;
        PreCommand &c = out[n++];
        c.data[0] = kOpLeScanEnable & 0xff; c.data[1] = kOpLeScanEnable >> 8; c.data[2] = 2;
        c.data[3] = s.filterDup; c.data[4] = s.enable;
        c.length = 5;
    }
    if (s.parkedExtScan) {
        s.parkedExtScan = false;
        PreCommand &c = out[n++];
        c.data[0] = kOpLeExtScanEnable & 0xff; c.data[1] = kOpLeExtScanEnable >> 8; c.data[2] = 2;
        c.data[3] = s.extEnable; c.data[4] = s.extFilterDup;
        c.length = 5;
    }
    if (s.parkedLegacyAdv) {
        s.parkedLegacyAdv = false;
        PreCommand &c = out[n++];
        c.data[0] = kOpLeAdvEnable & 0xff; c.data[1] = kOpLeAdvEnable >> 8; c.data[2] = 1;
        c.data[3] = 1;
        c.length = 4;
    }
    if (s.parkedExtAdv) {
        s.parkedExtAdv = false;
        PreCommand &c = out[n++];
        for (size_t i = 0; i < s.extAdvReplayLength; ++i)
            c.data[i] = s.extAdvReplay[i];
        c.length = s.extAdvReplayLength;
    }
    return n;
}
// bluetoothd initiates connections with the duty cycle it uses for background discovery — its
// own log line spells it out: "connection scan params: window - 0x30, interval - 0x1e0", i.e. a
// 30 ms window every 300 ms, 10 % of the time. An iPhone advertises its Continuity data on the
// order of a second or two, so a 10 % initiator misses most of those events inside the 6 s the
// attempt is allowed to live; the rapport link that carries com.sharing.hotspot.request then
// ends as "BLE client connect timeout". Widening the window to the interval makes the initiator
// listen continuously for those few seconds. Only the create's own scan window is rewritten:
// host scanning and advertising are left exactly as bluetoothd set them, which is what the
// earlier pre-scan park got wrong. Returns true when the command was modified; *observed gets
// the interval and window the host asked for (interval << 16 | window).
inline bool widenCreateScan(uint8_t *hci, size_t length, uint32_t *observed) {
    if (!hci || length < 4)
        return false;
    const uint16_t op = uint16_t(hci[0] | hci[1] << 8);
    if (op == kOpLeCreateConn) {
        // Scan_Interval then Scan_Window are the first fields of the payload.
        if (length < 7)
            return false;
        const uint16_t interval = uint16_t(hci[3] | hci[4] << 8), window = uint16_t(hci[5] | hci[6] << 8);
        if (observed)
            *observed = uint32_t(interval) << 16 | window;
        if (!window || interval <= window)
            return false;
        hci[5] = uint8_t(interval & 0xff);
        hci[6] = uint8_t(interval >> 8);
        return true;
    }
    if (op != kOpLeExtCreateConn)
        return false;
    // Filter policy, own address type, peer address type and address, then the initiating PHYs;
    // each supported PHY contributes fourteen bytes (Scan_Interval, Scan_Window and the
    // connection parameters), starting with its own scan pair.
    if (length < 17)
        return false;
    const uint8_t phys = hci[12];
    bool changed = false;
    for (unsigned i = 0; i < 3; ++i) {
        if (!(phys & (1U << i)))
            continue;
        const size_t base = 13 + 14 * i;
        if (base + 4 > length)
            break;
        const uint16_t interval = uint16_t(hci[base] | hci[base + 1] << 8),
                       window = uint16_t(hci[base + 2] | hci[base + 3] << 8);
        if (!i && observed)
            *observed = uint32_t(interval) << 16 | window;
        if (!window || interval <= window)
            continue;
        hci[base + 2] = uint8_t(interval & 0xff);
        hci[base + 3] = uint8_t(interval >> 8);
        changed = true;
    }
    return changed;
}

// True once per controller start, before the first legacy scan parameters command.
inline bool wantsLegacyScanAddress(State &s, const uint8_t *hci, size_t length) {
    if (!hci || length < 3 || uint16_t(hci[0] | hci[1] << 8) != kOpLeScanParams || s.legacyAddressSent)
        return false;
    s.legacyAddressSent = true;
    return true;
}
struct Action { bool sendSetAddress, sendEventMask; uint8_t output[11]; size_t outputLength; };
inline bool same(const uint8_t *a, const uint8_t *b) {
    for (unsigned i = 0; i < 6; ++i) if (a[i] != b[i]) return false;
    return true;
}
// hci is the whole command, opcode first. Call this for every outgoing command. The returned
// action must be sent after the command itself, since a set exists only after 0x2036.
inline Action onCommand(State &s, const uint8_t *hci, size_t length, bool extendedScanning) {
    Action a{};
    if (!hci || length < 3) return a;
    const uint16_t op = uint16_t(hci[0] | hci[1] << 8);
    switch (op) {
    case kOpReset:
        s = State{};
        break;
    case kOpLeRandomAddress:
        if (length >= 9 && !same(hci + 3, kFixedPatcherAddress)) {
            for (unsigned i = 0; i < 6; ++i) s.address[i] = hci[3 + i];
            s.haveAddress = true;
            s.haveGeneralAddress = true;
        }
        break;
    case kOpSetRandomAddress:
        // bluetoothd's address for this set. It's rejected if the set doesn't exist yet, so
        // this doesn't count as the set having one, but it's kept for when the set is created.
        if (length >= 10 && !same(hci + 4, kFixedPatcherAddress)) {
            if (hci[3] < 32) {
                for (unsigned i = 0; i < 6; ++i) s.setAddress[hci[3]][i] = hci[4 + i];
                s.knownSetAddresses |= 1U << hci[3];
            }
            if (!s.haveGeneralAddress) {
                for (unsigned i = 0; i < 6; ++i) s.address[i] = hci[4 + i];
                s.haveAddress = true;
            }
        }
        break;
    case kOpExtAdvParams:
        // Handle at parameter 0, Own_Address_Type at parameter 10.
        if (length >= 14 && hci[13] == 1 && hci[3] < 32 && !(s.addressedSets & (1U << hci[3])) &&
            (s.haveAddress || (s.knownSetAddresses & (1U << hci[3])))) {
            const uint8_t *chosen = (s.knownSetAddresses & (1U << hci[3])) ? s.setAddress[hci[3]] : s.address;
            s.addressedSets |= 1U << hci[3];
            a.sendSetAddress = true;
            a.output[0] = kOpSetRandomAddress & 0xff; a.output[1] = kOpSetRandomAddress >> 8; a.output[2] = 7;
            a.output[3] = hci[3];
            for (unsigned i = 0; i < 6; ++i) a.output[4 + i] = chosen[i];
            a.outputLength = 10;
        }
        break;
    case kOpRemoveSet:
        // A removed set forgets its address too: a set made later with the same handle must not be
        // given an address from before bluetoothd rotated it.
        if (length >= 4 && hci[3] < 32) {
            s.addressedSets &= ~(1U << hci[3]);
            s.knownSetAddresses &= ~(1U << hci[3]);
            for (unsigned i = 0; i < 6; ++i) s.setAddress[hci[3]][i] = 0;
        }
        break;
    case kOpClearSets:
        s.addressedSets = 0;
        s.knownSetAddresses = 0;
        for (auto &address : s.setAddress)
            for (unsigned i = 0; i < 6; ++i) address[i] = 0;
        break;
    case kOpLeEventMask:
        // Extended scanning reports are bit 12, which bluetoothd leaves off for this chip.
        if (extendedScanning && length >= 11 && !(hci[4] & 0x10)) {
            a.sendEventMask = true;
            for (unsigned i = 0; i < 11; ++i) a.output[i] = hci[i];
            a.output[4] |= 0x10;
            a.outputLength = 11;
        }
        break;
    default:
        break;
    }
    return a;
}
// Intel Bluetooth controllers never send an LE Data Length Change event on Tahoe, so bluetoothd
// stays at LE_READY_WAIT_FOR_LENGTH after the link encrypts and a new BLE HID device (a mouse, a
// keyboard) never binds: "No database exists". Already bonded devices skip GATT discovery, which
// is why they still work. IntelBTPatcher's fix is to send LE Read Remote Features twice more and
// rewrite the two duplicate completions into the events bluetoothd is waiting for. Lilu panics if
// two plugins hook the same function, so with this kext active that job is here instead.
// OpenIntelWireless/IntelBluetoothFirmware#514.
enum : uint16_t { kOpLeReadRemoteFeatures = 0x2016 };
enum : uint8_t { kEvtLeMeta = 0x3e, kLeMetaReadRemoteFeaturesComplete = 0x04 };
// The link is up and encrypted when this runs, so the two events describe that link: the handle
// comes from the completion being replaced. Values are IntelBTPatcher's: 27 octets, 328 us.
enum : size_t { kPhyUpdateEventLength = 8, kDataLengthEventLength = 13 };
// The duplicates are tracked per connection handle: with one shared stage, a second link reading
// remote features while the first link's duplicates were pending reset the stage, and the fake events
// could carry the wrong handle. An entry that gets no events (a controller may answer a duplicate
// with a Command Status error only) expires after kDataLengthExpiryNs, so nothing waits forever.
enum : unsigned { kDataLengthLinks = 8 };
// At most this many interrupt reads are wrapped per round, whatever else happens: a round that never
// matches must not keep every read of the controller's event pipe wrapped until it expires, which
// left USB devices unresponsive until a cold boot.
enum : uint32_t { kMaxWrappedReads = 24 };
enum : uint64_t { kDataLengthExpiryNs = 2000000000ULL };
struct DataLengthLink {
    uint16_t handle;       // 12-bit connection handle
    uint8_t stage;         // 0: the real completion, 1: fake PHY update, 2: fake data length
    uint8_t outstanding;   // duplicates whose completion hasn't been rewritten yet; 0 = free slot
    uint64_t armedNs;      // when the duplicates were sent
};
struct DataLengthState {
    DataLengthLink links[kDataLengthLinks];
    uint32_t expired;      // entries given up after kDataLengthExpiryNs
    uint32_t aborts;       // rounds dropped because nothing could be rewritten into a read
    uint32_t wrapped;      // reads wrapped since the last round was armed
};
inline uint16_t dataLengthHandle(uint8_t low, uint8_t high) { return uint16_t((low | high << 8) & 0x0fff); }
inline DataLengthLink *findLink(DataLengthState &s, uint16_t handle) {
    for (auto &l : s.links)
        if (l.outstanding && l.handle == handle)
            return &l;
    return nullptr;
}
// Frees the entries older than the expiry. Returns whether any entry is still waiting.
inline bool expireLinks(DataLengthState &s, uint64_t nowNs) {
    bool waiting = false;
    for (auto &l : s.links) {
        if (!l.outstanding)
            continue;
        if (nowNs - l.armedNs > kDataLengthExpiryNs) {
            l = DataLengthLink{};
            ++s.expired;
        } else {
            waiting = true;
        }
    }
    return waiting;
}
// Drops every pending link, keeping the counters: used when a read can't be rewritten (a failed
// transfer, no hold of our own on the buffer, a short write). Which link it belonged to can't be
// known without reading it, and a dropped round only leaves bluetoothd on the timeout it already had.
inline void abortLinks(DataLengthState &s) {
    for (auto &l : s.links)
        l = DataLengthLink{};
    ++s.aborts;
}
inline bool dataLengthPending(const DataLengthState &s) {
    for (auto &l : s.links)
        if (l.outstanding)
            return true;
    return false;
}
// How many copies of this command to send after it. Only LE Read Remote Features with its handle, and
// a controller start forgets every command still in flight. With every slot busy, no copies are sent:
// that link then behaves as it would without this fix.
inline unsigned extraRemoteReads(DataLengthState &s, const uint8_t *hci, size_t length, uint64_t nowNs) {
    if (!hci || length < 3)
        return 0;
    const uint16_t op = uint16_t(hci[0] | hci[1] << 8);
    if (op == kOpReset) {
        const uint32_t expired = s.expired, aborts = s.aborts;
        s = DataLengthState{};
        s.expired = expired;
        s.aborts = aborts;
        return 0;
    }
    if (op != kOpLeReadRemoteFeatures || length < 5 || hci[2] < 2)
        return 0;
    expireLinks(s, nowNs);
    const uint16_t handle = dataLengthHandle(hci[3], hci[4]);
    DataLengthLink *l = findLink(s, handle);
    if (!l) {
        for (auto &free : s.links)
            if (!free.outstanding) {
                l = &free;
                break;
            }
        if (!l)
            return 0;
    }
    // A repeated read on the same link starts over: its earlier duplicates are folded into this one.
    l->handle = handle;
    l->stage = 0;
    l->outstanding = 2;
    l->armedNs = nowNs;
    s.wrapped = 0;   // a new round starts with a full budget of wrapped reads
    return 2;
}
// Whether to wrap an interrupt read: something is waiting, the buffer can hold the longest rewritten
// event, and the round's budget isn't spent. A spent budget drops the round.
inline bool wrapRead(DataLengthState &s, uint64_t nowNs, size_t bufferLength) {
    if (!expireLinks(s, nowNs) || bufferLength < kDataLengthEventLength)
        return false;
    if (s.wrapped >= kMaxWrappedReads) {
        abortLinks(s);
        return false;
    }
    ++s.wrapped;
    return true;
}
// A copy that couldn't be sent never produces an event, so it's not waited for.
inline void forgetRemoteRead(DataLengthState &s, const uint8_t *hci, size_t length) {
    if (!hci || length < 5)
        return;
    DataLengthLink *l = findLink(s, dataLengthHandle(hci[3], hci[4]));
    if (l && !--l->outstanding)
        *l = DataLengthLink{};
}
// Rewrites one incoming event in place and returns its new size. Everything except a duplicate
// LE Read Remote Features Complete for a link that is waiting passes through untouched; the first of
// that link's two duplicates becomes PHY Update Complete, the second LE Data Length Change, as in
// IntelBTPatcher.
inline size_t rewriteEvent(DataLengthState &s, uint8_t *event, size_t length) {
    if (!event || length < 6 || event[0] != kEvtLeMeta || event[2] != kLeMetaReadRemoteFeaturesComplete)
        return length;
    const uint8_t handleLow = event[4], handleHigh = event[5];
    DataLengthLink *l = findLink(s, dataLengthHandle(handleLow, handleHigh));
    if (!l)
        return length;
    if (l->stage == 0) {  // the real completion goes to bluetoothd as it is
        l->stage = 1;
        return length;
    }
    // Status 0, both directions 2M.
    if (l->stage == 1) {
        const uint8_t fake[kPhyUpdateEventLength] = {kEvtLeMeta, 6, 0x0c, 0x00, handleLow, handleHigh, 0x02, 0x02};
        for (unsigned i = 0; i < kPhyUpdateEventLength; ++i)
            event[i] = fake[i];
        l->stage = 2;
        if (!--l->outstanding)
            *l = DataLengthLink{};
        return kPhyUpdateEventLength;
    }
    // A Data Length Change has no status byte, so the handle moves to parameter 0.
    const uint8_t fake[kDataLengthEventLength] = {kEvtLeMeta, 11, 0x07, handleLow, handleHigh,
                                                  0x1b, 0x00, 0x48, 0x01, 0x1b, 0x00, 0x48, 0x01};
    for (unsigned i = 0; i < kDataLengthEventLength; ++i)
        event[i] = fake[i];
    *l = DataLengthLink{};
    return kDataLengthEventLength;
}

// ---- Continuity broadcast witness -------------------------------------------------------
// Both halves of Instant Hotspot discovery exist only on the air, so the HCI layer is the one
// honest place to see them: the Mac broadcasts a 0x0D "Wi-Fi Settings" frame carrying a
// 4-byte iCloud-derived id to ask for a hotspot, and a phone signed into the same account
// answers with 0x0E only after matching that id. If the Mac never transmits 0x0D, no menu
// entry can appear and no amount of connection tuning will help; if 0x0E arrives but its
// report is not connectable, no create can ever land. Both are parsed here so a host test can
// drive the edges.

// Walks the AD structures of one payload and ORs every Apple Continuity type id into mask.
// Returns the last type byte seen, or 0xff when the payload carried no Apple manufacturer
// data. Length-prefixed walk: a truncated or alien payload stops rather than runs off.
inline uint8_t continuityTypes(const uint8_t *data, size_t length, uint32_t *mask) {
    uint8_t last = 0xff;
    if (!data)
        return last;
    for (size_t i = 0; i < length;) {
        const size_t field = data[i];
        if (field < 2 || i + field + 1 > length)
            break;
        if (data[i + 1] == 0xff && field >= 4 && data[i + 2] == 0x4c && data[i + 3] == 0x00) {
            const uint8_t type = data[i + 4];
            last = type;
            if (mask && type < 32)
                *mask |= 1U << type;
        }
        i += field + 1;
    }
    return last;
}
// Where the advertising payload sits inside an LE Set [Extended] Advertising Data command.
// Legacy: command length then data. Extended: set handle, fragment operation, fragment
// preference, data length, then data.
struct AdvPayload { size_t offset, length; };
inline bool advPayload(const uint8_t *hci, size_t length, AdvPayload *out) {
    if (!hci || length < 5 || !out)
        return false;
    const uint16_t op = uint16_t(hci[0] | hci[1] << 8);
    size_t offset, dataLength;
    if (op == kOpLeAdvData) {
        offset = 3;
        dataLength = hci[2];
    } else if (op == kOpLeExtAdvData) {
        if (length < 8)
            return false;
        offset = 7;
        dataLength = hci[6];
    } else
        return false;
    if (offset >= length)
        return false;
    if (offset + dataLength > length)
        dataLength = length - offset;
    out->offset = offset;
    out->length = dataLength;
    return dataLength > 0;
}
// One advertising report out of an LE Meta Event 0x02. The event type byte is the fact that
// decides whether a create can succeed: only a connectable report lets the controller answer
// with CONNECT_IND. 0x00 ADV_IND / 0x01 ADV_DIRECT_IND are connectable, 0x02 ADV_SCAN_IND and
// 0x03 ADV_NONCONN_IND are not, and the extended forms 0x0d-0x13 carry a connectable bit that
// the caller reads from the high nibble of the type's own encoding (kept verbatim here).
struct AdvReportItem { uint8_t eventType, addr[6], rssi; uint32_t appleTypes; };
inline int advReportItems(const uint8_t *event, size_t size, AdvReportItem *out, int max) {
    if (!event || !out || max <= 0 || size < 6 || event[0] != 0x3e || event[2] != 0x02)
        return 0;
    const uint8_t count = event[3];
    size_t offset = 4;
    int items = 0;
    for (unsigned r = 0; r < count && items < max && offset + 9 <= size; ++r) {
        const size_t dataLength = event[offset + 8];
        if (offset + 9 + dataLength + 1 > size)
            break;
        AdvReportItem &item = out[items++];
        item.eventType = event[offset];
        memcpy(item.addr, event + offset + 2, 6);
        item.rssi = event[offset + 9 + dataLength];
        item.appleTypes = 0;
        continuityTypes(event + offset + 9, dataLength, &item.appleTypes);
        offset += 9 + dataLength + 1;
    }
    return items;
}
}
