// What to do with outgoing HCI commands and incoming controller events. Kernel types stay out of
// this file so the host test can drive every branch of it.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
namespace Ibtp {

// ---- opcodes ---------------------------------------------------------------------------
// Values as in Linux's include/net/bluetooth/hci.h. Two of them used to be wrong here, which sent
// every park down a path bluetoothd never takes: the extended create is 0x2043, not 0x203a, and
// bluetoothd only uses that form at all on BT 5 controllers.
enum : uint16_t {
    OpReset = 0x0c03,
    OpLeEventMask = 0x2001,
    OpLeReadLocalFeatures = 0x2003,
    OpLeOwnRandomAddr = 0x2005,          // LE Set Random Address: the controller's own address
    OpLeAdvParams = 0x2006,
    OpLeAdvData = 0x2008,
    OpLeAdvEnable = 0x200a,
    OpLeScanParams = 0x200b,
    OpLeScanEnable = 0x200c,
    OpLeCreateConn = 0x200d,
    OpLeCreateConnCancel = 0x200e,
    OpLeAcceptListSize = 0x200f,
    OpLeAcceptListClear = 0x2010,
    OpLeAcceptListAdd = 0x2011,
    OpLeAcceptListRemove = 0x2012,
    OpLeConnUpdate = 0x2013,
    OpLeReadRemoteFeatures = 0x2016,
    OpLeResolvingAdd = 0x2027,
    OpLeResolvingRemove = 0x2028,
    OpLeResolvingClear = 0x2029,
    OpLeResolvingSize = 0x202d,
    OpLeAdvSetRandomAddr = 0x2035,       // LE Set Advertising Set Random Address: per set
    OpLeExtAdvParams = 0x2036,
    OpLeExtAdvData = 0x2037,
    OpLeExtScanParams = 0x2038,
    OpLeExtAdvEnable = 0x2039,
    OpLeRemoveAdvSet = 0x203c,
    OpLeClearAdvSets = 0x203d,
    OpLeExtScanEnable = 0x2042,
    OpLeExtCreateConn = 0x2043,
};
// What the controller reports back. The LE meta event wraps every subevent but the legacy ones.
enum : uint8_t {
    kEvtDisconnComplete = 0x05,
    kEvtCmdComplete = 0x0e,
    kEvtCmdStatus = 0x0f,
    kEvtLeMeta = 0x3e,
    kSubConnComplete = 0x01,
    kSubAdvReport = 0x02,
    kSubRemoteFeatures = 0x04,
    kSubConnCompleteEnhanced = 0x0a,
    kSubConnCompleteEnhancedV2 = 0x0b,
};
inline uint16_t opcodeOf(const uint8_t *hci) { return uint16_t(hci[0] | hci[1] << 8); }
// A 12-bit connection handle out of two event bytes; the top nibble carries flags.
inline uint16_t connHandle(uint8_t low, uint8_t high) { return uint16_t((low | high << 8) & 0x0fff); }
inline bool addressEquals(const uint8_t *a, const uint8_t *b) {
    for (unsigned i = 0; i < 6; ++i)
        if (a[i] != b[i])
            return false;
    return true;
}

// ---- what the host has asked the controller for ----------------------------------------
// Which route bluetoothd takes to reach a phone cannot be seen from the outside: it may hand the
// address straight to the controller (filter policy 0), put the phone on the filter accept list, or
// put it on the resolving list and let the controller match its resolvable private address - the
// last one is the only route that survives the phone rotating its address every minute. Telling
// them apart costs a reboot each, so one bitmask is collected per boot and read once.
enum : uint32_t {
    FpLegacyCreate = 1U << 0,
    FpExtCreate = 1U << 1,
    FpAcceptClear = 1U << 2,
    FpAcceptAdd = 1U << 3,
    FpAcceptRemove = 1U << 4,
    FpResolvingAdd = 1U << 5,
    FpResolvingClear = 1U << 6,
    FpResolvingSize = 1U << 7,
    FpAcceptSize = 1U << 8,
    FpExtAdvParams = 1U << 9,
    FpExtAdvData = 1U << 10,
    FpExtAdvEnable = 1U << 11,
    FpAdvParams = 1U << 12,
    FpAdvEnable = 1U << 13,
    FpScanEnable = 1U << 14,
    FpExtScanEnable = 1U << 15,
    FpOwnRandomAddr = 1U << 16,
    FpAdvSetRandomAddr = 1U << 17,
    FpRemoteFeatures = 1U << 18,
    FpConnUpdate = 1U << 19,
    FpCreateCancel = 1U << 20,
    FpResolvingRemove = 1U << 21,
    FpExtScanParams = 1U << 22,
    FpScanParams = 1U << 23,
};
struct OpBit { uint16_t opcode; uint32_t bit; };
// Every opcode worth recording, one row each. 0x2038 and 0x200b must never collapse into one entry:
// extended scan parameters and legacy scan parameters mean different things.
static const OpBit kOpBits[] = {
    {OpLeCreateConn, FpLegacyCreate},
    {OpLeExtCreateConn, FpExtCreate},
    {OpLeAcceptListClear, FpAcceptClear},
    {OpLeAcceptListAdd, FpAcceptAdd},
    {OpLeAcceptListRemove, FpAcceptRemove},
    {OpLeResolvingAdd, FpResolvingAdd},
    {OpLeResolvingRemove, FpResolvingRemove},
    {OpLeResolvingClear, FpResolvingClear},
    {OpLeResolvingSize, FpResolvingSize},
    {OpLeAcceptListSize, FpAcceptSize},
    {OpLeExtAdvParams, FpExtAdvParams},
    {OpLeExtAdvData, FpExtAdvData},
    {OpLeExtAdvEnable, FpExtAdvEnable},
    {OpLeAdvParams, FpAdvParams},
    {OpLeAdvEnable, FpAdvEnable},
    {OpLeScanEnable, FpScanEnable},
    {OpLeExtScanEnable, FpExtScanEnable},
    {OpLeOwnRandomAddr, FpOwnRandomAddr},
    {OpLeAdvSetRandomAddr, FpAdvSetRandomAddr},
    {OpLeReadRemoteFeatures, FpRemoteFeatures},
    {OpLeConnUpdate, FpConnUpdate},
    {OpLeCreateConnCancel, FpCreateCancel},
    {OpLeExtScanParams, FpExtScanParams},
    {OpLeScanParams, FpScanParams},
};
// The bit for an opcode worth recording, 0 for everything else.
inline uint32_t fingerprintOf(uint16_t op) {
    for (const OpBit &entry : kOpBits)
        if (entry.opcode == op)
            return entry.bit;
    return 0;
}
// The address IntelBTPatcher sends once before the first legacy scan, to work around a bluetoothd
// bug. Lilu panics when two plugins hook the same function, so this kext took that job over and the
// original is never loaded beside it. The address must never end up as an advertising address -
// that would make the Mac trackable.
static const uint8_t kLegacySeedAddress[6] = {0x94, 0x50, 0x64, 0xd0, 0x78, 0x6b};

// ---- the host's state as far as this kext can see it -----------------------------------
// ownAddress is the fallback for every set: bluetoothd's LE random address, or the last address
// seen until that arrives. perSetAddress holds what bluetoothd chose for each set. scanOn/scanDup
// and extScanOn/extScanDup mirror the last LE Scan Enable and LE Extended Scan Enable commands.
struct Mirror {
    uint8_t ownAddress[6];
    bool hasOwnAddress;
    bool ownFromController;         // ownAddress came from 0x2005, not from a set
    uint32_t addressed;             // sets already given an address, bit per handle
    bool seedSent;                  // the fixed legacy address has gone out
    uint8_t perSetAddress[32][6];
    uint32_t knownSets;             // sets whose address bluetoothd has named, bit per handle
    uint8_t scanOn, scanDup;
    uint8_t extScanOn, extScanDup;
    bool advOn;
    bool extAdvOn;
};

// ---- parking ---------------------------------------------------------------------------
// A payload that has to go out before the host's own command, at most four of them.
struct OutPacket {
    uint8_t bytes[72];
    size_t len;
};
// Builds a "turn it off" command: opcode, length byte, payload.
inline OutPacket &buildPacket(OutPacket &c, uint16_t op, uint8_t first, uint8_t second, size_t len) {
    c.bytes[0] = uint8_t(op & 0xff);
    c.bytes[1] = uint8_t(op >> 8);
    c.bytes[2] = uint8_t(len - 3);
    c.bytes[3] = first;
    if (len > 4)
        c.bytes[4] = second;
    c.len = len;
    return c;
}
// An LE Create Connection issued while the controller is scanning never forms here - the same
// documented Intel bug behind noble #165. The pending create gets cancelled and bluetoothd times
// the request out; that is the Instant Hotspot credential link (rapportd, central role, 6 s of fast
// scan rounds, then failure). bluetoothd scans through 0x2042 and connects through 0x2043 on BT 5,
// so both scan forms are parked with synthetic disables before a create leaves and restored once
// the attempt ends. The same applies to the host's advertising sets: sharingd advertises
// continuously for AirDrop discovery, and that is the remaining concurrency.
//
// Written on the command thread, read on the USB workloop when an attempt-ending event arrives; a
// missed restore only leaves scanning and advertising off until bluetoothd's next round.
struct Park {
    bool parked;                    // an attempt is parked, and we switched something off
    bool restorePending;            // the attempt ended; put it back with the next command
    bool forcedByWatchdog;          // the stall counter forced the restore, no event did
    uint16_t createOpcode;          // the create whose attempt is parked
    uint32_t stalledFor;            // host commands seen since the park
    uint8_t scanOn, scanDup;        // legacy scan state to put back
    uint8_t extScanOn, extScanDup;  // extended scan state to put back
    bool tookScan, tookExtScan;     // what this park actually switched off
    bool tookAdv, tookExtAdv;
    uint8_t advReplay[72];          // the last extended advertising enable, replayed verbatim
    size_t advReplaySize;
};
// Mirrors every host scan/advertising enable and, when a connection attempt starts, hands back the
// disables that park everything else. Returns how many packets go out first (0..4).
//
// allowPark=false keeps it a pure mirror: it still records every enable, it just never synthesises
// a disable. Parking has been OFF by default since 2.7.9: it was never seen to make a create
// succeed, and a field test showed it kills Instant Hotspot outright - the phone can only offer the
// password share while it can see the Mac's advertising, and the park switched that off (the Wi-Fi
// menu's same-Apple-ID entry vanished with it).
inline int parkBeforeCreate(Mirror &m, Park &p, const uint8_t *hci, size_t length, OutPacket *out,
                            bool allowPark = true) {
    if (!hci || length < 3)
        return 0;
    switch (opcodeOf(hci)) {
    case OpLeScanEnable:
        // The host just re-asserted control over legacy scanning; forget any park of it.
        if (length < 5)
            return 0;
        m.scanDup = hci[3];
        m.scanOn = hci[4];
        if (p.parked && m.scanOn)
            p.tookScan = false;
        return 0;
    case OpLeExtScanEnable:
        // Same for extended scanning; here the enable comes first and the filter policy second.
        if (length < 5)
            return 0;
        m.extScanDup = hci[4];
        m.extScanOn = hci[3];
        if (p.parked && m.extScanOn)
            p.tookExtScan = false;
        return 0;
    case OpLeAdvEnable:
        if (length < 4)
            return 0;
        m.advOn = hci[3] != 0;
        if (p.parked && m.advOn)
            p.tookAdv = false;
        return 0;
    case OpLeExtAdvEnable:
        if (length < 4)
            return 0;
        m.extAdvOn = hci[3] != 0;
        if (m.extAdvOn && length <= sizeof(p.advReplay)) {
            memcpy(p.advReplay, hci, length);
            p.advReplaySize = length;
        }
        if (p.parked && m.extAdvOn)
            p.tookExtAdv = false;
        return 0;
    case OpLeCreateConn:
    case OpLeExtCreateConn:
        break;
    default:
        return 0;
    }
    if (!allowPark)
        return 0;
    p.createOpcode = opcodeOf(hci);
    p.stalledFor = 0;
    p.forcedByWatchdog = false;
    int n = 0;
    if (m.scanOn) {
        p.scanOn = m.scanOn;
        p.scanDup = m.scanDup;
        m.scanOn = 0;
        buildPacket(out[n++], OpLeScanEnable, 0, 0, 5);       // filter duplicates off, scanning off
        p.tookScan = true;
    }
    if (m.extScanOn) {
        p.extScanOn = m.extScanOn;
        p.extScanDup = m.extScanDup;
        m.extScanOn = 0;
        buildPacket(out[n++], OpLeExtScanEnable, 0, 0, 5);    // enable off, filter policy ignored
        p.tookExtScan = true;
    }
    if (m.advOn) {
        m.advOn = false;
        buildPacket(out[n++], OpLeAdvEnable, 0, 0, 4);        // advertising off
        p.tookAdv = true;
    }
    if (m.extAdvOn) {
        m.extAdvOn = false;
        buildPacket(out[n++], OpLeExtAdvEnable, 0, 0, 5);     // enable off, zero sets
        p.tookExtAdv = true;
    }
    if (n)
        p.parked = true;
    return n;
}
// A parked attempt that outlives this many host commands is force-restored. bluetoothd's own cancel
// path normally gets there first (~6 s); this covers an attempt whose every event was dropped.
constexpr uint32_t kStallLimit = 128;
// Events that end a connection attempt: an LE meta connection complete, a plain disconnection, or a
// command-level answer to the create. The command-level ones close a gap the LE meta events leave:
// when the controller eats the create, it may answer Command Status with an error, or swallow the
// attempt and leave bluetoothd cancelling (0x200e) after its 6 s timeout - that cancel's Command
// Status/Complete arrives with no LE meta event at all. Without them, parked scanning and
// advertising stayed off for good, which is what made the hotspot password share spin forever.
inline void observeEndingEvent(Park &p, const uint8_t *event, size_t length) {
    if (!event || length < 3 || !p.parked || p.restorePending)
        return;
    bool ended = false;
    if (event[0] == kEvtLeMeta) {
        ended = event[2] == kSubConnComplete || event[2] == kSubConnCompleteEnhanced ||
                event[2] == kSubConnCompleteEnhancedV2;
    } else if (event[0] == kEvtDisconnComplete) {
        ended = true;
    } else if (event[0] == kEvtCmdStatus && length >= 6) {
        // status, numPackets, opcode: status at [2], opcode at [4..5].
        const uint16_t op = uint16_t(event[4] | event[5] << 8);
        ended = (op == p.createOpcode && event[2] != 0) || op == OpLeCreateConnCancel;
    } else if (event[0] == kEvtCmdComplete && length >= 6) {
        // numPackets, opcode, status: opcode at [3..4], status at [5].
        const uint16_t op = uint16_t(event[3] | event[4] << 8);
        ended = (op == p.createOpcode && event[5] != 0) || op == OpLeCreateConnCancel;
    }
    if (ended)
        p.restorePending = true;
}
// The packets that bring back exactly what the park switched off (0..4). Called before
// parkBeforeCreate for every command except a create itself - restoring in front of a create would
// undo the park.
inline int restoreAfterCreate(Park &p, OutPacket *out) {
    if (!p.parked)
        return 0;
    // The counter only climbs while no ending event has arrived; a legitimate attempt cancels long
    // before this, so a full counter means the controller dropped the attempt entirely.
    if (!p.restorePending && ++p.stalledFor >= kStallLimit)
        p.restorePending = p.forcedByWatchdog = true;
    if (!p.restorePending)
        return 0;
    p.restorePending = false;
    p.parked = false;
    p.createOpcode = 0;
    p.stalledFor = 0;
    int n = 0;
    if (p.tookScan) {
        p.tookScan = false;
        buildPacket(out[n++], OpLeScanEnable, p.scanDup, p.scanOn, 5);
    }
    if (p.tookExtScan) {
        p.tookExtScan = false;
        buildPacket(out[n++], OpLeExtScanEnable, p.extScanOn, p.extScanDup, 5);
    }
    if (p.tookAdv) {
        p.tookAdv = false;
        buildPacket(out[n++], OpLeAdvEnable, 1, 0, 4);
    }
    if (p.tookExtAdv) {
        p.tookExtAdv = false;
        OutPacket &c = out[n++];
        for (size_t i = 0; i < p.advReplaySize; ++i)
            c.bytes[i] = p.advReplay[i];
        c.len = p.advReplaySize;
    }
    return n;
}

// ---- the initiating scan window --------------------------------------------------------
// bluetoothd initiates with the duty cycle it uses for background discovery; its own log says so:
// "connection scan params: window - 0x30, interval - 0x1e0", a 30 ms window every 300 ms. An iPhone
// advertises its Continuity data for a second or two at a time, so a 10 % initiator misses most of
// it inside the 6 s an attempt lives, and the rapport link carrying com.sharing.hotspot.request ends
// as "BLE client connect timeout". Widening the window to the interval makes the initiator listen
// continuously for those seconds. Only the create's own window is rewritten - host scanning and
// advertising stay exactly as bluetoothd set them, which is what the earlier pre-scan park got
// wrong. Returns true when the command changed; *observed gets interval << 16 | window.
inline bool widenInitiatorScan(uint8_t *hci, size_t length, uint32_t *observed) {
    if (!hci || length < 4)
        return false;
    const uint16_t op = opcodeOf(hci);
    if (op == OpLeCreateConn) {
        // Scan_Interval then Scan_Window, at the head of the payload.
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
    if (op != OpLeExtCreateConn)
        return false;
    // Filter policy, own address type, peer address type and address, then the PHY mask; every
    // supported PHY adds fourteen bytes that start with its own scan interval/window pair.
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

// True once per controller start, just before the first legacy scan parameters command.
inline bool shouldSeedLegacyAddress(Mirror &m, const uint8_t *hci, size_t length) {
    if (!hci || length < 3 || opcodeOf(hci) != OpLeScanParams || m.seedSent)
        return false;
    m.seedSent = true;
    return true;
}

// ---- commands that have to follow the host's own ---------------------------------------
// What to send after the host's command was accepted, and whether anything is to be sent at all.
struct FollowUp {
    bool setAddress;
    bool eventMask;
    uint8_t bytes[11];
    size_t len;
};
// Call for every outgoing command; hci starts at the opcode. A set only exists after 0x2036, so
// whatever this asks for has to go out after the command itself.
inline FollowUp mirrorCommand(Mirror &m, const uint8_t *hci, size_t length, bool extendedScanning) {
    FollowUp f {};
    if (!hci || length < 3)
        return f;
    switch (opcodeOf(hci)) {
    case OpReset:
        m = Mirror{};
        break;
    case OpLeOwnRandomAddr:
        if (length >= 9 && !addressEquals(hci + 3, kLegacySeedAddress)) {
            for (unsigned i = 0; i < 6; ++i)
                m.ownAddress[i] = hci[3 + i];
            m.hasOwnAddress = true;
            m.ownFromController = true;
        }
        break;
    case OpLeAdvSetRandomAddr:
        // bluetoothd's address for this set. It is rejected while the set does not exist yet, so
        // this is not proof the set has one - it is kept for when the set is created.
        if (length >= 10 && !addressEquals(hci + 4, kLegacySeedAddress)) {
            if (hci[3] < 32) {
                for (unsigned i = 0; i < 6; ++i)
                    m.perSetAddress[hci[3]][i] = hci[4 + i];
                m.knownSets |= 1U << hci[3];
            }
            if (!m.ownFromController) {
                for (unsigned i = 0; i < 6; ++i)
                    m.ownAddress[i] = hci[4 + i];
                m.hasOwnAddress = true;
            }
        }
        break;
    case OpLeExtAdvParams:
        // Handle at parameter 0, own address type at parameter 10.
        if (length >= 14 && hci[13] == 1 && hci[3] < 32 && !(m.addressed & (1U << hci[3])) &&
            (m.hasOwnAddress || (m.knownSets & (1U << hci[3])))) {
            const uint8_t *chosen = (m.knownSets & (1U << hci[3])) ? m.perSetAddress[hci[3]] : m.ownAddress;
            m.addressed |= 1U << hci[3];
            f.setAddress = true;
            f.bytes[0] = OpLeAdvSetRandomAddr & 0xff;
            f.bytes[1] = OpLeAdvSetRandomAddr >> 8;
            f.bytes[2] = 7;
            f.bytes[3] = hci[3];
            for (unsigned i = 0; i < 6; ++i)
                f.bytes[4 + i] = chosen[i];
            f.len = 10;
        }
        break;
    case OpLeRemoveAdvSet:
        // A removed set forgets its address too, or a set later created on the same handle would
        // inherit one bluetoothd has already rotated away from.
        if (length >= 4 && hci[3] < 32) {
            m.addressed &= ~(1U << hci[3]);
            m.knownSets &= ~(1U << hci[3]);
            for (unsigned i = 0; i < 6; ++i)
                m.perSetAddress[hci[3]][i] = 0;
        }
        break;
    case OpLeClearAdvSets:
        m.addressed = 0;
        m.knownSets = 0;
        for (auto &address : m.perSetAddress)
            for (unsigned i = 0; i < 6; ++i)
                address[i] = 0;
        break;
    case OpLeEventMask:
        // Bit 12 is the extended scanning report, and bluetoothd leaves it off for this chip.
        if (extendedScanning && length >= 11 && !(hci[4] & 0x10)) {
            f.eventMask = true;
            for (unsigned i = 0; i < 11; ++i)
                f.bytes[i] = hci[i];
            f.bytes[4] |= 0x10;
            f.len = 11;
        }
        break;
    default:
        break;
    }
    return f;
}

// ---- LE Data Length Change -----------------------------------------------------------------
// Intel controllers never send an LE Data Length Change event on Tahoe, so bluetoothd sits at
// LE_READY_WAIT_FOR_LENGTH once the link encrypts and a new BLE HID device never binds ("No
// database exists"). Already bonded devices skip GATT discovery, which is why they still work. The
// fix is to send LE Read Remote Features twice more and rewrite the two duplicate completions into
// the events bluetoothd is waiting for. Lilu panics on a double hook, so with this kext loaded that
// job is here. OpenIntelWireless/IntelBluetoothFirmware#514.
//
// The link is up and encrypted here, so both events describe that link: the handle comes from the
// completion being replaced. 27 octets at 328 us, as upstream sends them.
enum : size_t { kPhyUpdateLen = 8, kDataLengthLen = 13 };
// Duplicates are tracked per connection handle. A single shared stage let a second link reset the
// first one's round, and the fake events could carry the wrong handle.
enum : unsigned { kFeatureSlots = 8 };
// At most this many reads are wrapped per round: a round that never matches must not keep wrapping
// every read of the event pipe, which left USB devices unresponsive until a cold boot.
enum : uint32_t { kWrapBudget = 24 };
enum : uint64_t { kRoundExpiryNs = 2000000000ULL };
struct FeatureSlot {
    uint16_t handle;    // 12-bit connection handle
    uint8_t stage;      // 0: the real completion, 1: fake PHY update, 2: fake data length
    uint8_t left;       // duplicates still to be rewritten; 0 means the slot is free
    uint64_t armedNs;
};
struct FeatureBoard {
    FeatureSlot slots[kFeatureSlots];
    uint32_t expired;   // rounds given up after kRoundExpiryNs
    uint32_t aborts;    // rounds dropped because nothing could be rewritten into a read
    uint32_t wrapped;   // reads wrapped since the last round was armed
};
inline FeatureSlot *findSlot(FeatureBoard &b, uint16_t handle) {
    for (auto &slot : b.slots)
        if (slot.left && slot.handle == handle)
            return &slot;
    return nullptr;
}
// Gives up the slots older than the expiry. Returns whether anything is still waiting.
inline bool sweepSlots(FeatureBoard &b, uint64_t nowNs) {
    bool waiting = false;
    for (auto &slot : b.slots) {
        if (!slot.left)
            continue;
        if (nowNs - slot.armedNs > kRoundExpiryNs) {
            slot = FeatureSlot{};
            ++b.expired;
        } else {
            waiting = true;
        }
    }
    return waiting;
}
// Drops every pending slot but keeps the counters: used when a read cannot be rewritten (failed
// transfer, no hold of our own, short write). Which link it belonged to cannot be known without
// reading it, and a dropped round only leaves bluetoothd on the timeout it already had.
inline void dropRound(FeatureBoard &b) {
    for (auto &slot : b.slots)
        slot = FeatureSlot{};
    ++b.aborts;
}
inline bool roundPending(const FeatureBoard &b) {
    for (auto &slot : b.slots)
        if (slot.left)
            return true;
    return false;
}
// How many copies of this command to send after it. Only LE Read Remote Features with a handle
// qualifies, and a controller start forgets every command still in flight. With every slot taken no
// copy is sent, so that link behaves as it would without this fix.
inline unsigned duplicateReads(FeatureBoard &b, const uint8_t *hci, size_t length, uint64_t nowNs) {
    if (!hci || length < 3)
        return 0;
    const uint16_t op = opcodeOf(hci);
    if (op == OpReset) {
        const uint32_t expired = b.expired, aborts = b.aborts;
        b = FeatureBoard{};
        b.expired = expired;
        b.aborts = aborts;
        return 0;
    }
    if (op != OpLeReadRemoteFeatures || length < 5 || hci[2] < 2)
        return 0;
    sweepSlots(b, nowNs);
    const uint16_t handle = connHandle(hci[3], hci[4]);
    FeatureSlot *slot = findSlot(b, handle);
    if (!slot) {
        for (auto &free : b.slots)
            if (!free.left) {
                slot = &free;
                break;
            }
        if (!slot)
            return 0;
    }
    // A repeated read on the same link starts over and folds the earlier duplicates into it.
    slot->handle = handle;
    slot->stage = 0;
    slot->left = 2;
    slot->armedNs = nowNs;
    b.wrapped = 0;      // a new round gets a full wrap budget
    return 2;
}
// Whether to wrap an interrupt read: something is waiting, the buffer can hold the longest rewritten
// event, and the round's budget is not spent. A spent budget drops the round.
inline bool takeWrap(FeatureBoard &b, uint64_t nowNs, size_t bufferLength) {
    if (!sweepSlots(b, nowNs) || bufferLength < kDataLengthLen)
        return false;
    if (b.wrapped >= kWrapBudget) {
        dropRound(b);
        return false;
    }
    ++b.wrapped;
    return true;
}
// A copy that could not be sent never produces an event, so nothing waits for it.
inline void dropDuplicate(FeatureBoard &b, const uint8_t *hci, size_t length) {
    if (!hci || length < 5)
        return;
    FeatureSlot *slot = findSlot(b, connHandle(hci[3], hci[4]));
    if (slot && !--slot->left)
        *slot = FeatureSlot{};
}
// Rewrites one incoming event in place and returns its new size. Everything except a duplicate LE
// Read Remote Features Complete for a waiting link passes through untouched: the first of that
// link's two duplicates becomes PHY Update Complete, the second LE Data Length Change.
inline size_t forgeEvent(FeatureBoard &b, uint8_t *event, size_t length) {
    if (!event || length < 6 || event[0] != kEvtLeMeta || event[2] != kSubRemoteFeatures)
        return length;
    const uint8_t handleLow = event[4], handleHigh = event[5];
    FeatureSlot *slot = findSlot(b, connHandle(handleLow, handleHigh));
    if (!slot)
        return length;
    if (slot->stage == 0) {     // the real completion goes to bluetoothd as it is
        slot->stage = 1;
        return length;
    }
    if (slot->stage == 1) {
        // Status 0, both directions 2M.
        const uint8_t fake[kPhyUpdateLen] = {kEvtLeMeta, 6, 0x0c, 0x00, handleLow, handleHigh, 0x02, 0x02};
        for (unsigned i = 0; i < kPhyUpdateLen; ++i)
            event[i] = fake[i];
        slot->stage = 2;
        if (!--slot->left)
            *slot = FeatureSlot{};
        return kPhyUpdateLen;
    }
    // A Data Length Change has no status byte, so the handle moves to parameter 0.
    const uint8_t fake[kDataLengthLen] = {kEvtLeMeta, 11, 0x07, handleLow, handleHigh,
                                          0x1b, 0x00, 0x48, 0x01, 0x1b, 0x00, 0x48, 0x01};
    for (unsigned i = 0; i < kDataLengthLen; ++i)
        event[i] = fake[i];
    *slot = FeatureSlot{};
    return kDataLengthLen;
}

// ---- Continuity over the air --------------------------------------------------------------
// Both halves of Instant Hotspot discovery exist only on the air, so the HCI layer is the one honest
// place to look: the Mac broadcasts a 0x0D "Wi-Fi Settings" frame carrying a 4-byte iCloud-derived
// id to ask for a hotspot, and a phone signed into the same account answers with 0x0E only after
// matching that id. If the Mac never transmits 0x0D no menu entry can appear, and no amount of
// connection tuning helps; if 0x0E arrives with a report that is not connectable, no create can
// land. Both are parsed here so a host test can drive the edges.

// Walks the AD structures of one payload and ORs every Apple Continuity type id into mask. Returns
// the last type byte seen, or 0xff when the payload carried no Apple manufacturer data. The walk is
// length-prefixed, so a truncated or alien payload stops instead of running off.
inline uint8_t appleTypeIds(const uint8_t *data, size_t length, uint32_t *mask) {
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
// Where the advertising payload sits inside an LE Set [Extended] Advertising Data command. Legacy:
// command length, then the data. Extended: set handle, fragment operation, fragment preference,
// data length, then the data.
struct PayloadRange {
    size_t offset, length;
};
inline bool payloadRange(const uint8_t *hci, size_t length, PayloadRange *out) {
    if (!hci || length < 5 || !out)
        return false;
    const uint16_t op = opcodeOf(hci);
    size_t offset, dataLength;
    if (op == OpLeAdvData) {
        offset = 3;
        dataLength = hci[2];
    } else if (op == OpLeExtAdvData) {
        if (length < 8)
            return false;
        offset = 7;
        dataLength = hci[6];
    } else {
        return false;
    }
    if (offset >= length)
        return false;
    if (offset + dataLength > length)
        dataLength = length - offset;
    out->offset = offset;
    out->length = dataLength;
    return dataLength > 0;
}
// One advertising report out of an LE Meta Event 0x02. The event type byte is the fact that decides
// whether a create can succeed, because only a connectable report lets the controller answer with
// CONNECT_IND: 0x00 ADV_IND and 0x01 ADV_DIRECT_IND are connectable, 0x02 ADV_SCAN_IND and 0x03
// ADV_NONCONN_IND are not, and the extended forms 0x0d-0x13 carry the bit in their own encoding.
struct Report {
    uint8_t eventType, addr[6], rssi;
    uint32_t appleTypes;
};
inline int parseReports(const uint8_t *event, size_t size, Report *out, int max) {
    if (!event || !out || max <= 0 || size < 6 || event[0] != kEvtLeMeta || event[2] != kSubAdvReport)
        return 0;
    const uint8_t count = event[3];
    size_t offset = 4;
    int items = 0;
    for (unsigned r = 0; r < count && items < max && offset + 9 <= size; ++r) {
        const size_t dataLength = event[offset + 8];
        if (offset + 9 + dataLength + 1 > size)
            break;
        Report &item = out[items++];
        item.eventType = event[offset];
        memcpy(item.addr, event + offset + 2, 6);
        item.rssi = event[offset + 9 + dataLength];
        item.appleTypes = 0;
        appleTypeIds(event + offset + 9, dataLength, &item.appleTypes);
        offset += 9 + dataLength + 1;
    }
    return items;
}
}
