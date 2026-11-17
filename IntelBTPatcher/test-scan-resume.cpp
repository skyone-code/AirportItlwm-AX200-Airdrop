// Host tests for the scan-park resume logic in IbtpHci.hpp (pure logic, no kernel types).
// Build & run:  clang++ -std=c++17 -Wall -Wextra -I. -o /tmp/test-scan-resume test-scan-resume.cpp && /tmp/test-scan-resume
#include "IntelBTPatcherHci.hpp"
#include <cstdio>
#include <cstring>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

using namespace Ibtp;

static State baseState() {
    State s{};
    s.extScanEnable = 1; s.extScanFilterDup = 1;
    s.extAdvEnabled = true;
    return s;
}

// Simulates bluetoothd enabling extended scanning (0x2042) and one advertising set (0x2039),
// through the mirror, so extAdvReplay holds the verbatim packet.
static void hostEnablesAll(State &s, ScanResumeState &r) {
    uint8_t scan[5] = {0x42, 0x20, 0x02, 0x01, 0x01};
    PreCommand out[4] = {};
    CHECK(wantsPreScanOff(s, r, scan, sizeof(scan), out) == 0);
    CHECK(s.extScanEnable == 1);
    uint8_t adv[10] = {0x39, 0x20, 0x07, 0x01, 0x01, 0x00, 0x02, 0x00, 0x7f, 0x00};
    CHECK(wantsPreScanOff(s, r, adv, sizeof(adv), out) == 0);
    CHECK(s.extAdvEnabled);
}

static int tickCommand(State &s, ScanResumeState &r) {
    // Any non-create command passes through wantsPreScanOff (no-op) and wantsScanResume.
    uint8_t cmd[4] = {0x05, 0x20, 0x01, 0x01};
    PreCommand out[4] = {};
    wantsPreScanOff(s, r, cmd, sizeof(cmd), out);
    return wantsScanResume(r, out);
}

int main() {
    // --- 1. Park on an extended create: ext scan + ext adv both switched off, verbatim replay kept.
    {
        State s = baseState();
        ScanResumeState r{};
        hostEnablesAll(s, r);
        uint8_t create[11] = {0x43, 0x20, 0x08, 0x00, 0x01, 1,2,3,4,5,6};
        PreCommand out[4] = {};
        const int n = wantsPreScanOff(s, r, create, sizeof(create), out);
        CHECK(n == 2);                       // ext scan off + ext adv off
        CHECK(r.suspended && !r.resumePending);
        CHECK(r.pendingOpcode == kOpLeExtCreateConn);
        CHECK(r.parkedExtScan && r.parkedExtAdv);
        CHECK(!r.parkedLegacyScan && !r.parkedLegacyAdv);
        CHECK(s.extScanEnable == 0 && !s.extAdvEnabled);
        CHECK(r.extAdvReplayLength == 10 && r.extAdvReplay[0] == 0x39 && r.extAdvReplay[2] == 0x07);
    }

    // --- 2. Command Status (0x0f) with an error for the create ends the attempt.
    {
        State s = baseState();
        ScanResumeState r{};
        hostEnablesAll(s, r);
        uint8_t create[11] = {0x43, 0x20, 0x08, 0x00, 0x01, 1,2,3,4,5,6};
        PreCommand out[4] = {};
        CHECK(wantsPreScanOff(s, r, create, sizeof(create), out) == 2);
        // Command Status: 0x0f, len 4, status 0x0C, numPackets 1, opcode 0x2043.
        uint8_t evt[6] = {0x0f, 0x04, 0x0C, 0x01, 0x43, 0x20};
        onScanResumeEvent(r, evt, sizeof(evt));
        CHECK(r.resumePending);
        const int n = wantsScanResume(r, out);
        CHECK(n == 2);
        CHECK(!r.suspended && !r.resumePending && r.pendingOpcode == 0);
        CHECK(out[0].data[0] == 0x42 && out[0].data[3] == 1 && out[0].data[4] == 1);  // ext scan restored
        CHECK(out[1].data[0] == 0x39 && out[1].length == 10);                          // adv replay verbatim
    }

    // --- 3. Command Status success (0x00) for the create does NOT end the attempt.
    {
        State s = baseState();
        ScanResumeState r{};
        hostEnablesAll(s, r);
        uint8_t create[11] = {0x43, 0x20, 0x08, 0x00, 0x01, 1,2,3,4,5,6};
        PreCommand out[4] = {};
        wantsPreScanOff(s, r, create, sizeof(create), out);
        uint8_t evt[6] = {0x0f, 0x04, 0x00, 0x01, 0x43, 0x20};
        onScanResumeEvent(r, evt, sizeof(evt));
        CHECK(!r.resumePending);
        // ...and a Command Complete error for the same opcode does.
        uint8_t evt2[7] = {0x0e, 0x05, 0x01, 0x43, 0x20, 0x12, 0x00};
        onScanResumeEvent(r, evt2, sizeof(evt2));
        CHECK(r.resumePending);
    }

    // --- 4. Create Connection Cancel (0x200e) ends the attempt from either command event form.
    {
        State s = baseState();
        ScanResumeState r{};
        hostEnablesAll(s, r);
        uint8_t create[11] = {0x43, 0x20, 0x08, 0x00, 0x01, 1,2,3,4,5,6};
        PreCommand out[4] = {};
        wantsPreScanOff(s, r, create, sizeof(create), out);
        uint8_t cancelStatus[6] = {0x0f, 0x04, 0x00, 0x01, 0x0e, 0x20};
        onScanResumeEvent(r, cancelStatus, sizeof(cancelStatus));
        CHECK(r.resumePending);
        wantsScanResume(r, out);
        CHECK(!r.suspended);

        // Again, via the Command Complete form.
        ScanResumeState r2{};
        hostEnablesAll(s, r2);
        wantsPreScanOff(s, r2, create, sizeof(create), out);
        uint8_t cancelComplete[7] = {0x0e, 0x05, 0x01, 0x0e, 0x20, 0x02, 0x00};
        onScanResumeEvent(r2, cancelComplete, sizeof(cancelComplete));
        CHECK(r2.resumePending);
    }

    // --- 5. LE meta conn complete still ends the attempt (regression).
    {
        State s = baseState();
        ScanResumeState r{};
        hostEnablesAll(s, r);
        uint8_t create[11] = {0x43, 0x20, 0x08, 0x00, 0x01, 1,2,3,4,5,6};
        PreCommand out[4] = {};
        wantsPreScanOff(s, r, create, sizeof(create), out);
        uint8_t meta[8] = {0x3e, 0x06, 0x01, 0x00, 1,2,3,4};
        onScanResumeEvent(r, meta, sizeof(meta));
        CHECK(r.resumePending);
    }

    // --- 6. Watchdog: an attempt whose events never come is force-resumed.
    {
        State s = baseState();
        ScanResumeState r{};
        hostEnablesAll(s, r);
        uint8_t create[11] = {0x43, 0x20, 0x08, 0x00, 0x01, 1,2,3,4,5,6};
        PreCommand out[4] = {};
        CHECK(wantsPreScanOff(s, r, create, sizeof(create), out) == 2);
        for (uint32_t i = 0; i < kResumeStallCommands - 1; ++i)
            CHECK(tickCommand(s, r) == 0);        // one short of the limit: nothing yet
        CHECK(r.suspended && !r.resumePending);
        const int n = tickCommand(s, r);          // the 128th command fires the watchdog
        CHECK(n == 2 && r.watchdogFired);
        CHECK(!r.suspended && r.stallGuard == 0);
        CHECK(r.resumePending == false);
        // A late error event after the resume is harmless: nothing is suspended any more.
        uint8_t evt[6] = {0x0f, 0x04, 0x0C, 0x01, 0x43, 0x20};
        onScanResumeEvent(r, evt, sizeof(evt));
        CHECK(!r.resumePending);
    }

    // --- 7. Host re-asserting ext scan while suspended: resume restores only the advertising.
    {
        State s = baseState();
        ScanResumeState r{};
        hostEnablesAll(s, r);
        uint8_t create[11] = {0x43, 0x20, 0x08, 0x00, 0x01, 1,2,3,4,5,6};
        PreCommand out[4] = {};
        CHECK(wantsPreScanOff(s, r, create, sizeof(create), out) == 2);
        uint8_t rescan[5] = {0x42, 0x20, 0x02, 0x01, 0x01};
        CHECK(wantsPreScanOff(s, r, rescan, sizeof(rescan), out) == 0);
        CHECK(!r.parkedExtScan);              // host took over scanning again
        CHECK(s.extScanEnable == 1);
        uint8_t evt[6] = {0x0f, 0x04, 0x0C, 0x01, 0x43, 0x20};
        onScanResumeEvent(r, evt, sizeof(evt));
        const int n = wantsScanResume(r, out);
        CHECK(n == 1);                        // only the advertising replay
        CHECK(out[0].data[0] == 0x39);
        CHECK(!r.suspended);
    }

    // --- 8. Legacy create parks only what legacy code had enabled.
    {
        State s = State{};
        s.scanEnable = 1; s.scanFilterDup = 1;
        ScanResumeState r{};
        uint8_t create[15] = {0x0d, 0x20, 0x0c, 0x20,0x00,0x20,0x00, 0x00, 0x01, 1,2,3,4,5,6};
        PreCommand out[4] = {};
        const int n = wantsPreScanOff(s, r, create, sizeof(create), out);
        CHECK(n == 1);
        CHECK(r.pendingOpcode == kOpLeCreateConn);
        CHECK(r.parkedLegacyScan && !r.parkedExtScan && !r.parkedExtAdv);
        uint8_t evt[6] = {0x0f, 0x04, 0x0C, 0x01, 0x0d, 0x20};
        onScanResumeEvent(r, evt, sizeof(evt));
        const int n2 = wantsScanResume(r, out);
        CHECK(n2 == 1 && out[0].data[0] == 0x0c && out[0].data[4] == 1);
    }

    // --- 9. allowPark=false (2.7.9 default): no park at all, but the mirrors still follow the
    // host's enables, so the witness and any later opt-in park stay correct.
    {
        State s = State{};
        ScanResumeState r{};
        uint8_t extScanOn[5] = {0x42, 0x20, 0x02, 0x01, 0x01};
        PreCommand out[4] = {};
        CHECK(wantsPreScanOff(s, r, extScanOn, sizeof(extScanOn), out, false) == 0);
        CHECK(s.extScanEnable == 1);
        uint8_t extAdvOn[10] = {0x39, 0x20, 0x07, 0x01, 0x00, 0x00, 0x02, 0x00, 0x7f, 0x00};
        CHECK(wantsPreScanOff(s, r, extAdvOn, sizeof(extAdvOn), out, false) == 0);
        CHECK(s.extAdvEnabled);
        uint8_t create[11] = {0x43, 0x20, 0x08, 0x00, 0x01, 1,2,3,4,5,6};
        CHECK(wantsPreScanOff(s, r, create, sizeof(create), out, false) == 0);
        CHECK(!r.suspended);
        CHECK(wantsScanResume(r, out) == 0);   // nothing was parked, nothing to restore
        // The same create with parking allowed still parks both, proving the switch is the
        // only difference between stock-ish behaviour and the experiment.
        CHECK(wantsPreScanOff(s, r, create, sizeof(create), out, true) == 2);
    }

    // --- 10. widenCreateScan: the 10 % initiating window becomes the interval, per PHY.
    {
        // Extended create, PHYs 1M|2M, both asking for 0x01e0/0x0030 (300 ms / 30 ms).
        // Payload: 10 fixed bytes + 14 per PHY, so the 2M scan pair sits at 27/29.
        uint8_t ext[42] = {0x43, 0x20, 38, 0x01, 0x00, 0x01, 0,0,0,0,0,0, 0x03,
                           0xe0,0x01, 0x30,0x00, 0x18,0x00, 0x28,0x00, 0x00,0x00, 0x48,0x00, 0x00,0x00,
                           0xe0,0x01, 0x30,0x00, 0x18,0x00, 0x28,0x00, 0x00,0x00, 0x48,0x00, 0x00,0x00};
        uint32_t observed = 0;
        CHECK(widenCreateScan(ext, sizeof(ext), &observed));
        CHECK(observed == ((0x01e0u << 16) | 0x0030u));
        CHECK(ext[15] == 0xe0 && ext[16] == 0x01);   // 1M window widened
        CHECK(ext[29] == 0xe0 && ext[30] == 0x01);   // 2M window widened
        CHECK(ext[13] == 0xe0 && ext[14] == 0x01);   // interval untouched
        CHECK(ext[17] == 0x18 && ext[19] == 0x28);   // connection params untouched
        CHECK(ext[31] == 0x18 && ext[33] == 0x28);   // 2M connection params untouched
        // Already continuous: nothing to do.
        uint8_t same[42] = {0x43, 0x20, 38, 0x01, 0x00, 0x01, 0,0,0,0,0,0, 0x01,
                            0xe0,0x01, 0xe0,0x01, 0x18,0x00, 0x28,0x00, 0x00,0x00, 0x48,0x00, 0x00,0x00};
        CHECK(!widenCreateScan(same, sizeof(same), &observed));
        CHECK(observed == ((0x01e0u << 16) | 0x01e0u));
        // Legacy create carries the same two fields at the head of its payload.
        uint8_t legacy[16] = {0x0d, 0x20, 0x0c, 0xe0,0x01, 0x30,0x00, 0x00, 0x00, 0,0,0,0,0,0, 0x00};
        CHECK(widenCreateScan(legacy, sizeof(legacy), &observed));
        CHECK(legacy[5] == 0xe0 && legacy[6] == 0x01);
        // Anything else is left alone.
        uint8_t scan[5] = {0x42, 0x20, 0x02, 0x01, 0x01};
        CHECK(!widenCreateScan(scan, sizeof(scan), &observed));
    }

    // --- 11. Continuity witness: find the 0x0d invite the Mac transmits and the 0x0e answer.
    {
        // Legacy LE Set Advertising Data (0x2008): AD structures start at byte 3.
        // flags, then Apple manufacturer data carrying Continuity type 0x0d (Wi-Fi Settings).
        uint8_t legacyAdv[18] = {0x08, 0x20, 15,
                                 0x02, 0x01, 0x06,
                                 0x0b, 0xff, 0x4c, 0x00, 0x0d, 0x01, 0x67, 0xf3, 0x6d, 0x01, 0x00, 0x00};
        AdvPayload payload {};
        CHECK(advPayload(legacyAdv, sizeof(legacyAdv), &payload));
        CHECK(payload.offset == 3 && payload.length == 15);
        uint32_t mask = 0;
        CHECK(continuityTypes(legacyAdv + payload.offset, payload.length, &mask) == 0x0d);
        CHECK(mask == 1u << 0x0d);

        // Extended form (0x2037) adds handle, fragment operation and preference before the data.
        uint8_t extAdv[19] = {0x37, 0x20, 16, 0x00, 0x03, 0x01, 12,
                              0x0b, 0xff, 0x4c, 0x00, 0x0e, 0x06, 0x01, 0x00, 0x5e, 0x00, 0x06, 0x03};
        CHECK(advPayload(extAdv, sizeof(extAdv), &payload));
        CHECK(payload.offset == 7 && payload.length == 12);
        mask = 0;
        CHECK(continuityTypes(extAdv + payload.offset, payload.length, &mask) == 0x0e);
        CHECK(mask == 1u << 0x0e);

        // A non-Apple company id in the same field type must not be mistaken for Continuity.
        uint8_t otherCompany[8] = {0x08, 0x20, 5, 0x04, 0xff, 0x06, 0x00, 0x01};
        CHECK(advPayload(otherCompany, sizeof(otherCompany), &payload));
        mask = 0;
        CHECK(continuityTypes(otherCompany + payload.offset, payload.length, &mask) == 0xff);
        CHECK(mask == 0);

        // A length byte that claims more than the buffer holds stops the walk, no overrun.
        uint8_t truncated[6] = {0x08, 0x20, 3, 0x09, 0xff, 0x4c};
        mask = 0;
        CHECK(continuityTypes(truncated + 3, 3, &mask) == 0xff);
        CHECK(mask == 0);

        // A command that is neither advertising-data opcode is rejected outright.
        uint8_t notAdv[8] = {0x0a, 0x20, 5, 0x01, 0x00, 0x00, 0x00, 0x00};
        CHECK(!advPayload(notAdv, sizeof(notAdv), &payload));

        // Two reports in one LE Meta Event 0x02: a connectable ADV_IND and a phone's 0x0e.
        uint8_t report[39] = {0x3e, 0x00, 0x02, 2,
                              0x00, 0x00, 1, 2, 3, 4, 5, 6, 3, 0x02, 0x01, 0x06, 0xd0,
                              0x03, 0x01, 11, 12, 13, 14, 15, 16, 12,
                              0x0b, 0xff, 0x4c, 0x00, 0x0e, 0x06, 0x01, 0x00, 0x5e, 0x00, 0x06, 0x03,
                              0xc0};
        AdvReportItem items[4] {};
        CHECK(advReportItems(report, sizeof(report), items, 4) == 2);
        CHECK(items[0].eventType == 0x00);          // connectable: ADV_IND
        CHECK(items[0].appleTypes == 0);
        CHECK(items[0].addr[0] == 1 && items[0].rssi == 0xd0);
        CHECK(items[1].eventType == 0x03);          // not connectable: ADV_NONCONN_IND
        CHECK(items[1].appleTypes == 1u << 0x0e);   // the phone answering with a hotspot
        CHECK(items[1].addr[0] == 11 && items[1].rssi == 0xc0);

        // Another subevent, and a buffer that ends mid-report, must both be handled safely.
        uint8_t notReport[6] = {0x3e, 0x02, 0x01, 0x00, 0x01, 0x00};
        CHECK(advReportItems(notReport, sizeof(notReport), items, 4) == 0);
        CHECK(advReportItems(report, 30, items, 4) == 1);
    }

    // 12: connection-path fingerprint. Every opcode here is one bluetoothd may or may not use,
    // and the whole point is telling them apart, so a wrong mapping silently invents a path
    // that was never taken.
    {
        CHECK(opFingerprint(0x200d) == kFpLegacyCreate);
        CHECK(opFingerprint(0x2043) == kFpExtCreate);
        CHECK(opFingerprint(0x2010) == kFpAcceptListClear);
        CHECK(opFingerprint(0x2011) == kFpAcceptListAdd);
        CHECK(opFingerprint(0x2012) == kFpAcceptListRemove);
        CHECK(opFingerprint(0x2027) == kFpResolvingAdd);
        CHECK(opFingerprint(0x2028) == kFpResolvingRemove);
        CHECK(opFingerprint(0x2029) == kFpResolvingClear);
        CHECK(opFingerprint(0x202d) == kFpResolvingSize);
        CHECK(opFingerprint(0x200f) == kFpAcceptListSize);
        CHECK(opFingerprint(0x2036) == kFpExtAdvParams);
        CHECK(opFingerprint(0x2037) == kFpExtAdvData);
        CHECK(opFingerprint(0x2039) == kFpExtAdvEnable);
        CHECK(opFingerprint(0x2006) == kFpAdvParams);
        CHECK(opFingerprint(0x200a) == kFpAdvEnable);
        CHECK(opFingerprint(0x200c) == kFpScanEnable);
        CHECK(opFingerprint(0x2042) == kFpExtScanEnable);
        CHECK(opFingerprint(0x2005) == kFpRandomAddress);
        CHECK(opFingerprint(0x2035) == kFpAdvSetRandomAddress);
        CHECK(opFingerprint(0x2013) == kFpConnUpdate);
        CHECK(opFingerprint(0x200e) == kFpCreateCancel);
        CHECK(opFingerprint(0x2016) == kFpReadRemoteFeatures);
        // 0x2038 extended scan parameters must not be confused with 0x200b legacy ones.
        CHECK(opFingerprint(0x2038) == kFpExtScanParams);
        CHECK(opFingerprint(0x200b) == kFpScanParams);
        CHECK(opFingerprint(0x2038) != opFingerprint(0x200b));
        // Everything outside the fingerprint set stays out of the mask.
        CHECK(opFingerprint(0x0c03) == 0);
        CHECK(opFingerprint(0x2001) == 0);
        CHECK(opFingerprint(0x0000) == 0);
        // No two opcodes may share a bit, or one path would masquerade as another.
        const uint16_t ops[] = {0x200d,0x2043,0x2010,0x2011,0x2012,0x2027,0x2028,0x2029,0x202d,
                                0x200f,0x2036,0x2037,0x2039,0x2006,0x200a,0x200c,0x2042,0x2005,
                                0x2035,0x2013,0x200e,0x2016,0x2038,0x200b};
        uint32_t unionBits = 0;
        unsigned distinct = 0;
        for (uint16_t op : ops) {
            const uint32_t bit = opFingerprint(op);
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
        DataLengthState s{};
        CHECK(extraRemoteReads(s, nullptr, 5, t0) == 0);
        CHECK(extraRemoteReads(s, readFeaturesA, 4, t0) == 0);
        CHECK(extraRemoteReads(s, reset, sizeof(reset), t0) == 0);
        const uint8_t other[5] = {0x15, 0x20, 0x02, 0x34, 0x12};          // 0x2015, not 0x2016
        CHECK(extraRemoteReads(s, other, 5, t0) == 0);
        const uint8_t noHandle[3] = {0x16, 0x20, 0x00};
        CHECK(extraRemoteReads(s, noHandle, 3, t0) == 0);
        CHECK(!dataLengthPending(s));

        // Arming sends two copies and records the handle; a controller start forgets everything.
        CHECK(extraRemoteReads(s, readFeaturesA, 5, t0) == 2);
        CHECK(dataLengthPending(s));
        CHECK(extraRemoteReads(s, reset, sizeof(reset), t0) == 0);
        CHECK(!dataLengthPending(s));

        // Per-handle: two links arming at once must not share a stage, or the fake events carry the
        // wrong handle. A's real completion stays untouched, then A's duplicates are rewritten.
        CHECK(extraRemoteReads(s, readFeaturesA, 5, t0) == 2);
        CHECK(extraRemoteReads(s, readFeaturesB, 5, t0) == 2);
        // Each event arrives in its own buffer, so every call starts from the controller's bytes.
        const uint8_t realA[12] = {0x3e, 0x0c, 0x04, 0x00, 0x34, 0x12, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46};
        const uint8_t realB[12] = {0x3e, 0x0c, 0x04, 0x00, 0x78, 0x56, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46};
        uint8_t eventA[16], eventB[16];
        memset(eventA, 0xcc, sizeof(eventA)); memcpy(eventA, realA, 12);
        CHECK(rewriteEvent(s, eventA, 12) == 12);                 // A's real completion, unchanged
        memset(eventB, 0xcc, sizeof(eventB)); memcpy(eventB, realB, 12);
        CHECK(rewriteEvent(s, eventB, 12) == 12);                 // B is still waiting for its own
        memset(eventA, 0xcc, sizeof(eventA)); memcpy(eventA, realA, 12);
        CHECK(rewriteEvent(s, eventA, 12) == kPhyUpdateEventLength);
        CHECK(eventA[2] == 0x0c && eventA[4] == 0x34 && eventA[5] == 0x12);
        memset(eventA, 0xcc, sizeof(eventA)); memcpy(eventA, realA, 12);
        CHECK(rewriteEvent(s, eventA, 12) == kDataLengthEventLength);
        CHECK(eventA[2] == 0x07 && eventA[3] == 0x34 && eventA[4] == 0x12);
        memset(eventA, 0xcc, sizeof(eventA)); memcpy(eventA, realA, 12);
        CHECK(rewriteEvent(s, eventA, 12) == 12);                 // A's round is over
        CHECK(dataLengthPending(s));                              // B still has one
        CHECK(extraRemoteReads(s, reset, sizeof(reset), t0) == 0);
        CHECK(!dataLengthPending(s));

        // A duplicate that can't be sent is forgotten by handle, not globally.
        CHECK(extraRemoteReads(s, readFeaturesA, 5, t0) == 2);
        CHECK(extraRemoteReads(s, readFeaturesB, 5, t0) == 2);
        forgetRemoteRead(s, readFeaturesA, 5);
        CHECK(extraRemoteReads(s, readFeaturesA, 5, t0) == 2);    // A re-arms (one slot was freed)
        // B's entry is untouched: its real completion still passes through, then rewrites.
        memset(eventB, 0xcc, sizeof(eventB)); memcpy(eventB, realB, 12);
        CHECK(rewriteEvent(s, eventB, 12) == 12);
        memset(eventB, 0xcc, sizeof(eventB)); memcpy(eventB, realB, 12);
        CHECK(rewriteEvent(s, eventB, 12) == kPhyUpdateEventLength);
        CHECK(extraRemoteReads(s, reset, sizeof(reset), t0) == 0);

        // Expiry: an entry whose events never arrive is given up after 2 s, so nothing waits forever.
        CHECK(extraRemoteReads(s, readFeaturesA, 5, t0) == 2);
        CHECK(wrapRead(s, t0 + 1000000ULL, 64));                  // 1 ms later, still waiting
        CHECK(!wrapRead(s, t0 + kDataLengthExpiryNs + 1, 64));    // past the expiry
        CHECK(s.expired == 1);
        CHECK(!dataLengthPending(s));
        // The counters survive the controller start that also clears the table.
        const uint32_t expiredBefore = s.expired;
        CHECK(extraRemoteReads(s, reset, sizeof(reset), t0) == 0);
        CHECK(s.expired == expiredBefore);

        // Bounded wrapping: a round that never matches may not keep every read of the event pipe
        // wrapped. After the budget, the round is dropped instead of hijacking the pipe forever.
        DataLengthState w{};
        CHECK(extraRemoteReads(w, readFeaturesA, 5, t0) == 2);
        CHECK(!wrapRead(w, t0, 5));                               // buffer can't hold the event
        unsigned wrapped = 0;
        while (wrapRead(w, t0, 64))
            ++wrapped;
        CHECK(wrapped == kMaxWrappedReads);
        CHECK(w.aborts == 1);
        CHECK(!dataLengthPending(w));
        CHECK(!wrapRead(w, t0, 64));                              // nothing left to wrap for

        // A new round restores the budget, so the second link isn't starved by the first one.
        CHECK(extraRemoteReads(w, readFeaturesB, 5, t0) == 2);
        CHECK(wrapRead(w, t0, 64));
        CHECK(w.wrapped == 1);

        // Every slot busy: no copies are sent, so that link behaves as if the fix weren't there
        // rather than overwriting another link's state.
        DataLengthState full{};
        for (unsigned i = 0; i < kDataLengthLinks; ++i) {
            const uint8_t cmd[5] = {0x16, 0x20, 0x02, uint8_t(i), 0x00};
            CHECK(extraRemoteReads(full, cmd, 5, t0) == 2);
        }
        const uint8_t ninth[5] = {0x16, 0x20, 0x02, 0x7f, 0x00};
        CHECK(extraRemoteReads(full, ninth, 5, t0) == 0);

        // Events for a link nobody is waiting for pass through byte for byte.
        uint8_t stranger[16] = {0x3e, 0x0c, 0x04, 0x00, 0xaa, 0xbb, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
        uint8_t copy[16];
        memcpy(copy, stranger, sizeof(copy));
        CHECK(rewriteEvent(full, stranger, 12) == 12);
        CHECK(memcmp(stranger, copy, sizeof(copy)) == 0);
    }

    if (failures) {
        printf("%d check(s) FAILED\n", failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
