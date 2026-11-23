// Disabled-state behavior shared by the kernel interface and host tests.
#pragma once
#include <IOKit/IOReturn.h>

inline IOReturn awSetInactiveState(const apple80211_awdl_sync_state *data)
{
    if (!data || data->version > APPLE80211_VERSION || data->state < 1 || data->state > 3)
        return kIOReturnBadArgument;
    // Native configure uses state 1 for DISABLE. The inactive transport can
    // acknowledge that idempotently; other states need real radio support.
    return data->state == 1 ? kIOReturnSuccess : kIOReturnUnsupported;
}

inline IOReturn awGetInactiveState(apple80211_awdl_sync_state *data)
{
    if (!data)
        return kIOReturnBadArgument;
    *data = {};
    data->version = APPLE80211_VERSION;
    data->state = 1;
    return kIOReturnSuccess;
}

// What Apple's peer manager has asked of the sync machinery. With accept off, this
// behaves exactly as the inactive functions above. With accept on, it records the
// request and reports it back so reads and writes agree. Accepting does NOT mean
// this driver synchronizes, elects or hops: this only lets the native stack stay up.
//
// Master alignment (0.7-equivalent): states 2 and 3 are the peer manager announcing
// the elected master; the Tahoe struct carries its address at +8. There is no election
// algorithm on our side at all - the master is whatever macOS programmed, and a zero
// address or our own address means we are it. The frame writer decides what goes on
// the air with this.
class AwNativeNativeState {
public:
    bool accept = false;
    IOReturn setEnabled(const apple80211_awdl_sync_enabled *data) {
        if (!data || data->version > APPLE80211_VERSION || data->enabled > 1)
            return kIOReturnBadArgument;
        if (data->enabled && !accept)
            return kIOReturnUnsupported;
        enabled = data->enabled != 0;
        return kIOReturnSuccess;
    }
    IOReturn getEnabled(apple80211_awdl_sync_enabled *data) const {
        if (!data)
            return kIOReturnBadArgument;
        *data = {};
        data->version = APPLE80211_VERSION;
        data->enabled = enabled ? 1 : 0;
        return kIOReturnSuccess;
    }
    IOReturn setState(const apple80211_awdl_sync_state *data) {
        if (!data || data->version > APPLE80211_VERSION || data->state < 1 || data->state > 3)
            return kIOReturnBadArgument;
        if (!accept)
            return data->state == 1 ? kIOReturnSuccess : kIOReturnUnsupported;
        if (data->state == 2 || data->state == 3) {
            // The master announcement: keep the address, even a zero one ("no master
            // programmed" is itself a state the frame writer must be able to see).
            static_assert(sizeof(data->address) == 6, "Tahoe sync state layout");
            for (unsigned i = 0; i < 6; ++i)
                master[i] = data->address[i];
            hasMaster = true;
        } else {
            hasMaster = false;
        }
        state = data->state;
        return kIOReturnSuccess;
    }
    IOReturn getState(apple80211_awdl_sync_state *data) const {
        const IOReturn result = awGetInactiveState(data);
        if (result == kIOReturnSuccess)
            data->state = state;
        return result;
    }
    // "I am the master": nothing programmed, or macOS programmed our own address.
    bool isSyncMaster(const uint8_t self[6]) const {
        static const uint8_t zero[6] = {};
        if (!hasMaster)
            return true;
        return !memcmp(master, zero, 6) || !memcmp(master, self, 6);
    }
    bool syncMasterKnown() const { return hasMaster; }
    const uint8_t *syncMaster() const { return master; }
private:
    bool enabled = false;
    uint32_t state = 1;
    uint8_t master[6] = {};
    bool hasMaster = false;
};

class AwNativeFrameTemplate {
public:
    // Conservative driver-owned storage cap, based on the legacy/reference
    // path. Tahoe's observed length/pointer fields do not prove this maximum.
    // This buffer is not itself an 802.11 frame or a TX submission.
    static constexpr size_t capacity = 0x724;
    IOReturn set(const void *payload, size_t length) {
        if (length > capacity || (length && !payload))
            return kIOReturnBadArgument;
        const auto *source = static_cast<const uint8_t *>(payload);
        for (size_t i = 0; i < length; ++i)
            bytes[i] = source[i];
        for (size_t i = length; i < used; ++i)
            bytes[i] = 0;
        used = length;
        return kIOReturnSuccess;
    }
    size_t size() const { return used; }
    // Caller must serialize access, including this copy and template updates.
    IOReturn copy(void *destination, size_t available) const {
        if (available < used || (used && !destination))
            return kIOReturnNoSpace;
        auto *target = static_cast<uint8_t *>(destination);
        for (size_t i = 0; i < used; ++i)
            target[i] = bytes[i];
        return kIOReturnSuccess;
    }
private:
    uint8_t bytes[capacity] = {};
    size_t used = 0;
};
