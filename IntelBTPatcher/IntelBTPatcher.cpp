// SPDX-License-Identifier: GPL-3.0-only
// The HCI hook follows IntelBTPatcher (OpenIntelWireless/IntelBluetoothFirmware, GPLv3),
// the page hook follows BlueToolFixup (acidanthera/BrcmPatchRAM).
//
// A separate Lilu plugin that lets bluetoothd use extended LE advertising on a third-party
// controller. It only rewrites a process in memory and stays idle without its boot flag; nothing
// on disk changes.
#include <Headers/kern_api.hpp>
#include <Headers/kern_user.hpp>
#include <Headers/kern_util.hpp>
#include <Headers/plugin_start.hpp>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/usb/IOUSBHostDevice.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include "IntelBTPatcherPatches.hpp"
#include "IntelBTPatcherHci.hpp"

// ---- boot switches ---------------------------------------------------------------------
// The page rewrites run unless -btlenoscan is given; everything else here is opt-in.
static bool scanPatchOn {};
// Parking: off since 2.7.9. It never made a create succeed and it demonstrably killed Instant
// Hotspot by switching off the advertising the phone has to see. -ibtppark brings it back.
static bool parkOn {};
// Both HCI interventions are opt-in so the default build behaves like upstream 0.9.1: no park, no
// rewrite of the host's own create command. That is field evidence, not caution in the abstract -
// the feature they were written for is listed as unimplemented in upstream's ROADMAP, so these
// failures are the expected baseline rather than a regression of ours. -ibtpwiden experiments with
// the widened initiating window.
static bool widenOn {};
// -ibtpprivacy: OR the LL Privacy bit (bit 6 of the feature word) into the answer to LE Read Local
// Supported Features. Observation always happens; the rewrite needs the flag. bluetoothd only uses
// the controller's resolving list when that bit is set, and on this controller it never is - which
// is why every create with filter policy 1 scans six seconds and dies: the accept list holds
// identity addresses while peers advertise with a resolvable private address nobody can resolve.
static bool privacyOn {};
// -ibtpnorpa turns off the one rewrite that makes an outgoing connection possible at all. On by
// default because it is the only thing standing between the host and a create that cannot match:
// the accept list holds a phone's identity address while the air carries its rotating one.
static bool rpaOn {true};
// Thirty seconds, not the five first assumed: a resolvable private address rotates on the order of
// fifteen minutes, so an address heard half a minute ago is still the one on the air, while creates
// often land many seconds after the last sighting (bluetoothd hears the phone roughly once every
// six seconds on this controller). A stale-but-real address costs a timeout at worst; an
// over-strict window costs the rewrite itself.
enum : uint64_t { kRpaFreshNs = 30000000000ULL };

// ---- telemetry -------------------------------------------------------------------------
// Evidence for tests, in the I/O Registry root. Written only on rare events, in thread context.
static void publishValue(const char *key, uint32_t value) {
    IORegistryEntry::getRegistryRoot()->setProperty(key, value, 32);
}
static uint32_t bump(uint32_t &counter) { return __atomic_add_fetch(&counter, 1, __ATOMIC_RELAXED); }
// Publishes a counter that changed, outside the spinlock, since a registry write allocates.
static void publishIfChanged(const char *key, uint32_t value, uint32_t &seen) {
    if (value != __atomic_load_n(&seen, __ATOMIC_RELAXED)) {
        __atomic_store_n(&seen, value, __ATOMIC_RELAXED);
        publishValue(key, value);
    }
}
static uint64_t monotonicNs() {
    uint64_t now = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &now);
    return now;
}

// ---- bluetoothd page rewrites ----------------------------------------------------------
// Applied as the pages are validated, the way BlueToolFixup does it. Lilu's onProcLoad route was
// tried first and never applied to this process.
static mach_vm_address_t csValidateOrig {};
static void csValidatePageHook(vnode_t vp, memory_object_t pager, memory_object_offset_t page_offset,
                               const void *data, int *validated_p, int *tainted_p, int *nx_p) {
    char path[PATH_MAX];
    int pathlen = PATH_MAX;
    FunctionCast(csValidatePageHook, csValidateOrig)(vp, pager, page_offset, data, validated_p, tainted_p, nx_p);
    if (vn_getpath(vp, path, &pathlen) != 0 || LIKELY(strcmp(path, "/usr/sbin/bluetoothd") != 0))
        return;
    // Each set matches its own bluetoothd build only, so on any other build nothing happens.
    for (const Ibtp::RewriteSet &set : Ibtp::kSets)
        for (size_t i = 0; i < set.count; ++i) {
            const Ibtp::Rewrite &site = set.items[i];
            if (!site.scanOnly || scanPatchOn)
                KernelPatcher::findAndReplace(const_cast<void *>(data), PAGE_SIZE, site.from, site.length,
                                              site.to, site.length);
        }
}

// ---- HCI over USB control requests -----------------------------------------------------
// bluetoothd talks to the controller through USB control requests. Commands it does not send in the
// order the controller needs them are added here, right after the command they depend on. Their
// completion events reach bluetoothd unmatched, exactly as IntelBTPatcher's command does.
static mach_vm_address_t deviceRequestOrig {};
// deviceRequest runs on many threads at once. mirror is only read or written with mirrorLock held,
// never across a USB call, and belongs to the one controller in mirrorOwner.
static Ibtp::Mirror mirror {};
static IOLock *mirrorLock {};
static void *mirrorOwner {};
// An added command never waits longer than this, even when the caller's own request had no timeout.
static constexpr uint32_t kAddedTimeoutMs = 1000;

// The LE Data Length Change fix needs the controller's interrupt IN pipe, which only IOUSBHostPipe
// can reach, so the pipe is remembered as it is created.
static mach_vm_address_t pipeIoOrig {};
static mach_vm_address_t initPipeOrig {};
static mach_vm_address_t initPipeConfigOrig {};
static void *eventPipe {};
// The pipe's own constructor cannot be routed on this build, so the interrupt IN pipe is learned from
// its traffic instead: reads are wrapped on a budget until one of them is recognisably an HCI event.
// Before this, eventPipe stayed null for the whole boot and no event was ever seen - all the
// advertising-report and connection witnesses below were dead code, which is how a create that never
// matched looked identical to one that was never attempted.
static uint32_t probeLeft {}, eventPipeFound {};
enum : uint32_t { kProbeReads = 8192 };
// Written when a command goes out (any thread), read when an interrupt read is submitted or
// completes (the USB workloop). featureLock is a spinlock held only around the board itself: never
// across a USB call, an allocation or a registry write. It is taken after mirrorLock, never before.
static Ibtp::FeatureBoard features {};
static IOSimpleLock *featureLock {};
static uint32_t forgeWrites {}, forgeWriteFails {}, expiredSeen {}, abortsSeen {};

// Scanning parked for an LE Create Connection and put back once the attempt ends. Written on the USB
// workloop (event side), read on the callers' threads (command side); same benign-race reasoning as
// the feature board.
static Ibtp::Park park {};
static uint32_t parksMade {}, parksLifted {}, parkSendFails {}, createsSeen {}, watchdogLifts {},
    strayParks {}, widened {}, widenMissed {};
// The controller's own LE feature word, from LE Read Local Supported Features, published once.
static uint64_t featureWord {};
static uint32_t featureReads {}, privacyPatched {}, featureWriteFails {};
// The answer to 0x2003 usually lands on a read nothing else wraps: no data-length round armed, no
// park open. The pipe it arrives on cannot be named either, since routing IOUSBHostPipe::init* fails
// on this build (PipeHooks lacks bit 1), so eventPipe is never captured and no read is wrapped. A
// bounded window instead: reads are taken over for a wall-clock span or a read count, whichever
// comes first, and passed through untouched unless they carry the answer. It is armed at load
// rather than when the ask appears, because the ask rides a host command this hook may never see.
enum : uint32_t { kWatchReads = 8000 };
enum : uint64_t { kWatchSpanNs = 20000000000ULL, kWatchRearmNs = 3000000000ULL };
static uint32_t watchBudget {}, featureAsks {}, watchWraps {};
static uint64_t watchArmedNs {};
// How long a window opened at that instant lasts: the load-time one is wide, every ask narrows it to
// just around the answer.
static uint64_t watchSpanNs { kWatchSpanNs };
// What the mirror believes, as one word: bit 0 legacy scan, bit 8 extended scan, bit 16 legacy
// advertising, bit 24 extended advertising. Written only when it changes, so a desync between the
// kext and the radio shows up in ioreg instead of having to be inferred.
static uint32_t mirrorBits = 0xffffffff;
static void publishMirrors(const Ibtp::Mirror &m) {
    const uint32_t bits = uint32_t(m.scanOn) | uint32_t(m.extScanOn) << 8 |
                          uint32_t(m.advOn) << 16 | uint32_t(m.extAdvOn) << 24;
    if (bits == mirrorBits)
        return;
    mirrorBits = bits;
    IORegistryEntry::getRegistryRoot()->setProperty("IbtpMirrors", bits, 32);
}

// ---- Continuity witness ----------------------------------------------------------------
// The parsing lives in Ibtp so the host test can drive it; this file only publishes the result.
// Both frames exist solely on the air, so the HCI layer is the one honest place to ask: does the Mac
// really broadcast the 0x0D Wi-Fi Settings invite, and does the phone answer with 0x0E? Read-only,
// nothing here alters a byte.
static uint32_t advDataCount, reportCount, hotspotAnswers, lastRssi;
static uint32_t advTypeMask, reportTypeMask, reportEvtMask;
static uint32_t shownTypeMask, shownEvtMask;
static bool inviteSaved;
// The address last put on the filter accept list, and how often an advertiser used it. A policy-1
// create can only match when these agree: the controller has no resolving list, so it compares the
// six bytes literally instead of resolving a private address first. An accept-list address that is
// never heard on the air is an identity address, and no timeout will ever find it.
static uint8_t acceptAddr[6] {};
static uint32_t acceptAddrHeard, advAddrCount;
// The newest connectable resolvable private address heard on the air, and when. bluetoothd keeps its
// IRKs in user space and resolves the phone's rotating address itself, so the controller is handed
// the phone's permanent identity address and asked to match it against a list of the same - while the
// air only ever carries the rotating form. A policy-1 create then waits out its full timeout and
// bluetoothd cancels it. This is the one address the controller can act on without an IRK, so it is
// kept with its age: a rotating address that has not been refreshed is worse than none.
static uint8_t rpaAddr[6];
static uint64_t rpaStamp;
static uint32_t rpaSeen, rpaRewrites, rpaMissed, rpaWriteFails, extReports;
static uint32_t advMetaSeen;
static void witnessAdvData(const uint8_t *hci, size_t length) {
    Ibtp::PayloadRange payload {};
    if (!Ibtp::payloadRange(hci, length, &payload))
        return;
    uint32_t types = 0;
    Ibtp::appleTypeIds(hci + payload.offset, payload.length, &types);
    if (!types)
        return;
    advTypeMask |= types;
    publishValue("IbtpAdvDataTypes", advTypeMask);
    publishValue("IbtpAdvDataSets", ++advDataCount);
    // Keep one real 0x0D frame, so the payload can be read back byte for byte.
    if ((types & 1U << 0x0d) && !inviteSaved) {
        inviteSaved = true;
        if (OSData *raw = OSData::withBytes(hci + payload.offset, uint32_t(payload.length))) {
            IORegistryEntry::getRegistryRoot()->setProperty("IbtpInviteFrame", raw);
            raw->release();
        }
    }
}
static void witnessAdvReport(const uint8_t *event, size_t size) {
    // Every advertising report the pipe delivers, parsed or not: if this grows while the parsed
    // counters stay empty the parser is wrong, not the controller.
    if (!(++advMetaSeen & 31))
        publishValue("IbtpAdvMeta", advMetaSeen);
    Ibtp::Report items[8] {};
    const int count = Ibtp::parseReports(event, size, items, 8);
    if (count <= 0)
        return;
    const uint64_t now = monotonicNs();
    if (event[2] == Ibtp::kSubExtAdvReport)
        publishValue("IbtpAdvExtReports", ++extReports);
    uint32_t types = 0;
    for (int i = 0; i < count; ++i) {
        types |= items[i].appleTypes;
        // The flag word only exists in the extended form, so it is what gets published: a host that
        // never asks for legacy reports leaves this at zero even while it scans continuously.
        const uint8_t kind = items[i].eventType & 0x7f;
        if (items[i].eventType & Ibtp::kReportExtended)
            reportEvtMask |= 1U << kind;
        // The freshest connectable resolvable address is the only handle on the phone that does not
        // need its IRK. Refresh on every sighting: an address that stops appearing has rotated away.
        if (Ibtp::reportConnectable(items[i].eventType) && Ibtp::addressResolvable(items[i].addr)) {
            if (!Ibtp::addressEquals(items[i].addr, rpaAddr)) {
                lilu_os_memcpy(rpaAddr, items[i].addr, sizeof(rpaAddr));
                if (OSData *raw = OSData::withBytes(rpaAddr, sizeof(rpaAddr))) {
                    IORegistryEntry::getRegistryRoot()->setProperty("IbtpRpaLast", raw);
                    raw->release();
                }
            }
            rpaStamp = now;
            publishValue("IbtpRpaSeen", ++rpaSeen);
        }
    }
    lastRssi = uint32_t(items[count - 1].rssi);
    reportTypeMask |= types;
    if (reportTypeMask != shownTypeMask || reportEvtMask != shownEvtMask) {
        shownTypeMask = reportTypeMask;
        shownEvtMask = reportEvtMask;
        publishValue("IbtpAdvReportAppleTypes", reportTypeMask);
        publishValue("IbtpAdvReportEventTypes", reportEvtMask);
    }
    if (types & 1U << 0x0e) {
        // The phone answered "I have a hotspot". The menu entry, the rapport link and the silent PSK
        // all depend on this frame ever being seen.
        publishValue("IbtpTetherSources", ++hotspotAnswers);
        publishValue("IbtpTetherRssi", lastRssi);
        if (OSData *raw = OSData::withBytes(items[count - 1].addr, 6)) {
            IORegistryEntry::getRegistryRoot()->setProperty("IbtpTetherAddr", raw);
            raw->release();
        }
    }
    if (!(++reportCount & 15))
        publishValue("IbtpAdvReports", reportCount);
    // Does the accept-list address ever show up on the air? Answered here rather than inferred later,
    // because a stale or identity entry looks exactly like a working one until the create times out.
    bool heard = false;
    for (int i = 0; i < count; ++i)
        heard = heard || Ibtp::addressEquals(items[i].addr, acceptAddr);
    if (heard)
        publishValue("IbtpAcceptAddrHeard", ++acceptAddrHeard);
    if ((++advAddrCount & 63) == 0) {
        if (OSData *raw = OSData::withBytes(items[count - 1].addr, 6)) {
            IORegistryEntry::getRegistryRoot()->setProperty("IbtpAdvLastAddr", raw);
            raw->release();
        }
    }
}
// What the create asked for and what the controller answered.
static uint32_t opMask;
static uint16_t failOp;
static uint8_t failStatus;
static uint32_t failCount;
static uint16_t createOp;
static uint8_t createPolicy, createAddrType, createPeerType, createPeer[6];
static uint32_t createCompletes, connEvents, connOk, createStatuses, cancels;
static uint8_t createStatus, connStatus;
// Which filter policies bluetoothd has used since boot, one bit each, and how often it named a peer
// address. The last create alone cannot answer either: a policy 0 with a real address is the only
// form the controller can act on without a resolving list, so whether that form ever appears decides
// which fix is even possible.
static uint32_t policySeen, peersGiven;
static void witnessCreate(const uint8_t *hci, size_t length) {
    if (!hci || length < 3)
        return;
    const uint16_t op = Ibtp::opcodeOf(hci);
    createOp = op;
    if (op == Ibtp::OpLeExtCreateConn && length >= 12) {
        // filter policy, own address type, peer address type, peer address
        createPolicy = hci[3];
        createAddrType = hci[4];
        createPeerType = hci[5];
        memcpy(createPeer, hci + 6, 6);
    } else if (op == Ibtp::OpLeCreateConn && length >= 15) {
        // scan interval, scan window, filter policy, peer address type, peer address
        createPolicy = hci[7];
        createAddrType = hci[8];
        createPeerType = hci[8];
        memcpy(createPeer, hci + 9, 6);
    } else {
        return;
    }
    IORegistryEntry::getRegistryRoot()->setProperty("IbtpCreateOpcode", createOp, 16);
    IORegistryEntry::getRegistryRoot()->setProperty("IbtpCreateFilterPolicy", createPolicy, 8);
    IORegistryEntry::getRegistryRoot()->setProperty("IbtpCreateAddrType", createAddrType, 8);
    IORegistryEntry::getRegistryRoot()->setProperty("IbtpCreatePeerType", createPeerType, 8);
    IORegistryEntry::getRegistryRoot()->setProperty("IbtpCreatePeerAddr", createPeer, 6);
    if (createPolicy < 32 && !(policySeen & (1U << createPolicy))) {
        policySeen |= 1U << createPolicy;
        IORegistryEntry::getRegistryRoot()->setProperty("IbtpCreatePolicyMask", policySeen, 32);
    }
    bool named = false;
    for (unsigned i = 0; i < 6; ++i)
        named = named || createPeer[i] != 0;
    if (named)
        IORegistryEntry::getRegistryRoot()->setProperty("IbtpCreateWithPeer", ++peersGiven, 32);
}

// What goes on the filter accept list, and whether anything ever goes on the resolving list. Without
// a resolving list the controller compares accept-list entries literally, so the type and value here
// are the whole reason a policy-1 create cannot match a peer advertising from a private address.
static uint32_t acceptAdds, resolvingAdds;
static void witnessLists(const uint8_t *hci, size_t length) {
    if (!hci || length < 10)
        return;
    const uint16_t op = Ibtp::opcodeOf(hci);
    if (op != Ibtp::OpLeAcceptListAdd && op != Ibtp::OpLeResolvingAdd)
        return;
    // Both commands are address type then address. The rest of the resolving entry is the IRK.
    auto *root = IORegistryEntry::getRegistryRoot();
    if (op == Ibtp::OpLeAcceptListAdd)
        root->setProperty("IbtpAcceptAdds", ++acceptAdds, 32);
    else
        root->setProperty("IbtpResolvingAdds", ++resolvingAdds, 32);
    root->setProperty("IbtpListLastType", uint32_t(hci[3]), 8);
    uint8_t addr[6];
    memcpy(addr, hci + 4, sizeof(addr));
    root->setProperty("IbtpListLastAddr", addr, 6);
    if (op == Ibtp::OpLeAcceptListAdd)
        memcpy(acceptAddr, addr, sizeof(acceptAddr));
}

// Vendor commands. The host hands its IRKs to Broadcom parts through one of these instead of the
// standard LE Add Device To Resolving List, so a controller that never sees one is a controller the
// host never asked to resolve anything. Opcode plus the first bytes of the payload identify the
// family without guessing at a payload layout this driver has no other way to learn.
static uint32_t vendorCount;
static void witnessVendor(const uint8_t *hci, size_t length) {
    if (!hci || length < 3)
        return;
    const uint16_t op = Ibtp::opcodeOf(hci);
    if ((op & 0xfc00) != 0xfc00)
        return;
    auto *root = IORegistryEntry::getRegistryRoot();
    root->setProperty("IbtpVendorLastOpcode", uint32_t(op), 16);
    root->setProperty("IbtpVendorLastLength", uint32_t(length), 16);
    const size_t take = length > 3 ? (length - 3 > 24 ? 24 : length - 3) : 0;
    if (take) {
        if (OSData *params = OSData::withBytes(hci + 3, uint32_t(take))) {
            root->setProperty("IbtpVendorLastParams", params);
            params->release();
        }
    }
    root->setProperty("IbtpVendorCmds", ++vendorCount, 32);
}

// Intel's vendor ID also sits on hubs and other devices. Without the controller's product name it
// has to be a wireless controller at least (USB class 0xE0).
static bool isOurController(void *that) {
    auto *device = static_cast<IORegistryEntry *>(that);
    auto *vendor = OSDynamicCast(OSNumber, device->getProperty("idVendor"));
    if (!vendor || vendor->unsigned32BitValue() != 0x8087)
        return false;
    if (auto *name = OSDynamicCast(OSString, device->getProperty("USB Product Name")))
        return name->isEqualTo("Bluetooth USB Host Controller");
    auto *deviceClass = OSDynamicCast(OSNumber, device->getProperty("bDeviceClass"));
    return deviceClass && deviceClass->unsigned32BitValue() == 0xE0;
}

static IOReturn deviceRequestHook(void *that, IOService *provider, StandardUSB::DeviceRequest &request, void *data,
                                  IOMemoryDescriptor *descriptor, unsigned int &length,
                                  IOUSBHostCompletion *completion, uint32_t timeout);

static IOReturn sendAdded(void *that, IOService *provider, const StandardUSB::DeviceRequest &request,
                          const uint8_t *bytes, size_t size, uint32_t timeout) {
    auto *extra = IOBufferMemoryDescriptor::withBytes(bytes, size, kIODirectionOut);
    if (!extra)
        return kIOReturnNoMemory;
    StandardUSB::DeviceRequest extraRequest = request;
    extraRequest.wLength = uint16_t(size);
    unsigned int extraLength = uint32_t(size);
    // Synchronous, so bounded: a stalled controller must not hold the calling thread forever.
    const uint32_t extraTimeout = timeout == 0 || timeout > kAddedTimeoutMs ? kAddedTimeoutMs : timeout;
    IOReturn result = extra->prepare(kIODirectionOut);
    if (result == kIOReturnSuccess) {
        result = FunctionCast(deviceRequestHook, deviceRequestOrig)(that, provider, extraRequest, nullptr, extra,
                                                                    extraLength, nullptr, extraTimeout);
        extra->complete(kIODirectionOut);
    }
    extra->release();
    return result;
}

static uint32_t addedCommands {}, addedAddresses {}, addedFailures {};
// The seed is counted apart from the follow-ups: it is the one command the whole workaround exists
// for, and until this counter appeared there was no way to tell whether it ever left the machine.
static uint32_t seededAddresses {}, seedFailures {};
static IOReturn deviceRequestHook(void *that, IOService *provider, StandardUSB::DeviceRequest &request, void *data,
                                  IOMemoryDescriptor *descriptor, unsigned int &length,
                                  IOUSBHostCompletion *completion, uint32_t timeout) {
    uint8_t hci[32] = {};
    size_t hciLength = 0;
    // Set only when the descriptor's own bytes were rewritten below: the hold is already gone by the
    // time the mirror section runs, and a second prepare/write round on this stack's descriptor is
    // not available (prepare fails, and an unwired writeBytes panics - the "IOGMD: not wired" crash
    // 2.7.10 died on).
    bool createRewritten {};
    bool createWriteFailed {};
    size_t createPeerOffset {};     // where the six rewritten peer bytes sit, 0 when untouched
    // Every USB device passes through here, so only the Intel Bluetooth controller is of interest.
    // Class requests to the device carry HCI commands; bRequest says nothing useful about that.
    if (mirrorLock && (request.bmRequestType == 0x20 || request.bmRequestType == 0x21) && isOurController(that)) {
        if (data) {
            hciLength = min(size_t(request.wLength), sizeof(hci));
            lilu_os_memcpy(hci, data, hciLength);
        } else if (descriptor && descriptor->prepare(kIODirectionOut) == kIOReturnSuccess) {
            // The request length, not the descriptor's capacity, bounds the copy: the stack maps
            // exactly wLength bytes of this buffer.
            hciLength = min(min(size_t(request.wLength), size_t(descriptor->getLength())), sizeof(hci));
            descriptor->readBytes(0, hci, hciLength);
            descriptor->complete(kIODirectionOut);
            // The rewrite runs in a second prepare/complete pair, and the direction flips: the
            // header contract is explicit that writeBytes requires the descriptor prepared
            // kIODirectionIn while readBytes requires kIODirectionOut - the bit describes the
            // device side, so host-to-memory PIO needs In no matter which way the USB transfer
            // runs. Writing inside the read's Out window (2.4.2.3) violated the contract and was
            // refused on every create, and re-preparing far from the read (2.4.2.2) lost the wire
            // state on top of that. An unwired write panics ("IOGMD: not wired", the 2.7.10 crash),
            // so the In hold before writeBytes is mandatory, not optional.
            const uint16_t earlyOp = hciLength >= 3 ? Ibtp::opcodeOf(hci) : 0;
            const bool rpaFresh = rpaStamp && monotonicNs() - rpaStamp < kRpaFreshNs;
            if (earlyOp && rpaOn &&
                (createPeerOffset = Ibtp::rewriteCreateToPrivateAddress(hci, hciLength, rpaAddr, rpaFresh)) != 0) {
                const IOReturn prepped = descriptor->prepare(kIODirectionIn);
                if (prepped == kIOReturnSuccess) {
                    const size_t wrote = descriptor->writeBytes(0, hci, hciLength);
                    createRewritten = wrote == hciLength;
                    createWriteFailed = !createRewritten;
                    if (createWriteFailed) {
                        // Counts alone cannot say why a prepared write is refused; keep the numbers.
                        publishValue("IbtpRpaWriteRet", uint32_t(wrote));
                        publishValue("IbtpRpaCmdLen", uint32_t(hciLength));
                        publishValue("IbtpRpaDescLen", uint32_t(descriptor->getLength()));
                        publishValue("IbtpRpaReqLen", request.wLength);
                    }
                    descriptor->complete(kIODirectionIn);
                } else {
                    createWriteFailed = true;
                    publishValue("IbtpRpaPrepInRet", uint32_t(prepped));
                }
            }
        }
    }
    bool seedLegacyAddress = false;
    unsigned remoteReads = 0;
    Ibtp::OutPacket pres[4] = {};
    int preCount = 0;
    bool restoring = false;
    Ibtp::FollowUp follow {};
    if (hciLength) {
        IOLockLock(mirrorLock);
        // A different controller object than last time starts from nothing.
        if (mirrorOwner != that) {
            mirror = Ibtp::Mirror{};
            park = Ibtp::Park{};
            mirrorOwner = that;
            // A re-enumerated or second controller: the remembered pipe and any pending duplicates
            // belonged to the old one.
            eventPipe = nullptr;
            if (featureLock) {
                IOSimpleLockLock(featureLock);
                const uint32_t expired = features.expired, aborts = features.aborts;
                features = Ibtp::FeatureBoard{};
                features.expired = expired;
                features.aborts = aborts;
                IOSimpleLockUnlock(featureLock);
            }
        }
        const uint16_t op = hciLength >= 3 ? Ibtp::opcodeOf(hci) : 0;
        // LE Read Local Supported Features: its answer decides the resolving-list route, and it comes
        // back on a read that may not be wrapped, so arm the bounded watch before it can arrive.
        if (op == Ibtp::OpLeReadLocalFeatures) {
            watchSpanNs = kWatchRearmNs;
            watchArmedNs = monotonicNs();
            // The budget only ever extends; an ask never grants the window unlimited reads.
            if (__atomic_load_n(&watchBudget, __ATOMIC_RELAXED) < 400)
                __atomic_store_n(&watchBudget, 400, __ATOMIC_RELAXED);
            publishValue("IbtpFeatureAsks", ++featureAsks);
        }
        witnessLists(hci, hciLength);
        witnessVendor(hci, hciLength);
        // Witness every advertising payload the host publishes, before any decision below can change
        // a byte. It is the only way to see whether the 0x0D invite frame goes out.
        witnessAdvData(hci, hciLength);
        // Which route the host takes to reach the phone. Read-only, and the only way to tell the
        // three apart without a sniffer - they all produce the same create command.
        if (const uint32_t fp = Ibtp::fingerprintOf(op)) {
            const uint32_t seen = opMask;
            opMask |= fp;
            if (opMask != seen)
                publishValue("IbtpLeOps", opMask);
        }
        if (op == Ibtp::OpReset) {
            park = Ibtp::Park{};
            // A controller reset turned every scan and advertising engine off.
            mirror.scanOn = mirror.extScanOn = 0;
            mirror.advOn = mirror.extAdvOn = false;
        }
        const bool isCreate = op == Ibtp::OpLeCreateConn || op == Ibtp::OpLeExtCreateConn;
        if (isCreate) {
            publishValue("IbtpLeCreates", ++createsSeen);
            witnessCreate(hci, hciLength);
            // The rotating address is only usable while it is still the one on the air. Five seconds
            // is several advertising intervals at every interval the phone uses, so a stale value is
            // already gone by the time the create would leave.
            const bool rpaFresh = rpaStamp && monotonicNs() - rpaStamp < kRpaFreshNs;
            bool written = createRewritten;
            if (!written && data && rpaOn) {
                const size_t peerAt = Ibtp::rewriteCreateToPrivateAddress(hci, hciLength, rpaAddr, rpaFresh);
                if (peerAt) {
                    // The direct-buffer route: the caller's own buffer is editable in place.
                    lilu_os_memcpy(data, hci, hciLength);
                    written = true;
                    createPeerOffset = peerAt;
                }
            }
            if (written) {
                publishValue("IbtpCreateRpa", ++rpaRewrites);
                // What actually left the machine: the six peer bytes, so a dead create can be told
                // from one aimed at a stale address. Extended form: policy[3], peerType[4],
                // peer[6..11]; legacy: policy[7], peerType[8], peer[9..14].
                if (OSData *raw = OSData::withBytes(hci + createPeerOffset, 6)) {
                    IORegistryEntry::getRegistryRoot()->setProperty("IbtpRpaApplied", raw);
                    raw->release();
                }
            } else if (createWriteFailed) {
                // Conditions matched at the top but the prepared write was refused - counted apart
                // from a stale address, because this one needs code, not patience.
                publishValue("IbtpRpaWriteFails", ++rpaWriteFails);
            } else if (rpaOn) {
                // No usable private address when the create left: stale or never heard.
                publishValue("IbtpCreateRpaSkipped", ++rpaMissed);
            }
            // Park scanning and advertising for the create; the helper remembers what it switched off
            // so the attempt-ending event can put back exactly that.
            preCount = Ibtp::parkBeforeCreate(mirror, park, hci, hciLength, pres, parkOn);
            publishValue("IbtpLastParkCommands", uint32_t(preCount));
            publishMirrors(mirror);
        } else {
            // Capture what was parked before the restore clears the flags: the mirror has to follow
            // what the radio really runs again, or the next create finds zeros and never parks - the
            // desync that let the bug through on every resume.
            const bool tookScan = park.tookScan, tookExtScan = park.tookExtScan,
                       tookAdv = park.tookAdv, tookExtAdv = park.tookExtAdv;
            if ((preCount = Ibtp::restoreAfterCreate(park, pres)) > 0) {
                restoring = true;
                if (park.forcedByWatchdog)
                    publishValue("IbtpResumeWatchdog", ++watchdogLifts);
                if (tookScan)
                    mirror.scanOn = park.scanOn;
                if (tookExtScan)
                    mirror.extScanOn = park.extScanOn;
                if (tookAdv)
                    mirror.advOn = true;
                if (tookExtAdv)
                    mirror.extAdvOn = true;
            } else {
                // The same helper is the only place the host's scan/advertising enables are mirrored.
                // Skipping it for ordinary commands left every mirror at zero, so no create ever
                // parked - the regression that made 2.7.7 a no-op.
                preCount = Ibtp::parkBeforeCreate(mirror, park, hci, hciLength, pres, parkOn);
                if (preCount > 0)   // a non-create op should never park; count it if one does
                    publishValue("IbtpUnexpectedParks", ++strayParks);
            }
            publishMirrors(mirror);
        }
        seedLegacyAddress = Ibtp::wantsSeedLegacyAddress(mirror, hci, hciLength);
        follow = Ibtp::mirrorCommand(mirror, hci, hciLength, scanPatchOn);
        IOLockUnlock(mirrorLock);
        if (featureLock) {
            const uint64_t now = monotonicNs();
            IOSimpleLockLock(featureLock);
            remoteReads = Ibtp::duplicateReads(features, hci, hciLength, now);
            IOSimpleLockUnlock(featureLock);
        }
        if (widenOn) {
            uint32_t observed = 0;
            if (Ibtp::widenInitiatorScan(hci, hciLength, &observed)) {
                publishValue("IbtpCreateScanParams", observed);
                // The command leaves from the caller's buffer and hci is only our shadow, so the
                // widened window has to be written back or the controller never sees it. Only the
                // plain data pointer is touched: writeBytes on the USB IOMemoryDescriptor panics
                // with "IOGMD: not wired for the IODMACommand", which is exactly what 2.7.10
                // crashed on.
                if (data) {
                    lilu_os_memcpy(data, hci, hciLength);
                    publishValue("IbtpWidened", ++widened);
                } else {
                    // Descriptor-only request: no safe way to rewrite it, so the host's own duty
                    // cycle stays. Counted, so the telemetry shows whether this is the common path.
                    publishValue("IbtpWidenSkipped", ++widenMissed);
                }
            }
        }
    }
    for (int i = 0; i < preCount; ++i) {
        if (sendAdded(that, provider, request, pres[i].bytes, pres[i].len, timeout) == kIOReturnSuccess) {
            if (restoring)
                publishValue("IbtpScanResumes", ++parksLifted);
            else
                publishValue("IbtpScanSuspends", ++parksMade);
        } else {
            publishValue("IbtpScanResumeFails", ++parkSendFails);
        }
    }
    for (unsigned i = 0; i < remoteReads; ++i) {
        // Sent before the real command, as IntelBTPatcher does: all three completions carry the same
        // features, so the first reaches bluetoothd untouched and the other two are rewritten into
        // the events it is missing.
        if (sendAdded(that, provider, request, hci, hciLength, timeout) != kIOReturnSuccess) {
            if (featureLock) {
                IOSimpleLockLock(featureLock);
                Ibtp::dropDuplicate(features, hci, hciLength);
                IOSimpleLockUnlock(featureLock);
            }
            publishValue("IbtpLengthFailures", bump(forgeWriteFails));
        }
    }
    if (seedLegacyAddress) {
        // IntelBTPatcher's workaround, sent before the scan command like the original. The mirror
        // flag moves only after the controller accepted it, so a rejected seed is tried again on the
        // next scan-parameters command instead of being counted as done.
        uint8_t seed[9] = {0x05, 0x20, 0x06};
        lilu_os_memcpy(seed + 3, Ibtp::kLegacySeedAddress, 6);
        if (sendAdded(that, provider, request, seed, sizeof(seed), timeout) == kIOReturnSuccess) {
            publishValue("IbtpLegacyAddresses", ++seededAddresses);
            IOLockLock(mirrorLock);
            Ibtp::noteSeedAccepted(mirror);
            IOLockUnlock(mirrorLock);
        } else {
            publishValue("IbtpLegacySeedFailures", ++seedFailures);
        }
    }
    static bool sawCommand {};
    if (hciLength >= 3 && !sawCommand) {
        sawCommand = true;
        publishValue("IbtpFirstRequest", request.bRequest);
    }
    const IOReturn result = FunctionCast(deviceRequestHook, deviceRequestOrig)(that, provider, request, data, descriptor,
                                                                              length, completion, timeout);
    if (follow.setAddress || follow.eventMask) {
        // Only after the command it depends on was accepted. A set address that did not go out is
        // forgotten, so the next parameters command for that set tries again.
        const IOReturn extra = result == kIOReturnSuccess ?
            sendAdded(that, provider, request, follow.bytes, follow.len, timeout) : result;
        if (follow.setAddress && extra != kIOReturnSuccess) {
            IOLockLock(mirrorLock);
            if (mirrorOwner == that)
                mirror.addressed &= ~(1U << follow.bytes[3]);
            IOLockUnlock(mirrorLock);
        }
        if (extra == kIOReturnSuccess) {
            publishValue("IbtpExtraCommands", ++addedCommands);
            if (follow.setAddress)
                publishValue("IbtpSetAddresses", ++addedAddresses);
        } else {
            publishValue("IbtpExtraFailures", ++addedFailures);
        }
    }
    return result;
}

// The controller's events arrive on its interrupt IN pipe and only the pipe object can wrap them, so
// it is remembered when created. Anything else on the bus is left alone.
static void rememberEventPipe(void *that, StandardUSB::EndpointDescriptor const *descriptor, IOUSBHostDevice *device) {
    if (!descriptor || !device || !isOurController(device))
        return;
    // The endpoint is read by hand: StandardUSB::getEndpointType/getEndpointDirection are exports of
    // IOUSBHostFamily that OpenCore's prelinker resolves to 0 (RIP-relative overflow), which makes
    // injection fail. The descriptor is the 7-byte USB layout: bEndpointAddress at offset 2 (bit 7 =
    // direction in), bmAttributes at offset 3 (bits 0-1 = type).
    auto *bytes = reinterpret_cast<const uint8_t *>(descriptor);
    if (bytes[2] & 0x80 && (bytes[3] & 0x03) == 0x03)
        eventPipe = that;
}

// The pipe is created through one of two overloads, depending on the macOS version.
static int initPipeHook(void *that, StandardUSB::EndpointDescriptor const *descriptor,
                        StandardUSB::SuperSpeedEndpointCompanionDescriptor const *companion, void *controller,
                        IOUSBHostDevice *device, void *interface, unsigned char a7, unsigned short a8) {
    const int result = FunctionCast(initPipeHook, initPipeOrig)(that, descriptor, companion, controller, device,
                                                                interface, a7, a8);
    rememberEventPipe(that, descriptor, device);
    return result;
}

static int initPipeConfigHook(void *that, StandardUSB::EndpointDescriptor const *descriptor,
                              StandardUSB::ConfigurationDescriptor const *config, void *controller,
                              IOUSBHostDevice *device, void *interface, unsigned char a7, unsigned short a8) {
    const int result = FunctionCast(initPipeConfigHook, initPipeConfigOrig)(that, descriptor, config, controller,
                                                                           device, interface, a7, a8);
    rememberEventPipe(that, descriptor, device);
    return result;
}

// A completion owns the caller's closure, so the original one is kept here while ours runs.
struct WrappedRead {
    IOUSBHostCompletion original;
    IOMemoryDescriptor *buffer;
    void *pipe;                     // which pipe this read belongs to, so the event one can be learned
    IOUSBHostCompletion wrapped;    // handed to the USB stack, so the caller's own is never edited
};

static void readCompletion(void *owner, void *parameter, IOReturn status, uint32_t bytesTransferred) {
    auto *delayed = static_cast<WrappedRead *>(owner);
    IOMemoryDescriptor *buffer = delayed->buffer;
    // readBytes()/writeBytes() need the descriptor wired, and only a hold taken here is known to be
    // there: touching a buffer this call could not prepare panics with "IOGMD: not wired for the
    // IODMACommand". 0.9.1 assumed a refused prepare() meant "already prepared on Tahoe" and read
    // anyway; upstream's Tahoe run shows prepare() succeeds. A hold taken here is also the only one
    // released here.
    const bool held = status == kIOReturnSuccess && buffer &&
        buffer->getLength() >= Ibtp::kDataLengthLen &&
        buffer->prepare(kIODirectionInOut) == kIOReturnSuccess;
    bool abort = !held;
    // Set when a feature-word answer was rewritten in place at the same size; the shared write-back
    // below only fires on a size change, so a same-size edit needs its own.
    bool featureEdited = false;
    if (held) {
        // Anything shorter than 6 bytes cannot be one of the rewritten events.
        if (bytesTransferred >= 6) {
            // The full HCI event is wanted here, not a 32-byte window: an extended advertising
            // report approaches the 257-byte event maximum, and a window cut mid-payload fails
            // the parser's bounds check, which looks exactly like "no report ever arrived".
            uint8_t event[260] = {};
            const size_t size = min(min(size_t(bytesTransferred), sizeof(event)), size_t(buffer->getLength()));
            buffer->readBytes(0, event, size);
            // The first read that is recognisably an HCI event names the pipe it arrived on. Once
            // known, only that pipe is wrapped again, so probing costs a bounded number of reads.
            if (!eventPipe && Ibtp::looksLikeHciEvent(event, size)) {
                eventPipe = delayed->pipe;
                publishValue("IbtpEventPipe", ++eventPipeFound);
            }
            // A connection attempt that just ended releases what was parked for it; the next outgoing
            // command carries the restore.
            Ibtp::observeEndingEvent(park, event, size);
            // Unconditional HCI witness: the controller's verdict on the last create, park or no
            // park. Gating it on the park meant a silent no-park run left no evidence at all (2.7.7
            // went in blind that way).
            if (event[0] == Ibtp::kEvtLeMeta && size >= 5 &&
                (event[2] == Ibtp::kSubAdvReport || event[2] == Ibtp::kSubExtAdvReport)) {
                // Advertising reports are pure observation: what the phone says - and whether what it
                // says is connectable - decides whether any create can land. bluetoothd scans with
                // the extended commands here, so subevent 0x0d is the form that actually arrives.
                witnessAdvReport(event, size);
            } else if (event[0] == Ibtp::kEvtCmdComplete && size >= 8 &&
                       uint16_t(event[3] | event[4] << 8) == createOp) {
                createStatus = event[5];
                publishValue("IbtpCreateCmdStatus", createStatus);
                publishValue("IbtpCreateCmdCompletes", ++createCompletes);
            } else if (event[0] == Ibtp::kEvtCmdStatus && size >= 6 &&
                       uint16_t(event[4] | event[5] << 8) == createOp) {
                // The controller's verdict when it rejects the create outright.
                publishValue("IbtpCreateStatus", event[2]);
                publishValue("IbtpCreateStatusEvents", ++createStatuses);
            } else if (event[0] == Ibtp::kEvtCmdComplete && size >= 11 &&
                       uint16_t(event[3] | event[4] << 8) == Ibtp::OpLeReadLocalFeatures) {
                // The controller's own feature word, published raw: whether bit 6 (LL Privacy) is
                // honest decides the whole resolving-list route.
                featureWord = 0;
                for (int i = 0; i < 8; ++i)
                    featureWord |= uint64_t(event[5 + i]) << (8 * i);
                IORegistryEntry::getRegistryRoot()->setProperty("IbtpLEFeatures", featureWord, 64);
                publishValue("IbtpLEFeatureReads", ++featureReads);
                if (privacyOn && !event[2] && !(event[5] & 0x40)) {
                    event[5] |= 0x40;
                    featureEdited = true;
                    publishValue("IbtpPrivacyRewrites", bump(privacyPatched));
                }
            } else if ((event[0] == Ibtp::kEvtCmdComplete || event[0] == Ibtp::kEvtCmdStatus) && size >= 6) {
                const uint16_t witnessOp = event[0] == Ibtp::kEvtCmdStatus
                    ? uint16_t(event[4] | event[5] << 8)
                    : uint16_t(event[3] | event[4] << 8);
                if (witnessOp == Ibtp::OpLeCreateConnCancel)
                    publishValue("IbtpCancelEvents", ++cancels);
                // An LE command the controller rejects is a dead end bluetoothd may never retry, and
                // the rejection never reaches user space. Keeping the first one on record separates
                // "the link was never possible" from "the link was attempted and timed out".
                const uint8_t witnessStatus = event[0] == Ibtp::kEvtCmdStatus ? event[2] : event[5];
                if (witnessStatus && (witnessOp & 0xfc00) == 0x2000) {
                    failOp = witnessOp;
                    failStatus = witnessStatus;
                    IORegistryEntry::getRegistryRoot()->setProperty("IbtpLeFailOp", failOp, 16);
                    publishValue("IbtpLeFailStatus", failStatus);
                    publishValue("IbtpLeFails", ++failCount);
                }
            } else if (event[0] == Ibtp::kEvtLeMeta && size >= 4 &&
                       (event[2] == Ibtp::kSubConnComplete || event[2] == Ibtp::kSubConnCompleteEnhanced ||
                        event[2] == Ibtp::kSubConnCompleteEnhancedV2)) {
                connStatus = event[3];
                publishValue("IbtpConnStatus", connStatus);
                publishValue("IbtpConnCompletes", ++connEvents);
                if (!connStatus)
                    publishValue("IbtpConnOk", ++connOk);
            }
            // A parked round can wrap reads with no data-length round armed, so the board and its
            // lock are not guaranteed here; without the lock nothing is tracked at all.
            size_t rewritten = size;
            if (featureLock) {
                IOSimpleLockLock(featureLock);
                rewritten = Ibtp::forgeEvent(features, event, size);
                IOSimpleLockUnlock(featureLock);
            }
            // writeBytes returns the number of bytes written, not an IOReturn. The shorter size may
            // only be claimed when every rewritten byte is really in the buffer: forwarding the
            // original size leaves stale feature bytes behind, the stack parses them as garbage
            // events and bluetoothd treats the parser desync as fatal (reason 607).
            if (featureEdited && rewritten == size) {
                if (buffer->writeBytes(0, event, rewritten) == rewritten) {
                    bytesTransferred = uint32_t(rewritten);
                } else {
                    publishValue("IbtpFeatureWriteFails", bump(featureWriteFails));
                    abort = true;
                }
            } else if (rewritten != size) {
                if (buffer->writeBytes(0, event, rewritten) == rewritten) {
                    bytesTransferred = uint32_t(rewritten);
                    publishValue("IbtpLengthRewrites", bump(forgeWrites));
                } else {
                    publishValue("IbtpLengthFailures", bump(forgeWriteFails));
                    abort = true;   // a duplicate that cannot be rewritten cannot be waited for either
                }
            }
        }
        buffer->complete(kIODirectionInOut);
    }
    if (abort && featureLock) {
        // Nothing could be rewritten into this read (failed transfer, no hold of our own, short
        // write): drop the pending round so no further read is wrapped for it.
        IOSimpleLockLock(featureLock);
        Ibtp::dropRound(features);
        const uint32_t aborts = features.aborts;
        IOSimpleLockUnlock(featureLock);
        publishIfChanged("IbtpLengthAborts", aborts, abortsSeen);
    }
    const IOUSBHostCompletion original = delayed->original;
    IOFree(delayed, sizeof(*delayed));
    if (original.action)
        original.action(original.owner, parameter, status, bytesTransferred);
}

// The park sees the event that ends it through the always-on event tap: no budget of its own any
// more, the completion forwards everything and the stall guard in restoreAfterCreate still puts
// scanning back on the next host command if an ending never comes.
static IOReturn pipeIoHook(void *that, IOMemoryDescriptor *dataBuffer, uint32_t bytesTransferred,
                           IOUSBHostCompletion *completion, uint32_t completionTimeoutMs) {
    bool wrap = false, probe = false;
    const bool knownPipe = that == eventPipe;
    const uint64_t now = monotonicNs();
    const uint32_t watchLeft = __atomic_load_n(&watchBudget, __ATOMIC_RELAXED);
    const bool watchLive = watchLeft != 0 && watchArmedNs != 0 && now - watchArmedNs < watchSpanNs;
    // Until the event pipe is known, every read is a candidate: the one whose payload parses as an
    // HCI event is the pipe, and the read it was seen on is too late to be wrapped anyway. The budget
    // bounds this to a fixed number of reads, so a controller that never reports cannot keep the
    // whole pipe permanently hijacked.
    if (!eventPipe && __atomic_load_n(&probeLeft, __ATOMIC_RELAXED) && completion && dataBuffer &&
        dataBuffer->getLength() >= 6) {
        uint32_t left = __atomic_load_n(&probeLeft, __ATOMIC_RELAXED);
        while (left) {
            if (__atomic_compare_exchange_n(&probeLeft, &left, left - 1, false, __ATOMIC_RELAXED,
                                            __ATOMIC_RELAXED)) {
                probe = true;
                break;
            }
        }
    }
    if (completion && dataBuffer && (knownPipe || watchLive || probe)) {
        if (knownPipe) {
            // Once the event pipe is known, every read of it is wrapped. A budgeted tap was the
            // first 2.4.2.1 design and it starved: only reads a feature round happened to await
            // ever reached the completion, so the advertising witness never saw a report and the
            // create rewrite had no address to work with. Forging decisions stay inside
            // forgeEvent's own round logic, so an always-on tap cannot rewrite more than before,
            // and the ending event the park waits for now arrives reliably as well.
            wrap = true;
            if (featureLock) {
                IOSimpleLockLock(featureLock);
                Ibtp::sweepSlots(features, now);
                const uint32_t expired = features.expired, aborts = features.aborts;
                IOSimpleLockUnlock(featureLock);
                // Counters of their own: IbtpLengthFailures counts copies that could not be sent or
                // written back.
                publishIfChanged("IbtpLengthExpired", expired, expiredSeen);
                publishIfChanged("IbtpLengthAborts", aborts, abortsSeen);
            }
        }
        if (!wrap && watchLive && dataBuffer->getLength() >= Ibtp::kDataLengthLen) {
            uint32_t left = watchLeft;
            while (left) {
                if (__atomic_compare_exchange_n(&watchBudget, &left, left - 1, false,
                                                __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
                    wrap = true;
                    // Published once: this fires for every read the window takes over.
                    if (!watchWraps++)
                        publishValue("IbtpFeatureWraps", 1);
                    break;
                }
            }
        }
    }
    if (wrap || probe) {
        auto *delayed = static_cast<WrappedRead *>(IOMalloc(sizeof(WrappedRead)));
        if (delayed) {
            // The caller's completion is never modified: ours is a copy in the same allocation, so a
            // caller that reuses its structure cannot send a read pointing at a freed wrapper, and a
            // failed submit leaves the caller's structure as it was.
            delayed->original = *completion;
            delayed->buffer = dataBuffer;
            delayed->pipe = that;
            delayed->wrapped = *completion;
            delayed->wrapped.action = readCompletion;
            delayed->wrapped.owner = delayed;
            const IOReturn result = FunctionCast(pipeIoHook, pipeIoOrig)(that, dataBuffer, bytesTransferred,
                                                                        &delayed->wrapped, completionTimeoutMs);
            // A refused submit never calls the completion, so nothing else would free this:
            // IOUSBHostPipe::io returns kIOReturnSuccess only if the completion will be called.
            if (result != kIOReturnSuccess)
                IOFree(delayed, sizeof(*delayed));
            return result;
        }
    }
    return FunctionCast(pipeIoHook, pipeIoOrig)(that, dataBuffer, bytesTransferred, completion, completionTimeoutMs);
}

static const char *usbHostPaths[] {"/System/Library/Extensions/IOUSBHostFamily.kext/Contents/MacOS/IOUSBHostFamily"};
static KernelPatcher::KextInfo usbHostInfo {"com.apple.iokit.IOUSBHostFamily", usbHostPaths, 1, {true}, {}, KernelPatcher::KextInfo::Unloaded};

static const char *bootargOff[] = {"-ibtpoff"};
static const char *bootargDebug[] = {"-ibtpdbg"};
static const char *bootargBeta[] = {"-ibtpbeta"};

PluginConfiguration ADDPR(config) {
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery | LiluAPI::AllowSafeMode,
    bootargOff, arrsize(bootargOff),
    bootargDebug, arrsize(bootargDebug),
    bootargBeta, arrsize(bootargBeta),
    // Lilu disables a plugin outside this range. Each build declares only its own macOS version:
    // before 0.8.3 the Sequoia build declared Tahoe here too, so Lilu never started it on Sequoia.
#if defined(IBTP_SONOMA)
    KernelVersion::Sonoma,
    KernelVersion::Sonoma,
#elif defined(IBTP_SEQUOIA)
    KernelVersion::Sequoia,
    KernelVersion::Sequoia,
#else
    KernelVersion::Tahoe,
    KernelVersion::Tahoe,
#endif
    []() {
        // On by default: Lilu already turns the whole plugin off on -ibtpoff, so patchScanning is
        // the only switch left for the page rewrites.
        scanPatchOn = !checkKernelArgument("-btlenoscan");
        parkOn = checkKernelArgument("-ibtppark");
        widenOn = checkKernelArgument("-ibtpwiden");
        privacyOn = checkKernelArgument("-ibtpprivacy");
        rpaOn = !checkKernelArgument("-ibtpnorpa");
        publishValue("IbtpParkEnabled", parkOn ? 1 : 0);
        publishValue("IbtpWidenEnabled", widenOn ? 1 : 0);
        publishValue("IbtpPrivacyEnabled", privacyOn ? 1 : 0);
        publishValue("IbtpRpaEnabled", rpaOn ? 1 : 0);
        // Iteration tag, not a bundle version. The kernel's kext version parser takes three numeric
        // fields and stops - measured, not assumed: with CFBundleVersion set to "2.4.2.1", kmutil
        // silently dropped the whole bundle from its scan ("No extension with identifier"), which
        // would have failed the injection quietly. So the bundle version was frozen at 2.4.2 while
        // the in-flight iteration lived here and in the archive name; this finalized build moves
        // the real version to 2.4.3.
        IORegistryEntry::getRegistryRoot()->setProperty("IbtpBuild", "2.4.3");
        // Arm the feature-word watch here, not when an ask shows up: the ask is a host command this
        // hook may never be called for, and arming on it left the watch dead for the whole boot when
        // it never came - indistinguishable from "the answer is not on a read". Reads inside the
        // window pass through untouched.
        watchSpanNs = kWatchSpanNs;
        watchArmedNs = monotonicNs();
        __atomic_store_n(&watchBudget, kWatchReads, __ATOMIC_RELAXED);
        __atomic_store_n(&probeLeft, kProbeReads, __ATOMIC_RELAXED);
        publishValue("IbtpFeatureArmed", 1);
        lilu.onPatcherLoadForce([](void *, KernelPatcher &patcher) {
            KernelPatcher::RouteRequest route("_cs_validate_page", csValidatePageHook, csValidateOrig);
            if (!patcher.routeMultipleLong(KernelPatcher::KernelID, &route, 1))
                SYSLOG("ibtp", "failed to route cs validation");
        });
        mirrorLock = IOLockAlloc();
        featureLock = IOSimpleLockAlloc();
        if (!featureLock)
            SYSLOG("ibtp", "no lock for the data length fix, it stays off");
        lilu.onKextLoadForce(&usbHostInfo, 1, [](void *, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
            if (index != usbHostInfo.loadIndex || !mirrorLock)
                return;
            KernelPatcher::RouteRequest route(
                "__ZN15IOUSBHostDevice13deviceRequestEP9IOServiceRN11StandardUSB13DeviceRequestEPvP18IOMemoryDescriptorRjP19IOUSBHostCompletionj",
                deviceRequestHook, deviceRequestOrig);
            const bool routed = patcher.routeMultiple(index, &route, 1, address, size);
            if (!routed) {
                SYSLOG("ibtp", "failed to route device request");
                patcher.clearError();
            }
            publishValue("IbtpHookRouted", routed ? 1 : 0);
            // The LE Data Length Change fix also needs the pipe object and its completions.
            KernelPatcher::RouteRequest ioRoute("__ZN13IOUSBHostPipe2ioEP18IOMemoryDescriptorjP19IOUSBHostCompletionj",
                                                pipeIoHook, pipeIoOrig);
            const bool ioRouted = patcher.routeMultiple(index, &ioRoute, 1, address, size);
            if (!ioRouted) {
                SYSLOG("ibtp", "failed to route pipe io");
                patcher.clearError();
            }
            // 26.5 and later take the configuration descriptor where older builds take the endpoint
            // companion one. Only one of the two exists, so a miss is not an error.
            KernelPatcher::RouteRequest configRoute("__ZN13IOUSBHostPipe28initWithDescriptorsAndOwnersEPKN11StandardUSB18EndpointDescriptorEPKNS0_23ConfigurationDescriptorEP22AppleUSBHostControllerP15IOUSBHostDeviceP18IOUSBHostInterfaceht",
                                                    initPipeConfigHook, initPipeConfigOrig);
            const bool configRouted = patcher.routeMultiple(index, &configRoute, 1, address, size);
            if (!configRouted)
                patcher.clearError();
            KernelPatcher::RouteRequest initRoute("__ZN13IOUSBHostPipe28initWithDescriptorsAndOwnersEPKN11StandardUSB18EndpointDescriptorEPKNS0_37SuperSpeedEndpointCompanionDescriptorEP22AppleUSBHostControllerP15IOUSBHostDeviceP18IOUSBHostInterfaceht",
                                                  initPipeHook, initPipeOrig);
            const bool initRouted = patcher.routeMultiple(index, &initRoute, 1, address, size);
            if (!initRouted)
                patcher.clearError();
            if (!configRouted && !initRouted)
                SYSLOG("ibtp", "failed to route pipe creation");
            publishValue("IbtpPipeHooks", (configRouted ? 1U : 0U) | (initRouted ? 2U : 0U) | (ioRouted ? 4U : 0U));
        });
    }
};

// Match Xcode's kext lifecycle. The kernel runs C++ initializers; libkmod forwards the start/stop
// callbacks through _realmain and _antimain.
extern "C" {
extern kern_return_t _start(kmod_info_t *, void *);
extern kern_return_t _stop(kmod_info_t *, void *);
extern kern_return_t IntelBTPatcher_kern_start(kmod_info_t *, void *);
extern kern_return_t IntelBTPatcher_kern_stop(kmod_info_t *, void *);
#ifdef IBTP_XCODE_KEXT_INFO
// Xcode's kernel-extension target emits _kmod_info (name and version from the MODULE_NAME /
// MODULE_VERSION build settings), so only the entry points are left here.
kern_return_t _start(kmod_info_t *ki, void *data) { return IntelBTPatcher_kern_start(ki, data); }
kern_return_t _stop(kmod_info_t *ki, void *data) { return IntelBTPatcher_kern_stop(ki, data); }
#else
// The kmod name must equal CFBundleIdentifier in the built Info.plist.
__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL(com.zxystd.IntelBTPatcher, "2.4.2", _start, _stop)
__attribute__((visibility("hidden"))) kmod_start_func_t *_realmain = IntelBTPatcher_kern_start;
__attribute__((visibility("hidden"))) kmod_stop_func_t *_antimain = IntelBTPatcher_kern_stop;
__attribute__((visibility("hidden"))) int _kext_apple_cc = __APPLE_CC__;
#endif
}
