// Host tests for the scan-park resume logic in IntelBTPatcherHci.hpp (pure logic, no kernel types).
// Build & run:  clang++ -std=c++17 -Wall -Wextra -I. -o /tmp/test-scan-resume test-scan-resume.cpp && /tmp/test-scan-resume
#include "IntelBTPatcherHci.hpp"
#include <cstdio>
#include <cstring>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

using namespace Ibtp;

static Mirror baseState() {
    Mirror s{};
    s.extScanOn = 1; s.extScanDup = 1;
    s.extAdvOn = true;
    return s;
}

// Simulates bluetoothd enabling extended scanning (0x2042) and one advertising set (0x2039),
// through the mirror, so advReplay holds the verbatim packet.
static void hostEnablesAll(Mirror &s, Park &r) {
    uint8_t scan[5] = {0x42, 0x20, 0x02, 0x01, 0x01};
    OutPacket out[4] = {};
    CHECK(parkBeforeCreate(s, r, scan, sizeof(scan), out) == 0);
    CHECK(s.extScanOn == 1);
    uint8_t adv[10] = {0x39, 0x20, 0x07, 0x01, 0x01, 0x00, 0x02, 0x00, 0x7f, 0x00};
    CHECK(parkBeforeCreate(s, r, adv, sizeof(adv), out) == 0);
    CHECK(s.extAdvOn);
}

static int tickCommand(Mirror &s, Park &r) {
    // Any non-create command passes through parkBeforeCreate (no-op) and restoreAfterCreate.
    uint8_t cmd[4] = {0x05, 0x20, 0x01, 0x01};
    OutPacket out[4] = {};
    parkBeforeCreate(s, r, cmd, sizeof(cmd), out);
    return restoreAfterCreate(r, out);
}

int main() {
    // --- 1. Park on an extended create: ext scan + ext adv both switched off, verbatim replay kept.
    {
        Mirror s = baseState();
        Park r{};
        hostEnablesAll(s, r);
        uint8_t create[11] = {0x43, 0x20, 0x08, 0x00, 0x01, 1,2,3,4,5,6};
        OutPacket out[4] = {};
        const int n = parkBeforeCreate(s, r, create, sizeof(create), out);
        CHECK(n == 2);                       // ext scan off + ext adv off
        CHECK(r.parked && !r.restorePending);
        CHECK(r.createOpcode == OpLeExtCreateConn);
        CHECK(r.tookExtScan && r.tookExtAdv);
        CHECK(!r.tookScan && !r.tookAdv);
        CHECK(s.extScanOn == 0 && !s.extAdvOn);
        CHECK(r.advReplaySize == 10 && r.advReplay[0] == 0x39 && r.advReplay[2] == 0x07);
    }

    // --- 2. Command Status (0x0f) with an error for the create ends the attempt.
    {
        Mirror s = baseState();
        Park r{};
        hostEnablesAll(s, r);
        uint8_t create[11] = {0x43, 0x20, 0x08, 0x00, 0x01, 1,2,3,4,5,6};
        OutPacket out[4] = {};
        CHECK(parkBeforeCreate(s, r, create, sizeof(create), out) == 2);
        // Command Status: 0x0f, len 4, status 0x0C, numPackets 1, opcode 0x2043.
        uint8_t evt[6] = {0x0f, 0x04, 0x0C, 0x01, 0x43, 0x20};
        observeEndingEvent(r, evt, sizeof(evt));
        CHECK(r.restorePending);
        const int n = restoreAfterCreate(r, out);
        CHECK(n == 2);
        CHECK(!r.parked && !r.restorePending && r.createOpcode == 0);
        CHECK(out[0].bytes[0] == 0x42 && out[0].bytes[3] == 1 && out[0].bytes[4] == 1);  // ext scan restored
        CHECK(out[1].bytes[0] == 0x39 && out[1].len == 10);                             // adv replay verbatim
    }

    // --- 3. Command Status success (0x00) for the create does NOT end the attempt.
    {
        Mirror s = baseState();
        Park r{};
        hostEnablesAll(s, r);
        uint8_t create[11] = {0x43, 0x20, 0x08, 0x00, 0x01, 1,2,3,4,5,6};
        OutPacket out[4] = {};
        parkBeforeCreate(s, r, create, sizeof(create), out);
        uint8_t evt[6] = {0x0f, 0x04, 0x00, 0x01, 0x43, 0x20};
        observeEndingEvent(r, evt, sizeof(evt));
        CHECK(!r.restorePending);
        // ...and a Command Complete error for the same opcode does.
        uint8_t evt2[7] = {0x0e, 0x05, 0x01, 0x43, 0x20, 0x12, 0x00};
        observeEndingEvent(r, evt2, sizeof(evt2));
        CHECK(r.restorePending);
    }

    // --- 4. Create Connection Cancel (0x200e) ends the attempt from either command event form.
    {
        Mirror s = baseState();
        Park r{};
        hostEnablesAll(s, r);
        uint8_t create[11] = {0x43, 0x20, 0x08, 0x00, 0x01, 1,2,3,4,5,6};
        OutPacket out[4] = {};
        parkBeforeCreate(s, r, create, sizeof(create), out);
        uint8_t cancelStatus[6] = {0x0f, 0x04, 0x00, 0x01, 0x0e, 0x20};
        observeEndingEvent(r, cancelStatus, sizeof(cancelStatus));
        CHECK(r.restorePending);
        restoreAfterCreate(r, out);
        CHECK(!r.parked);

        // Again, via the Command Complete form.
        Park r2{};
        hostEnablesAll(s, r2);
        parkBeforeCreate(s, r2, create, sizeof(create), out);
        uint8_t cancelComplete[7] = {0x0e, 0x05, 0x01, 0x0e, 0x20, 0x02, 0x00};
        observeEndingEvent(r2, cancelComplete, sizeof(cancelComplete));
        CHECK(r2.restorePending);
    }

    // --- 5. LE meta conn complete still ends the attempt (regression).
    {
        Mirror s = baseState();
        Park r{};
        hostEnablesAll(s, r);
        uint8_t create[11] = {0x43, 0x20, 0x08, 0x00, 0x01, 1,2,3,4,5,6};
        OutPacket out[4] = {};
        parkBeforeCreate(s, r, create, sizeof(create), out);
        uint8_t meta[8] = {0x3e, 0x06, 0x01, 0x00, 1,2,3,4};
        observeEndingEvent(r, meta, sizeof(meta));
        CHECK(r.restorePending);
    }

    // --- 6. Watchdog: an attempt whose events never come is force-resumed.
    {
        Mirror s = baseState();
        Park r{};
        hostEnablesAll(s, r);
        uint8_t create[11] = {0x43, 0x20, 0x08, 0x00, 0x01, 1,2,3,4,5,6};
        OutPacket out[4] = {};
        CHECK(parkBeforeCreate(s, r, create, sizeof(create), out) == 2);
        for (uint32_t i = 0; i < kStallLimit - 1; ++i)
            CHECK(tickCommand(s, r) == 0);        // one short of the limit: nothing yet
        CHECK(r.parked && !r.restorePending);
        const int n = tickCommand(s, r);          // the 128th command fires the watchdog
        CHECK(n == 2 && r.forcedByWatchdog);
        CHECK(!r.parked && r.stalledFor == 0);
        CHECK(r.restorePending == false);
        // A late error event after the resume is harmless: nothing is parked any more.
        uint8_t evt[6] = {0x0f, 0x04, 0x0C, 0x01, 0x43, 0x20};
        observeEndingEvent(r, evt, sizeof(evt));
        CHECK(!r.restorePending);
    }

    // --- 7. Host re-asserting ext scan while parked: resume restores only the advertising.
    {
        Mirror s = baseState();
        Park r{};
        hostEnablesAll(s, r);
        uint8_t create[11] = {0x43, 0x20, 0x08, 0x00, 0x01, 1,2,3,4,5,6};
        OutPacket out[4] = {};
        CHECK(parkBeforeCreate(s, r, create, sizeof(create), out) == 2);
        uint8_t rescan[5] = {0x42, 0x20, 0x02, 0x01, 0x01};
        CHECK(parkBeforeCreate(s, r, rescan, sizeof(rescan), out) == 0);
        CHECK(!r.tookExtScan);              // host took over scanning again
        CHECK(s.extScanOn == 1);
        uint8_t evt[6] = {0x0f, 0x04, 0x0C, 0x01, 0x43, 0x20};
        observeEndingEvent(r, evt, sizeof(evt));
        const int n = restoreAfterCreate(r, out);
        CHECK(n == 1);                        // only the advertising replay
        CHECK(out[0].bytes[0] == 0x39);
        CHECK(!r.parked);
    }

    // --- 8. Legacy create parks only what legacy code had enabled.
    {
        Mirror s = Mirror{};
        s.scanOn = 1; s.scanDup = 1;
        Park r{};
        uint8_t create[15] = {0x0d, 0x20, 0x0c, 0x20,0x00,0x20,0x00, 0x00, 0x01, 1,2,3,4,5,6};
        OutPacket out[4] = {};
        const int n = parkBeforeCreate(s, r, create, sizeof(create), out);
        CHECK(n == 1);
        CHECK(r.createOpcode == OpLeCreateConn);
        CHECK(r.tookScan && !r.tookExtScan && !r.tookExtAdv);
        uint8_t evt[6] = {0x0f, 0x04, 0x0C, 0x01, 0x0d, 0x20};
        observeEndingEvent(r, evt, sizeof(evt));
        const int n2 = restoreAfterCreate(r, out);
        CHECK(n2 == 1 && out[0].bytes[0] == 0x0c && out[0].bytes[4] == 1);
    }

    // --- 9. allowPark=false (2.7.9 default): no park at all, but the mirrors still follow the
    // host's enables, so the witness and any later opt-in park stay correct.
    {
        Mirror s = Mirror{};
        Park r{};
        uint8_t extScanOn[5] = {0x42, 0x20, 0x02, 0x01, 0x01};
        OutPacket out[4] = {};
        CHECK(parkBeforeCreate(s, r, extScanOn, sizeof(extScanOn), out, false) == 0);
        CHECK(s.extScanOn == 1);
        uint8_t extAdvOn[10] = {0x39, 0x20, 0x07, 0x01, 0x00, 0x00, 0x02, 0x00, 0x7f, 0x00};
        CHECK(parkBeforeCreate(s, r, extAdvOn, sizeof(extAdvOn), out, false) == 0);
        CHECK(s.extAdvOn);
        uint8_t create[11] = {0x43, 0x20, 0x08, 0x00, 0x01, 1,2,3,4,5,6};
        CHECK(parkBeforeCreate(s, r, create, sizeof(create), out, false) == 0);
        CHECK(!r.parked);
        CHECK(restoreAfterCreate(r, out) == 0);   // nothing was parked, nothing to restore
        // The same create with parking allowed still parks both, proving the switch is the
        // only difference between stock-ish behaviour and the experiment.
        CHECK(parkBeforeCreate(s, r, create, sizeof(create), out, true) == 2);
    }

    // --- 10. widenInitiatorScan: the 10 % initiating window becomes the interval, per PHY.
    {
        // Extended create, PHYs 1M|2M, both asking for 0x01e0/0x0030 (300 ms / 30 ms).
        // Payload: 10 fixed bytes + 14 per PHY, so the 2M scan pair sits at 27/29.
        uint8_t ext[42] = {0x43, 0x20, 38, 0x01, 0x00, 0x01, 0,0,0,0,0,0, 0x03,
                           0xe0,0x01, 0x30,0x00, 0x18,0x00, 0x28,0x00, 0x00,0x00, 0x48,0x00, 0x00,0x00,
                           0xe0,0x01, 0x30,0x00, 0x18,0x00, 0x28,0x00, 0x00,0x00, 0x48,0x00, 0x00,0x00};
        uint32_t observed = 0;
        CHECK(widenInitiatorScan(ext, sizeof(ext), &observed));
        CHECK(observed == ((0x01e0u << 16) | 0x0030u));
        CHECK(ext[15] == 0xe0 && ext[16] == 0x01);   // 1M window widened
        CHECK(ext[29] == 0xe0 && ext[30] == 0x01);   // 2M window widened
        CHECK(ext[13] == 0xe0 && ext[14] == 0x01);   // interval untouched
        CHECK(ext[17] == 0x18 && ext[19] == 0x28);   // connection params untouched
        CHECK(ext[31] == 0x18 && ext[33] == 0x28);   // 2M connection params untouched
        // Already continuous: nothing to do.
        uint8_t same[42] = {0x43, 0x20, 38, 0x01, 0x00, 0x01, 0,0,0,0,0,0, 0x01,
                            0xe0,0x01, 0xe0,0x01, 0x18,0x00, 0x28,0x00, 0x00,0x00, 0x48,0x00, 0x00,0x00};
        CHECK(!widenInitiatorScan(same, sizeof(same), &observed));
        CHECK(observed == ((0x01e0u << 16) | 0x01e0u));
        // Legacy create carries the same two fields at the head of its payload.
        uint8_t legacy[16] = {0x0d, 0x20, 0x0c, 0xe0,0x01, 0x30,0x00, 0x00, 0x00, 0,0,0,0,0,0, 0x00};
        CHECK(widenInitiatorScan(legacy, sizeof(legacy), &observed));
        CHECK(legacy[5] == 0xe0 && legacy[6] == 0x01);
        // Anything else is left alone.
        uint8_t scan[5] = {0x42, 0x20, 0x02, 0x01, 0x01};
        CHECK(!widenInitiatorScan(scan, sizeof(scan), &observed));
    }

    // --- 11. Continuity witness: find the 0x0d invite the Mac transmits and the 0x0e answer.
    {
        // Legacy LE Set Advertising Data (0x2008): AD structures start at byte 3.
        // flags, then Apple manufacturer data carrying Continuity type 0x0d (Wi-Fi Settings).
        uint8_t legacyAdv[18] = {0x08, 0x20, 15,
                                 0x02, 0x01, 0x06,
                                 0x0b, 0xff, 0x4c, 0x00, 0x0d, 0x01, 0x67, 0xf3, 0x6d, 0x01, 0x00, 0x00};
        PayloadRange payload {};
        CHECK(payloadRange(legacyAdv, sizeof(legacyAdv), &payload));
        CHECK(payload.offset == 3 && payload.length == 15);
        uint32_t mask = 0;
        CHECK(appleTypeIds(legacyAdv + payload.offset, payload.length, &mask) == 0x0d);
        CHECK(mask == 1u << 0x0d);

        // Extended form (0x2037) adds handle, fragment operation and preference before the data.
        uint8_t extAdv[19] = {0x37, 0x20, 16, 0x00, 0x03, 0x01, 12,
                              0x0b, 0xff, 0x4c, 0x00, 0x0e, 0x06, 0x01, 0x00, 0x5e, 0x00, 0x06, 0x03};
        CHECK(payloadRange(extAdv, sizeof(extAdv), &payload));
        CHECK(payload.offset == 7 && payload.length == 12);
        mask = 0;
        CHECK(appleTypeIds(extAdv + payload.offset, payload.length, &mask) == 0x0e);
        CHECK(mask == 1u << 0x0e);

        // A non-Apple company id in the same field type must not be mistaken for Continuity.
        uint8_t otherCompany[8] = {0x08, 0x20, 5, 0x04, 0xff, 0x06, 0x00, 0x01};
        CHECK(payloadRange(otherCompany, sizeof(otherCompany), &payload));
        mask = 0;
        CHECK(appleTypeIds(otherCompany + payload.offset, payload.length, &mask) == 0xff);
        CHECK(mask == 0);

        // A length byte that claims more than the buffer holds stops the walk, no overrun.
        uint8_t truncated[6] = {0x08, 0x20, 3, 0x09, 0xff, 0x4c};
        mask = 0;
        CHECK(appleTypeIds(truncated + 3, 3, &mask) == 0xff);
        CHECK(mask == 0);

        // A command that is neither advertising-data opcode is rejected outright.
        uint8_t notAdv[8] = {0x0a, 0x20, 5, 0x01, 0x00, 0x00, 0x00, 0x00};
        CHECK(!payloadRange(notAdv, sizeof(notAdv), &payload));

        // Two reports in one LE Meta Event 0x02: a connectable ADV_IND and a phone's 0x0e.
        uint8_t report[39] = {0x3e, 0x00, 0x02, 2,
                              0x00, 0x00, 1, 2, 3, 4, 5, 6, 3, 0x02, 0x01, 0x06, 0xd0,
                              0x03, 0x01, 11, 12, 13, 14, 15, 16, 12,
                              0x0b, 0xff, 0x4c, 0x00, 0x0e, 0x06, 0x01, 0x00, 0x5e, 0x00, 0x06, 0x03,
                              0xc0};
        Report items[4] {};
        CHECK(parseReports(report, sizeof(report), items, 4) == 2);
        CHECK(items[0].eventType == 0x00);          // connectable: ADV_IND
        CHECK(items[0].appleTypes == 0);
        CHECK(items[0].addr[0] == 1 && items[0].rssi == 0xd0);
        CHECK(items[1].eventType == 0x03);          // not connectable: ADV_NONCONN_IND
        CHECK(items[1].appleTypes == 1u << 0x0e);   // the phone answering with a hotspot
        CHECK(items[1].addr[0] == 11 && items[1].rssi == 0xc0);

        // Another subevent, and a buffer that ends mid-report, must both be handled safely.
        uint8_t notReport[6] = {0x3e, 0x02, 0x01, 0x00, 0x01, 0x00};
        CHECK(parseReports(notReport, sizeof(notReport), items, 4) == 0);
        CHECK(parseReports(report, 30, items, 4) == 1);
    }

    // 12: connection-path fingerprint. Every opcode here is one bluetoothd may or may not use,
    // and the whole point is telling them apart, so a wrong mapping silently invents a path
    // that was never taken.
    {
        CHECK(fingerprintOf(0x200d) == FpLegacyCreate);
        CHECK(fingerprintOf(0x2043) == FpExtCreate);
        CHECK(fingerprintOf(0x2010) == FpAcceptClear);
        CHECK(fingerprintOf(0x2011) == FpAcceptAdd);
        CHECK(fingerprintOf(0x2012) == FpAcceptRemove);
        CHECK(fingerprintOf(0x2027) == FpResolvingAdd);
        CHECK(fingerprintOf(0x2028) == FpResolvingRemove);
        CHECK(fingerprintOf(0x2029) == FpResolvingClear);
        CHECK(fingerprintOf(0x202d) == FpResolvingSize);
        CHECK(fingerprintOf(0x200f) == FpAcceptSize);
        CHECK(fingerprintOf(0x2036) == FpExtAdvParams);
        CHECK(fingerprintOf(0x2037) == FpExtAdvData);
        CHECK(fingerprintOf(0x2039) == FpExtAdvEnable);
        CHECK(fingerprintOf(0x2006) == FpAdvParams);
        CHECK(fingerprintOf(0x200a) == FpAdvEnable);
        CHECK(fingerprintOf(0x200c) == FpScanEnable);
        CHECK(fingerprintOf(0x2042) == FpExtScanEnable);
        CHECK(fingerprintOf(0x2005) == FpOwnRandomAddr);
        CHECK(fingerprintOf(0x2035) == FpAdvSetRandomAddr);
        CHECK(fingerprintOf(0x2013) == FpConnUpdate);
        CHECK(fingerprintOf(0x200e) == FpCreateCancel);
        CHECK(fingerprintOf(0x2016) == FpRemoteFeatures);
        // 0x2038 extended scan parameters must not be confused with 0x200b legacy ones.
        CHECK(fingerprintOf(0x2038) == FpExtScanParams);
        CHECK(fingerprintOf(0x200b) == FpScanParams);
        CHECK(fingerprintOf(0x2038) != fingerprintOf(0x200b));
        // Everything outside the fingerprint set stays out of the mask.
        CHECK(fingerprintOf(0x0c03) == 0);
        CHECK(fingerprintOf(0x2001) == 0);
        CHECK(fingerprintOf(0x0000) == 0);
        // No two opcodes may share a bit, or one path would masquerade as another.
        const uint16_t ops[] = {0x200d,0x2043,0x2010,0x2011,0x2012,0x2027,0x2028,0x2029,0x202d,
                                0x200f,0x2036,0x2037,0x2039,0x2006,0x200a,0x200c,0x2042,0x2005,
                                0x2035,0x2013,0x200e,0x2016,0x2038,0x200b};
        uint32_t unionBits = 0;
        unsigned distinct = 0;
        for (uint16_t op : ops) {
            const uint32_t bit = fingerprintOf(op);
            CHECK(bit && !(unionBits & bit));
            if (bit && !(unionBits & bit))
                ++distinct;
            unionBits |= bit;
        }
        CHECK(distinct == sizeof(ops) / sizeof(ops[0]));
    }

    // --- 14. The data-length round, ported from upstream 0.9.2: per-handle tracking, a 2 s expiry,
    // a bounded number of wrapped reads per round. Each of these is a defect that shipped in 0.9.1.
    {
        const uint64_t t0 = 1000000000ULL;   // arbitrary base; only differences matter
        const uint8_t readFeaturesA[5] = {0x16, 0x20, 0x02, 0x34, 0x12};   // handle 0x1234
        const uint8_t readFeaturesB[5] = {0x16, 0x20, 0x02, 0x78, 0x56};   // handle 0x5678
        const uint8_t reset[3] = {0x03, 0x0c, 0x00};

        // Guards: no buffer, too short, not the command, no handle in the payload.
        FeatureBoard s{};
        CHECK(duplicateReads(s, nullptr, 5, t0) == 0);
        CHECK(duplicateReads(s, readFeaturesA, 4, t0) == 0);
        CHECK(duplicateReads(s, reset, sizeof(reset), t0) == 0);
        const uint8_t other[5] = {0x15, 0x20, 0x02, 0x34, 0x12};          // 0x2015, not 0x2016
        CHECK(duplicateReads(s, other, 5, t0) == 0);
        const uint8_t noHandle[3] = {0x16, 0x20, 0x00};
        CHECK(duplicateReads(s, noHandle, 3, t0) == 0);
        CHECK(!roundPending(s));

        // Arming sends two copies and records the handle; a controller start forgets everything.
        CHECK(duplicateReads(s, readFeaturesA, 5, t0) == 2);
        CHECK(roundPending(s));
        CHECK(duplicateReads(s, reset, sizeof(reset), t0) == 0);
        CHECK(!roundPending(s));

        // Per-handle: two links arming at once must not share a stage, or the fake events carry the
        // wrong handle. A's real completion stays untouched, then A's duplicates are rewritten.
        CHECK(duplicateReads(s, readFeaturesA, 5, t0) == 2);
        CHECK(duplicateReads(s, readFeaturesB, 5, t0) == 2);
        // Each event arrives in its own buffer, so every call starts from the controller's bytes.
        const uint8_t realA[12] = {0x3e, 0x0c, 0x04, 0x00, 0x34, 0x12, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46};
        const uint8_t realB[12] = {0x3e, 0x0c, 0x04, 0x00, 0x78, 0x56, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46};
        uint8_t eventA[16], eventB[16];
        memset(eventA, 0xcc, sizeof(eventA)); memcpy(eventA, realA, 12);
        CHECK(forgeEvent(s, eventA, 12) == 12);                 // A's real completion, unchanged
        memset(eventB, 0xcc, sizeof(eventB)); memcpy(eventB, realB, 12);
        CHECK(forgeEvent(s, eventB, 12) == 12);                 // B is still waiting for its own
        memset(eventA, 0xcc, sizeof(eventA)); memcpy(eventA, realA, 12);
        CHECK(forgeEvent(s, eventA, 12) == kPhyUpdateLen);
        CHECK(eventA[2] == 0x0c && eventA[4] == 0x34 && eventA[5] == 0x12);
        memset(eventA, 0xcc, sizeof(eventA)); memcpy(eventA, realA, 12);
        CHECK(forgeEvent(s, eventA, 12) == kDataLengthLen);
        CHECK(eventA[2] == 0x07 && eventA[3] == 0x34 && eventA[4] == 0x12);
        memset(eventA, 0xcc, sizeof(eventA)); memcpy(eventA, realA, 12);
        CHECK(forgeEvent(s, eventA, 12) == 12);                 // A's round is over
        CHECK(roundPending(s));                              // B still has one
        CHECK(duplicateReads(s, reset, sizeof(reset), t0) == 0);
        CHECK(!roundPending(s));

        // A duplicate that can't be sent is forgotten by handle, not globally.
        CHECK(duplicateReads(s, readFeaturesA, 5, t0) == 2);
        CHECK(duplicateReads(s, readFeaturesB, 5, t0) == 2);
        dropDuplicate(s, readFeaturesA, 5);
        CHECK(duplicateReads(s, readFeaturesA, 5, t0) == 2);    // A re-arms (one slot was freed)
        // B's entry is untouched: its real completion still passes through, then rewrites.
        memset(eventB, 0xcc, sizeof(eventB)); memcpy(eventB, realB, 12);
        CHECK(forgeEvent(s, eventB, 12) == 12);
        memset(eventB, 0xcc, sizeof(eventB)); memcpy(eventB, realB, 12);
        CHECK(forgeEvent(s, eventB, 12) == kPhyUpdateLen);
        CHECK(duplicateReads(s, reset, sizeof(reset), t0) == 0);

        // Expiry: an entry whose events never arrive is given up after 2 s, so nothing waits forever.
        CHECK(duplicateReads(s, readFeaturesA, 5, t0) == 2);
        CHECK(takeWrap(s, t0 + 1000000ULL, 64));                  // 1 ms later, still waiting
        CHECK(!takeWrap(s, t0 + kRoundExpiryNs + 1, 64));    // past the expiry
        CHECK(s.expired == 1);
        CHECK(!roundPending(s));
        // The counters survive the controller start that also clears the table.
        const uint32_t expiredBefore = s.expired;
        CHECK(duplicateReads(s, reset, sizeof(reset), t0) == 0);
        CHECK(s.expired == expiredBefore);

        // Bounded wrapping: a round that never matches may not keep every read of the event pipe
        // wrapped. After the budget, the round is dropped instead of hijacking the pipe forever.
        FeatureBoard w{};
        CHECK(duplicateReads(w, readFeaturesA, 5, t0) == 2);
        CHECK(!takeWrap(w, t0, 5));                               // buffer can't hold the event
        unsigned wrapped = 0;
        while (takeWrap(w, t0, 64))
            ++wrapped;
        CHECK(wrapped == kWrapBudget);
        CHECK(w.aborts == 1);
        CHECK(!roundPending(w));
        CHECK(!takeWrap(w, t0, 64));                              // nothing left to wrap for

        // A new round restores the budget, so the second link isn't starved by the first one.
        CHECK(duplicateReads(w, readFeaturesB, 5, t0) == 2);
        CHECK(takeWrap(w, t0, 64));
        CHECK(w.wrapped == 1);

        // Every slot busy: no copies are sent, so that link behaves as if the fix weren't there
        // rather than overwriting another link's state.
        FeatureBoard full{};
        for (unsigned i = 0; i < kFeatureSlots; ++i) {
            const uint8_t cmd[5] = {0x16, 0x20, 0x02, uint8_t(i), 0x00};
            CHECK(duplicateReads(full, cmd, 5, t0) == 2);
        }
        const uint8_t ninth[5] = {0x16, 0x20, 0x02, 0x7f, 0x00};
        CHECK(duplicateReads(full, ninth, 5, t0) == 0);

        // Events for a link nobody is waiting for pass through byte for byte.
        uint8_t stranger[16] = {0x3e, 0x0c, 0x04, 0x00, 0xaa, 0xbb, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
        uint8_t copy[16];
        memcpy(copy, stranger, sizeof(copy));
        CHECK(forgeEvent(full, stranger, 12) == 12);
        CHECK(memcmp(stranger, copy, sizeof(copy)) == 0);
    }

    // --- 9. The legacy seed is owed once per boot, and only after the controller accepted it.
    {
        Mirror s = baseState();
        const uint8_t scanParams[10] = {0x0b, 0x20, 0x07, 0x01, 0x10, 0x00, 0x10, 0x00, 0x00, 0x00};
        const uint8_t advEnable[4] = {0x0a, 0x20, 0x01, 0x01};
        CHECK(wantsSeedLegacyAddress(s, scanParams, sizeof(scanParams)));
        CHECK(!wantsSeedLegacyAddress(s, advEnable, sizeof(advEnable)));
        // Asking is not a promise: a send that failed has to leave the debt unpaid.
        CHECK(wantsSeedLegacyAddress(s, scanParams, sizeof(scanParams)));
        noteSeedAccepted(s);
        CHECK(!wantsSeedLegacyAddress(s, scanParams, sizeof(scanParams)));
        // A controller reset re-arms it, the same way the host starts over.
        const uint8_t reset[3] = {0x03, 0x0c, 0x00};
        mirrorCommand(s, reset, sizeof(reset), true);
        CHECK(wantsSeedLegacyAddress(s, scanParams, sizeof(scanParams)));
    }

    if (failures) {
        printf("%d check(s) FAILED\n", failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
