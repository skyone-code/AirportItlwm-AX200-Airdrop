// Local, always-present fixed-channel schedule serialization. This is not peer
// synchronization or a transmit timer. Timing semantics cross-checked against
// seemoo-lab/owl src/sync.c and src/tx.c; independently implemented here.
#pragma once
#include <AwSyncPrefix.hpp>
#include <AwChannels.hpp>

namespace Aw {
// macOS states its own AWDL timing in setAWDL_SYNC_PARAMS, in TU. Zero means the field was not stated and
// keeps whatever the caller had. The built-in 16/110 pair below is a capture constant, not a plan: the
// period drives the AW counter and remainingAW, so substituting macOS's period is what "follow the plan"
// actually means on the wire.
struct RequestedTiming {
    uint16_t awLength, awPeriod, extensionLength, syncPeriod;
};

// Clamp instead of reject: a refused sync frame costs the link outright, while a clamped one only costs
// precision. Order matters — the period is installed first because the length is checked against it.
// Returns true when at least one field came from `t`.
inline bool applyRequestedTiming(SyncPrefix &p, const RequestedTiming &t)
{
    bool taken = false;
    if (t.awPeriod >= 8 && t.awPeriod <= 16383) { p.awPeriod = t.awPeriod; taken = true; }
    if (t.syncPeriod >= 8 && t.syncPeriod <= 16383) { p.afPeriod = t.syncPeriod; taken = true; }
    if (t.awLength && t.awLength <= p.awPeriod) { p.commonLength = t.awLength; taken = true; }
    if (t.extensionLength && t.extensionLength <= 16383) { p.extensionLength = t.extensionLength; taken = true; }
    // macOS may state a period without stating a window; keeping the old length would then announce a
    // window longer than its own period, which the encoder below rejects outright.
    if (p.commonLength > p.awPeriod) p.commonLength = p.awPeriod;
    return taken;
}

// Caller supplies local identity, explicit flags/periods and one monotonic
// epoch. Never derive our identity or schedule by replaying a received frame.
// The 73-byte payload includes the known prefix, presence/counter fields,
// sixteen identical legacy slots and two zero padding bytes. Channel 149 uses the
// 5 GHz form of channel 6's 20 MHz flags: 0x2b has the 2.4 GHz band bit, 0x1b the
// 5 GHz one, as peers' own 5 GHz entries (0x1d) show. Inferred, not from a document.
//
// The sixteen channel-sequence slots stay identical on purpose. Their per-slot "not present" encoding is
// not documented, and a wrong byte there is read as garbage by every peer in range, whereas a wrong duty
// cycle is only wrong numerically and is already carried by commonLength/awPeriod. A fixed single-channel
// test has nothing to say per slot, so it names the one channel sixteen times.
inline BuildResult buildLocalFixedSchedule(const SyncPrefix &settings,
        uint64_t epochUS, uint64_t nowUS, uint16_t initialAW,
        uint8_t *output, size_t available, size_t *written)
{
    if (!output || !written || nowUS < epochUS || (initialAW & 3) ||
        !fixedScheduleChannel(settings.nextChannel) ||
        settings.masterChannel != settings.nextChannel ||
        !settings.awPeriod || settings.awPeriod > 16383 ||
        settings.commonLength > settings.awPeriod)
        return BuildResult::Invalid;
    if (available < 73) return BuildResult::NoSpace;
    const uint64_t elapsedTU = (nowUS - epochUS) / 1024;
    const uint32_t extendedTU = uint32_t(settings.awPeriod) * 4;
    const uint32_t phaseTU = uint32_t(elapsedTU % extendedTU);
    const uint16_t counter = uint16_t((elapsedTU / settings.awPeriod + initialAW) & 0xffff);
    SyncPrefix p = settings;
    p.txDownCounter = uint16_t(extendedTU - phaseTU);
    p.remainingAW = phaseTU < p.commonLength ? uint16_t(p.commonLength - phaseTU) : 0;
    uint8_t payload[73] = {};
    size_t prefixLength;
    const auto result = buildSyncPrefix(p, payload, sizeof(payload), &prefixLength);
    if (result != BuildResult::Ready) return result;
    payload[27] = 4;
    write16(payload+29, counter);
    // No infrastructure AP is associated in this monitor test: alignment 0.
    payload[33] = 15; payload[34] = 1; payload[36] = 3;
    payload[37] = 0xff; payload[38] = 0xff;
    for (unsigned i = 0; i < 16; ++i) {
        payload[39+2*i] = legacySlotFlags(settings.nextChannel);
        payload[40+2*i] = settings.nextChannel;
    }
    for (unsigned i = 0; i < 73; ++i) output[i] = payload[i];
    *written = 73;
    return BuildResult::Ready;
}
} // namespace Aw
