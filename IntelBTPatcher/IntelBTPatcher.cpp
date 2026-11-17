// SPDX-License-Identifier: GPL-3.0-only
// The HCI hook follows IntelBTPatcher (OpenIntelWireless/IntelBluetoothFirmware, GPLv3),
// the page hook follows BlueToolFixup (acidanthera/BrcmPatchRAM).
//
// Separate Lilu plugin: lets bluetoothd use extended LE advertising on a third-party
// controller. It only patches a process in memory, and does nothing without its boot
// flag. See docs/HOW-IT-WORKS.md. Nothing on disk changes.
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

// Patches bluetoothd's pages as they're validated, the same way BlueToolFixup does.
// Lilu's onProcLoad route was tried first and didn't apply to this process.
static mach_vm_address_t orig_cs_validate {};
static bool patchScanning {};
// Defined further down with the rest of the telemetry; the page hook publishes to it from here.
static void noteHook(const char *key, uint32_t value);
// Parking is opt-in since 2.7.9 (see wantsPreScanOff): off by default because it never made a
// create succeed and it demonstrably killed Instant Hotspot by switching off the advertising
// the phone needs to see. Add -ibtppark to bring the experiment back.
static bool parkScanning {};
// Both HCI interventions are opt-in, so the default build behaves exactly like upstream
// IntelBTPatcher 0.9.1: no park, no rewrite of the host's own create command. The reason is
// field evidence, not caution in the abstract — the feature they were written for (Instant
// Hotspot) is listed as unimplemented in upstream's ROADMAP under "Later", so the failures
// they were meant to cure are the expected baseline here, not a regression to chase. Add
// -ibtpwiden to experiment with the widened initiating-scan window.
static bool widenScan {};
// -ibtpprivacy: OR the LL Privacy bit (bit 6, LE feature word byte 0) into the answer of
// LE Read Local Supported Features (0x2003). Observation is unconditional; the rewrite only
// happens with the flag. bluetoothd uses the controller's resolving list only when this bit
// is set, and it never is on this controller — which is exactly why every outgoing create
// with filter policy 1 scans six seconds and dies: the accept list holds identity (Public)
// addresses while every peer advertises with an RPA (Random) nobody can resolve.
static bool privacyFix {};

static void patched_cs_validate_page(vnode_t vp, memory_object_t pager, memory_object_offset_t page_offset,
                                     const void *data, int *validated_p, int *tainted_p, int *nx_p) {
    char path[PATH_MAX];
    int pathlen = PATH_MAX;
    FunctionCast(patched_cs_validate_page, orig_cs_validate)(vp, pager, page_offset, data, validated_p, tainted_p, nx_p);
    if (vn_getpath(vp, path, &pathlen) != 0 || LIKELY(strcmp(path, "/usr/sbin/bluetoothd") != 0))
        return;
    // Each set only matches its own bluetoothd build; on any other build nothing changes.
    for (const auto &patch : Ibtp::kPatches)
        if (!patch.scanningOnly || patchScanning)
            KernelPatcher::findAndReplace(const_cast<void *>(data), PAGE_SIZE, patch.find, patch.size,
                                          patch.replace, patch.size);
}

// bluetoothd talks to the controller over USB control requests. Commands it doesn't send in
// the required order are added here, after the command they depend on. Their completion
// events reach bluetoothd as unmatched, as they already do for IntelBTPatcher's command.
static mach_vm_address_t orig_device_request {};
// deviceRequest runs on many threads at once. hciState is only read or changed with hciLock held,
// never across a USB call, and belongs to the one controller in hciOwner.
static Ibtp::State hciState {};
static IOLock *hciLock {};
static void *hciOwner {};
// An added command never waits longer than this, even when the caller's request had no timeout.
static constexpr uint32_t kExtraTimeoutMS = 1000;

// The LE Data Length Change fix (OpenIntelWireless#514) needs the controller's interrupt IN pipe,
// which is only reachable through IOUSBHostPipe: the pipe is remembered when it's created.
static mach_vm_address_t orig_pipe_io {};
static mach_vm_address_t orig_init_pipe {};
static mach_vm_address_t orig_init_pipe_config {};
static void *interruptPipe {};
// Written when a command goes out (any thread) and read when an interrupt read is submitted or
// completes (the USB workloop). lengthLock is a spinlock held only around the table itself: never
// across a USB call, an allocation or a registry write. Taken after hciLock when both are needed,
// never before. Entries are per connection handle and expire after 2 s.
static Ibtp::DataLengthState lengthState {};
static IOSimpleLock *lengthLock {};
static uint32_t lengthRewrites {}, lengthFailures {}, lengthExpiredSeen {}, lengthAbortsSeen {};
static uint32_t bumpCounter(uint32_t &counter) { return __atomic_add_fetch(&counter, 1, __ATOMIC_RELAXED); }
static uint64_t nowNanoseconds() {
    uint64_t now = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &now);
    return now;
}
// Scanning parked for an LE Create Connection, restored once the attempt ends. Written on the
// USB workloop (event side), read on the callers' threads (command side); same benign-race
// reasoning as lengthState above.
static Ibtp::ScanResumeState scanResume {};
static uint32_t scanSuspends {}, scanResumes {}, scanResumeFails {}, leCreates {}, watchdogResumes {}, unexpectedParks {},
    widenedCreates {}, widenSkipped {};
// The controller's own LE feature word from LE Read Local Supported Features (0x2003), as a
// 64-bit value, published once at startup. Bit 6 of byte 0 is LL Privacy.
static uint64_t lastLEFeatures {};
static uint32_t leFeatureReads {}, privacyRewrites {}, featureWriteFails {};
// The answer to 0x2003 usually lands on a read nothing else wraps (no data-length round armed, no
// park open). The pipe it comes in on cannot be named either: routing IOUSBHostPipe::init* failed
// on this build (PipeHooks lacks bit 1), so interruptPipe is never captured and no read is ever
// wrapped. A bounded window instead: reads are taken over for a wall-clock span or a read count,
// whichever comes first, and passed through untouched unless they carry the answer. It is armed at
// load, not only when the ask shows up — the ask rides a host command this hook may never see, and
// waiting for it meant an 800 ms window opened on nothing and captured nothing. Twenty seconds covers
// the whole early HCI init; every ask seen afterwards re-arms it shorter, right where the answer is due.
enum : uint32_t { kFeatureWatchReads = 8000 };
enum : uint64_t { kFeatureWatchNs = 20000000000ULL };
enum : uint64_t { kFeatureRearmNs = 3000000000ULL };
static uint32_t featureWatchReads {}, featureAsks {}, featureWraps {};
static uint64_t featureWatchArmedNs {};
// How long the window opened at that instant lasts: the load-time watch is wide, each ask shortens it
// to just around the answer.
static uint64_t featureWatchSpanNs { kFeatureWatchNs };
// Live view of what the mirror believes: bit0 legacy scan, bit8 extended scan, bit16 legacy
// advertising, bit24 extended advertising. Written only when it changes, so a desync between
// the kext and the radio is visible in ioreg instead of having to be inferred.
static uint32_t lastMirrorBits = 0xffffffff;
static void observeMirrors(const Ibtp::State &s) {
    const uint32_t bits = uint32_t(s.scanEnable) | uint32_t(s.extScanEnable) << 8 |
                          uint32_t(s.legacyAdvEnabled) << 16 | uint32_t(s.extAdvEnabled) << 24;
    if (bits == lastMirrorBits)
        return;
    lastMirrorBits = bits;
    IORegistryEntry::getRegistryRoot()->setProperty("IbtpMirrors", bits, 32);
}

static IOReturn patched_device_request(void *that, IOService *provider, StandardUSB::DeviceRequest &request, void *data,
                                       IOMemoryDescriptor *descriptor, unsigned int &length,
                                       IOUSBHostCompletion *completion, uint32_t timeout);

// Evidence for tests, in the I/O Registry root. Set only on rare events, in thread context.
static uint32_t extraCommands {}, setAddresses {}, extraFailures {};
static void noteHook(const char *key, uint32_t value) {
    IORegistryEntry::getRegistryRoot()->setProperty(key, value, 32);
}
// Continuity broadcast witness: the parsing lives in Ibtp so the host test can drive it,
// this file only publishes the result. Both frames exist solely on the air, so the HCI layer
// is the one honest place to ask — does the Mac really broadcast the 0x0D Wi-Fi Settings
// invite, and does the phone answer with 0x0E? Read-only: nothing here alters a byte.
static uint32_t advDataSets, advReports, tetherSources, lastAdvRssi;
static uint32_t advDataTypes, advReportAppleTypes, advReportEventTypes;
static uint32_t publishedAdvAppleTypes, publishedAdvEventTypes;
static bool inviteCaptured;
static void noteAdvData(const uint8_t *hci, size_t length) {
    Ibtp::AdvPayload payload {};
    if (!Ibtp::advPayload(hci, length, &payload))
        return;
    uint32_t types = 0;
    Ibtp::continuityTypes(hci + payload.offset, payload.length, &types);
    if (!types)
        return;
    advDataTypes |= types;
    noteHook("IbtpAdvDataTypes", advDataTypes);
    noteHook("IbtpAdvDataSets", ++advDataSets);
    // Keep one real 0x0D frame so the payload can be read back byte for byte.
    if ((types & 1U << 0x0d) && !inviteCaptured) {
        inviteCaptured = true;
        if (OSData *raw = OSData::withBytes(hci + payload.offset, uint32_t(payload.length))) {
            IORegistryEntry::getRegistryRoot()->setProperty("IbtpInviteFrame", raw);
            raw->release();
        }
    }
}
static void noteAdvReport(const uint8_t *event, size_t size) {
    Ibtp::AdvReportItem items[8] {};
    const int count = Ibtp::advReportItems(event, size, items, 8);
    if (count <= 0)
        return;
    uint32_t types = 0;
    for (int i = 0; i < count; ++i) {
        types |= items[i].appleTypes;
        if (items[i].eventType < 32)
            advReportEventTypes |= 1U << items[i].eventType;
    }
    lastAdvRssi = uint32_t(items[count - 1].rssi);
    advReportAppleTypes |= types;
    if (advReportAppleTypes != publishedAdvAppleTypes || advReportEventTypes != publishedAdvEventTypes) {
        publishedAdvAppleTypes = advReportAppleTypes;
        publishedAdvEventTypes = advReportEventTypes;
        noteHook("IbtpAdvReportAppleTypes", advReportAppleTypes);
        noteHook("IbtpAdvReportEventTypes", advReportEventTypes);
    }
    if (types & 1U << 0x0e) {
        // The phone answered "I have a hotspot". The menu entry, the rapport link and the
        // silent PSK all hang off this frame ever being seen.
        noteHook("IbtpTetherSources", ++tetherSources);
        noteHook("IbtpTetherRssi", lastAdvRssi);
        if (OSData *raw = OSData::withBytes(items[count - 1].addr, 6)) {
            IORegistryEntry::getRegistryRoot()->setProperty("IbtpTetherAddr", raw);
            raw->release();
        }
    }
    if (!(++advReports & 15))
        noteHook("IbtpAdvReports", advReports);
}
// HCI-witness diagnostics: what the create asked for and what the controller answered.
static uint32_t leOpFingerprint;
static uint16_t lastLeFailOp;
static uint8_t lastLeFailStatus;
static uint32_t leOpFails;
static uint16_t lastCreateOpcode;
static uint8_t lastCreateFilterPolicy, lastCreateAddrType, lastCreateAddr[6];
static uint32_t createCmdCompletes, connCompletes, connOk, createStatusEvents, cancelEvents;
static uint8_t lastCreateCmdStatus, lastConnStatus;
static void noteCreateWitness(const uint8_t *hci, size_t length) {
    if (!hci || length < 3)
        return;
    const uint16_t op = uint16_t(hci[0] | hci[1] << 8);
    lastCreateOpcode = op;
    if (op == Ibtp::kOpLeExtCreateConn && length >= 11) {
        // Params: filter policy(1), address type(1), address(6), ...
        lastCreateFilterPolicy = hci[3];
        lastCreateAddrType = hci[4];
        memcpy(lastCreateAddr, hci + 5, 6);
    } else if (op == Ibtp::kOpLeCreateConn && length >= 15) {
        // Params: scan interval(2), scan window(2), filter policy(1), address type(1), address(6).
        lastCreateFilterPolicy = hci[7];
        lastCreateAddrType = hci[8];
        memcpy(lastCreateAddr, hci + 9, 6);
    } else
        return;
    IORegistryEntry::getRegistryRoot()->setProperty("IbtpCreateOpcode", lastCreateOpcode, 16);
    IORegistryEntry::getRegistryRoot()->setProperty("IbtpCreateFilterPolicy", lastCreateFilterPolicy, 8);
    IORegistryEntry::getRegistryRoot()->setProperty("IbtpCreateAddrType", lastCreateAddrType, 8);
    IORegistryEntry::getRegistryRoot()->setProperty("IbtpCreatePeerAddr", lastCreateAddr, 6);
}

// Intel's vendor ID is also on hubs and other devices. Without the controller's product name, it
// must at least be a wireless controller (USB class 0xE0).
static bool isIntelBluetooth(void *that) {
    auto *device = static_cast<IORegistryEntry *>(that);
    auto *vendor = OSDynamicCast(OSNumber, device->getProperty("idVendor"));
    if (!vendor || vendor->unsigned32BitValue() != 0x8087)
        return false;
    if (auto *name = OSDynamicCast(OSString, device->getProperty("USB Product Name")))
        return name->isEqualTo("Bluetooth USB Host Controller");
    auto *deviceClass = OSDynamicCast(OSNumber, device->getProperty("bDeviceClass"));
    return deviceClass && deviceClass->unsigned32BitValue() == 0xE0;
}

static IOReturn sendExtra(void *that, IOService *provider, const StandardUSB::DeviceRequest &request,
                          const uint8_t *bytes, size_t size, uint32_t timeout) {
    auto *extra = IOBufferMemoryDescriptor::withBytes(bytes, size, kIODirectionOut);
    if (!extra)
        return kIOReturnNoMemory;
    StandardUSB::DeviceRequest extraRequest = request;
    extraRequest.wLength = uint16_t(size);
    unsigned int extraLength = uint32_t(size);
    // Synchronous (no completion), so bounded: a stalled controller can't hold the thread forever.
    const uint32_t extraTimeout = timeout == 0 || timeout > kExtraTimeoutMS ? kExtraTimeoutMS : timeout;
    IOReturn result = extra->prepare(kIODirectionOut);
    if (result == kIOReturnSuccess) {
        result = FunctionCast(patched_device_request, orig_device_request)(that, provider, extraRequest, nullptr, extra,
                                                                           extraLength, nullptr, extraTimeout);
        extra->complete(kIODirectionOut);
    }
    extra->release();
    return result;
}

static IOReturn patched_device_request(void *that, IOService *provider, StandardUSB::DeviceRequest &request, void *data,
                                       IOMemoryDescriptor *descriptor, unsigned int &length,
                                       IOUSBHostCompletion *completion, uint32_t timeout) {
    uint8_t hci[32] = {};
    size_t hciLength = 0;
    // Every USB device comes through here, so only look at the Intel Bluetooth controller.
    // Class requests to the device carry HCI commands; bRequest isn't reliable for this.
    // HCI commands are class requests, host to device, addressed to the device (or its interface).
    if (hciLock && (request.bmRequestType == 0x20 || request.bmRequestType == 0x21) && isIntelBluetooth(that)) {
        if (data) {
            hciLength = min(size_t(request.wLength), sizeof(hci));
            lilu_os_memcpy(hci, data, hciLength);
        } else if (descriptor && descriptor->prepare(kIODirectionOut) == kIOReturnSuccess) {
            hciLength = min(size_t(descriptor->getLength()), sizeof(hci));
            descriptor->readBytes(0, hci, hciLength);
            descriptor->complete(kIODirectionOut);
        }
    }
    bool legacyScanAddress = false;
    unsigned remoteReads = 0;
    Ibtp::PreCommand pres[4] = {};
    int preCount = 0;
    bool resume = false;
    Ibtp::Action action {};
    if (hciLength) {
        IOLockLock(hciLock);
        // A different controller than last time starts from nothing.
        if (hciOwner != that) {
            hciState = Ibtp::State{};
            scanResume = Ibtp::ScanResumeState{};
            hciOwner = that;
            // A different controller object (re-enumerated, or a second one): the remembered pipe and
            // any pending duplicates belonged to the old one.
            interruptPipe = nullptr;
            if (lengthLock) {
                IOSimpleLockLock(lengthLock);
                const uint32_t expired = lengthState.expired, aborts = lengthState.aborts;
                lengthState = Ibtp::DataLengthState{};
                lengthState.expired = expired;
                lengthState.aborts = aborts;
                IOSimpleLockUnlock(lengthLock);
            }
        }
        const uint16_t op = hciLength >= 3 ? uint16_t(hci[0] | hci[1] << 8) : 0;
        // LE Read Local Supported Features: the answer is what decides the resolving-list route, and
        // it comes back on a read that may not be wrapped. Arm the bounded watch before it arrives.
        if (op == 0x2003) {
            featureWatchSpanNs = kFeatureRearmNs;
            featureWatchArmedNs = nowNanoseconds();
            // The budget only extends: an ask never grants the window unlimited reads.
            if (__atomic_load_n(&featureWatchReads, __ATOMIC_RELAXED) < 400)
                __atomic_store_n(&featureWatchReads, 400, __ATOMIC_RELAXED);
            noteHook("IbtpFeatureAsks", ++featureAsks);
        }
        // Witness every advertising payload the host publishes, before any decision here can
        // change a byte. This is the only way to see whether the 0x0D invite frame goes out.
        noteAdvData(hci, hciLength);
        // Which mechanism the host uses to reach the phone: address handed straight to the
        // controller, filter accept list, or resolving list. Read-only, and the only way to tell
        // them apart without a sniffer — every one of them produces the same create command.
        if (const uint32_t fp = Ibtp::opFingerprint(op)) {
            const uint32_t seen = leOpFingerprint;
            leOpFingerprint |= fp;
            if (leOpFingerprint != seen)
                noteHook("IbtpLeOps", leOpFingerprint);
        }
        if (op == Ibtp::kOpReset) {
            scanResume = Ibtp::ScanResumeState{};
            // A controller reset turned every scan/advertising engine off.
            hciState.scanEnable = hciState.extScanEnable = 0;
            hciState.legacyAdvEnabled = hciState.extAdvEnabled = false;
        }
        // A parked create attempt is resumed before anything else — but never in front of
        // another create, which would undo the park and reinstate the bug.
        const bool isCreate = op == Ibtp::kOpLeCreateConn || op == Ibtp::kOpLeExtCreateConn;
        if (isCreate) {
            noteHook("IbtpLeCreates", ++leCreates);
            noteCreateWitness(hci, hciLength);
        }
        if (isCreate) {
            // Park scanning and advertising for the create; the helper records what it
            // switched off so the attempt-ending event can restore exactly that.
            preCount = Ibtp::wantsPreScanOff(hciState, scanResume, hci, hciLength, pres, parkScanning);
            noteHook("IbtpLastParkCommands", uint32_t(preCount));
            observeMirrors(hciState);
        } else {
            // Capture what was parked before wantsScanResume clears the flags; the mirror must
            // follow what the radio actually runs again, or the next create finds mirrors of 0
            // and never parks — the desync that let the bug through every resume.
            const bool parkedLegacyScan = scanResume.parkedLegacyScan, parkedExtScan = scanResume.parkedExtScan,
                       parkedLegacyAdv = scanResume.parkedLegacyAdv, parkedExtAdv = scanResume.parkedExtAdv;
            if ((preCount = Ibtp::wantsScanResume(scanResume, pres)) > 0) {
                resume = true;
                if (scanResume.watchdogFired)
                    noteHook("IbtpResumeWatchdog", ++watchdogResumes);
                // Bring our mirrors in line with what the resume just re-enabled.
                if (parkedLegacyScan)
                    hciState.scanEnable = scanResume.enable;
                if (parkedExtScan)
                    hciState.extScanEnable = scanResume.extEnable;
                if (parkedLegacyAdv)
                    hciState.legacyAdvEnabled = true;
                if (parkedExtAdv)
                    hciState.extAdvEnabled = true;
            } else {
                // This helper is also the only place the host's scan/advertising enable commands
                // are mirrored. Skipping it for ordinary commands left every mirror at 0, so no
                // create ever parked — the regression that made 2.7.7 a no-op.
                preCount = Ibtp::wantsPreScanOff(hciState, scanResume, hci, hciLength, pres, parkScanning);
                if (preCount > 0) {  // a non-create op should never park; count it if one does
                    noteHook("IbtpUnexpectedParks", ++unexpectedParks);
                }
                observeMirrors(hciState);
            }
        }
        legacyScanAddress = Ibtp::wantsLegacyScanAddress(hciState, hci, hciLength);
        action = Ibtp::onCommand(hciState, hci, hciLength, patchScanning);
        IOLockUnlock(hciLock);
        if (lengthLock) {
            const uint64_t now = nowNanoseconds();
            IOSimpleLockLock(lengthLock);
            remoteReads = Ibtp::extraRemoteReads(lengthState, hci, hciLength, now);
            IOSimpleLockUnlock(lengthLock);
        }
        if (widenScan) {
            uint32_t observed = 0;
            if (Ibtp::widenCreateScan(hci, hciLength, &observed)) {
                noteHook("IbtpCreateScanParams", observed);
                // The command leaves from the caller's buffer and hci is only our shadow, so the
                // widened window has to be written back or the controller never sees it. Only
                // the plain data pointer is touched: writeBytes on the USB IOMemoryDescriptor
                // panics with "IOGMD: not wired for the IODMACommand" because the descriptor is
                // not wired for CPU access — that write-back is exactly what 2.7.10 crashed on.
                if (data) {
                    lilu_os_memcpy(data, hci, hciLength);
                    noteHook("IbtpWidened", ++widenedCreates);
                } else {
                    // Descriptor-only request: there is no safe way to rewrite it, so the host's
                    // own duty cycle stays. Counted so the telemetry says whether this path is
                    // the common one before touching anything else.
                    noteHook("IbtpWidenSkipped", ++widenSkipped);
                }
            }
        }
    }
    for (int i = 0; i < preCount; ++i) {
        if (sendExtra(that, provider, request, pres[i].data, pres[i].length, timeout) == kIOReturnSuccess) {
            if (resume)
                noteHook("IbtpScanResumes", ++scanResumes);
            else
                noteHook("IbtpScanSuspends", ++scanSuspends);
        } else {
            noteHook("IbtpScanResumeFails", ++scanResumeFails);
        }
    }
    for (unsigned i = 0; i < remoteReads; ++i) {
        // Sent before the real command, as IntelBTPatcher does: the three completions carry the
        // same features, so the first one reaches bluetoothd unchanged and the other two are
        // rewritten into the events it's missing.
        if (sendExtra(that, provider, request, hci, hciLength, timeout) != kIOReturnSuccess) {
            if (lengthLock) {
                IOSimpleLockLock(lengthLock);
                Ibtp::forgetRemoteRead(lengthState, hci, hciLength);
                IOSimpleLockUnlock(lengthLock);
            }
            noteHook("IbtpLengthFailures", bumpCounter(lengthFailures));
        }
    }
    if (legacyScanAddress) {
        // IntelBTPatcher's workaround, sent before the scan command like the original.
        uint8_t fixed[9] = {0x05, 0x20, 0x06};
        lilu_os_memcpy(fixed + 3, Ibtp::kFixedPatcherAddress, 6);
        sendExtra(that, provider, request, fixed, sizeof(fixed), timeout);
    }
    static bool sawCommand {};
    if (hciLength >= 3 && !sawCommand) {
        sawCommand = true;
        noteHook("IbtpFirstRequest", request.bRequest);
    }
    const IOReturn result = FunctionCast(patched_device_request, orig_device_request)(that, provider, request, data, descriptor, length, completion, timeout);
    if (action.sendSetAddress || action.sendEventMask) {
        // Only after the command it depends on was accepted. A set address that didn't go out is
        // forgotten, so the next parameters command for that set tries again.
        const IOReturn extra = result == kIOReturnSuccess ?
            sendExtra(that, provider, request, action.output, action.outputLength, timeout) : result;
        if (action.sendSetAddress && extra != kIOReturnSuccess) {
            IOLockLock(hciLock);
            if (hciOwner == that)
                hciState.addressedSets &= ~(1U << action.output[3]);
            IOLockUnlock(hciLock);
        }
        if (extra == kIOReturnSuccess) {
            noteHook("IbtpExtraCommands", ++extraCommands);
            if (action.sendSetAddress)
                noteHook("IbtpSetAddresses", ++setAddresses);
        } else {
            noteHook("IbtpExtraFailures", ++extraFailures);
        }
    }
    return result;
}

// The controller's events arrive on its interrupt IN pipe, and only the pipe object can be used
// to wrap them, so it's remembered when it's created. Anything else on the bus is left alone.
static void captureInterruptPipe(void *that, StandardUSB::EndpointDescriptor const *descriptor, IOUSBHostDevice *device) {
    if (!descriptor || !device || !isIntelBluetooth(device))
        return;
    // The endpoint is picked apart by hand: StandardUSB::getEndpointType/getEndpointDirection are
    // exports of IOUSBHostFamily, and OpenCore's prelinker resolves them to 0 (RIP-relative
    // overflow), which makes injection fail. The descriptor is the 7-byte USB layout:
    // bEndpointAddress (offset 2, bit 7 = direction in), bmAttributes (offset 3, bits 0-1 = type).
    auto *bytes = reinterpret_cast<const uint8_t *>(descriptor);
    if (bytes[2] & 0x80 && (bytes[3] & 0x03) == 0x03)
        interruptPipe = that;
}

// The pipe is created through one of two overloads, depending on the macOS version.
static int patched_init_pipe(void *that, StandardUSB::EndpointDescriptor const *descriptor,
                             StandardUSB::SuperSpeedEndpointCompanionDescriptor const *companion, void *controller,
                             IOUSBHostDevice *device, void *interface, unsigned char a7, unsigned short a8) {
    const int result = FunctionCast(patched_init_pipe, orig_init_pipe)(that, descriptor, companion, controller, device,
                                                                       interface, a7, a8);
    captureInterruptPipe(that, descriptor, device);
    return result;
}

static int patched_init_pipe_config(void *that, StandardUSB::EndpointDescriptor const *descriptor,
                                    StandardUSB::ConfigurationDescriptor const *config, void *controller,
                                    IOUSBHostDevice *device, void *interface, unsigned char a7, unsigned short a8) {
    const int result = FunctionCast(patched_init_pipe_config, orig_init_pipe_config)(that, descriptor, config, controller,
                                                                                     device, interface, a7, a8);
    captureInterruptPipe(that, descriptor, device);
    return result;
}

// A completion owns the caller's closure, so the original one is kept here while ours runs.
struct DelayedCompletion {
    IOUSBHostCompletion original;
    IOMemoryDescriptor *buffer;
    IOUSBHostCompletion wrapped;    // what the USB stack is given, so the caller's own is never edited
};

// Publishes a counter that changed, outside the spinlock (a registry write allocates).
static void noteChanged(const char *key, uint32_t value, uint32_t &seen) {
    if (value != __atomic_load_n(&seen, __ATOMIC_RELAXED)) {
        __atomic_store_n(&seen, value, __ATOMIC_RELAXED);
        noteHook(key, value);
    }
}

static void rewriteCompletion(void *owner, void *parameter, IOReturn status, uint32_t bytesTransferred) {
    auto *delayed = static_cast<DelayedCompletion *>(owner);
    IOMemoryDescriptor *buffer = delayed->buffer;
    // readBytes() and writeBytes() need the descriptor wired, and only a hold this call took is known
    // to be there: touching a buffer this call couldn't prepare panics with "IOGMD: not wired for the
    // IODMACommand". 0.9.1 assumed a refused prepare() meant "already prepared on Tahoe" and read
    // anyway, which was never tested; upstream's Tahoe run shows prepare() succeeds. A hold taken here
    // is also the only one released here.
    const bool held = status == kIOReturnSuccess && buffer &&
        buffer->getLength() >= Ibtp::kDataLengthEventLength &&
        buffer->prepare(kIODirectionInOut) == kIOReturnSuccess;
    bool abort = !held;
    // Set when a 0x2003 answer was rewritten in place at the same size: the shared write-back
    // below only fires on size changes, so a same-size edit needs its own.
    bool featureModified = false;
    if (held) {
        // Events shorter than 6 bytes can't be one of the rewritten ones; they pass through untouched.
        if (bytesTransferred >= 6) {
            uint8_t event[32] = {};
            const size_t size = min(min(size_t(bytesTransferred), sizeof(event)), size_t(buffer->getLength()));
            buffer->readBytes(0, event, size);
            // A connection attempt that just ended releases the scanning parked for it; the next
            // outgoing command carries the restore.
            Ibtp::onScanResumeEvent(scanResume, event, size);
            // HCI witness, unconditional: the controller's verdict on the last create, whether or not
            // a park is open. Gating it on the park meant a silent no-park run left no evidence at all
            // (2.7.7 went in blind that way).
            if (event[0] == 0x3e && size >= 5 && event[2] == 0x02) {
                // Advertising reports are pure observation: what the phone is saying, and whether
                // what it says is connectable, decides if any create can land.
                noteAdvReport(event, size);
            } else if (event[0] == 0x0e && size >= 8 &&
                uint16_t(event[3] | event[4] << 8) == lastCreateOpcode) {
                lastCreateCmdStatus = event[5];
                noteHook("IbtpCreateCmdStatus", lastCreateCmdStatus);
                noteHook("IbtpCreateCmdCompletes", ++createCmdCompletes);
            } else if (event[0] == 0x0f && size >= 6 &&
                       uint16_t(event[4] | event[5] << 8) == lastCreateOpcode) {
                // Command Status: the controller's verdict when it rejects the create outright.
                noteHook("IbtpCreateStatus", event[2]);
                noteHook("IbtpCreateStatusEvents", ++createStatusEvents);
            } else if (event[0] == 0x0e && size >= 11 &&
                       uint16_t(event[3] | event[4] << 8) == 0x2003) {
                // LE Read Local Supported Features: the controller's own word. Published raw —
                // whether bit 6 (LL Privacy) is honest decides the whole resolving-list route.
                lastLEFeatures = 0;
                for (int i = 0; i < 8; ++i)
                    lastLEFeatures |= uint64_t(event[5 + i]) << (8 * i);
                IORegistryEntry::getRegistryRoot()->setProperty("IbtpLEFeatures", lastLEFeatures, 64);
                noteHook("IbtpLEFeatureReads", ++leFeatureReads);
                if (privacyFix && !event[2] && !(event[5] & 0x40)) {
                    event[5] |= 0x40;
                    featureModified = true;
                    noteHook("IbtpPrivacyRewrites", bumpCounter(privacyRewrites));
                }
            } else if ((event[0] == 0x0e || event[0] == 0x0f) && size >= 6) {
                const uint16_t witnessOp = event[0] == 0x0f
                    ? uint16_t(event[4] | event[5] << 8)
                    : uint16_t(event[3] | event[4] << 8);
                if (witnessOp == Ibtp::kOpLeCreateConnCancel)
                    noteHook("IbtpCancelEvents", ++cancelEvents);
                // An LE command the controller rejects is a dead end bluetoothd may never retry, and
                // the rejection never reaches user space. Keeping the first one on record separates
                // "the link was never possible" from "the link was attempted and timed out".
                const uint8_t witnessStatus = event[0] == 0x0f ? event[2] : event[5];
                if (witnessStatus && (witnessOp & 0xfc00) == 0x2000) {
                    lastLeFailOp = witnessOp;
                    lastLeFailStatus = witnessStatus;
                    IORegistryEntry::getRegistryRoot()->setProperty("IbtpLeFailOp", lastLeFailOp, 16);
                    noteHook("IbtpLeFailStatus", lastLeFailStatus);
                    noteHook("IbtpLeFails", ++leOpFails);
                }
            } else if (event[0] == 0x3e && size >= 4 &&
                       (event[2] == 0x01 || event[2] == 0x0a || event[2] == 0x0b)) {
                lastConnStatus = event[3];
                noteHook("IbtpConnStatus", lastConnStatus);
                noteHook("IbtpConnCompletes", ++connCompletes);
                if (!lastConnStatus)
                    noteHook("IbtpConnOk", ++connOk);
            }
            // A parked round can wrap reads without a data-length round being armed, so the table and
            // its lock are not guaranteed to exist here; without the lock nothing is tracked at all.
            size_t rewritten = size;
            if (lengthLock) {
                IOSimpleLockLock(lengthLock);
                rewritten = Ibtp::rewriteEvent(lengthState, event, size);
                IOSimpleLockUnlock(lengthLock);
            }
            // writeBytes returns the number of bytes written, not an IOReturn. The shrunk size may
            // only be claimed when every rewritten byte is really in the buffer: forwarding the
            // original size leaves stale feature bytes behind, which the stack parses as garbage
            // events and bluetoothd takes the parser desync as fatal (reason 607).
            if (featureModified && rewritten == size) {
                if (buffer->writeBytes(0, event, rewritten) == rewritten) {
                    bytesTransferred = uint32_t(rewritten);
                } else {
                    noteHook("IbtpFeatureWriteFails", bumpCounter(featureWriteFails));
                    abort = true;
                }
            } else if (rewritten != size) {
                if (buffer->writeBytes(0, event, rewritten) == rewritten) {
                    bytesTransferred = uint32_t(rewritten);
                    noteHook("IbtpLengthRewrites", bumpCounter(lengthRewrites));
                } else {
                    noteHook("IbtpLengthFailures", bumpCounter(lengthFailures));
                    abort = true;   // a duplicate that can't be rewritten can't be waited for either
                }
            }
        }
        buffer->complete(kIODirectionInOut);
    }
    if (abort && lengthLock) {
        // Nothing could be rewritten into this read (failed transfer, no hold of our own, short
        // write): drop the pending round so no further read is wrapped for it.
        IOSimpleLockLock(lengthLock);
        Ibtp::abortLinks(lengthState);
        const uint32_t aborts = lengthState.aborts;
        IOSimpleLockUnlock(lengthLock);
        noteChanged("IbtpLengthAborts", aborts, lengthAbortsSeen);
    }
    const IOUSBHostCompletion original = delayed->original;
    IOFree(delayed, sizeof(*delayed));
    if (original.action)
        original.action(original.owner, parameter, status, bytesTransferred);
}

// The park needs to see the event that ends it, but 0.9.1 wrapped every read for as long as the park
// stayed open — the same unbounded hijack of the event pipe in another coat. So it gets its own
// budget: a read count and a wall-clock window. If it runs out, the stall guard in wantsScanResume
// restores scanning on the next host command, exactly as it already would have.
enum : uint32_t { kMaxParkWraps = 256 };
enum : uint64_t { kParkWrapBudgetNs = 1000000000ULL };
static uint32_t parkWraps {};
static uint64_t parkArmedNs {};

static IOReturn patched_pipe_io(void *that, IOMemoryDescriptor *dataBuffer, uint32_t bytesTransferred,
                                IOUSBHostCompletion *completion, uint32_t completionTimeoutMs) {
    bool wrap = false, parkWrap = false;
    const bool knownPipe = that == interruptPipe;
    const uint64_t now = nowNanoseconds();
    const uint32_t watchLeft = __atomic_load_n(&featureWatchReads, __ATOMIC_RELAXED);
    const bool watchLive = watchLeft != 0 && featureWatchArmedNs != 0 && now - featureWatchArmedNs < featureWatchSpanNs;
    if (completion && dataBuffer && (knownPipe || watchLive)) {
        if (knownPipe) {
            if (!scanResume.suspended) {
                parkWraps = 0;
                parkArmedNs = now;
            }
            if (lengthLock) {
                IOSimpleLockLock(lengthLock);
                wrap = Ibtp::wrapRead(lengthState, now, dataBuffer->getLength());
                const uint32_t expired = lengthState.expired, aborts = lengthState.aborts;
                IOSimpleLockUnlock(lengthLock);
                // Their own counters: IbtpLengthFailures counts copies that couldn't be sent or written back.
                noteChanged("IbtpLengthExpired", expired, lengthExpiredSeen);
                noteChanged("IbtpLengthAborts", aborts, lengthAbortsSeen);
            }
            if (!wrap && scanResume.suspended && parkWraps < kMaxParkWraps && now - parkArmedNs < kParkWrapBudgetNs &&
                dataBuffer->getLength() >= Ibtp::kDataLengthEventLength) {
                ++parkWraps;
                parkWrap = true;
            }
        }
        if (!wrap && !parkWrap && watchLive && dataBuffer->getLength() >= Ibtp::kDataLengthEventLength) {
            uint32_t left = watchLeft;
            while (left) {
                if (__atomic_compare_exchange_n(&featureWatchReads, &left, left - 1, false,
                                                __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
                    wrap = true;
                    // Published once: this fires for every read the window takes over.
                    if (!featureWraps++)
                        noteHook("IbtpFeatureWraps", 1);
                    break;
                }
            }
        }
    }
    if (wrap || parkWrap) {
        auto *delayed = static_cast<DelayedCompletion *>(IOMalloc(sizeof(DelayedCompletion)));
        if (delayed) {
            // The caller's completion is never modified: ours is a copy held in the same allocation, so
            // a caller that reuses its structure can't send a read pointing at a freed wrapper, and a
            // failed submit leaves the caller's structure as it was.
            delayed->original = *completion;
            delayed->buffer = dataBuffer;
            delayed->wrapped = *completion;
            delayed->wrapped.action = rewriteCompletion;
            delayed->wrapped.owner = delayed;
            const IOReturn result = FunctionCast(patched_pipe_io, orig_pipe_io)(that, dataBuffer, bytesTransferred,
                                                                               &delayed->wrapped, completionTimeoutMs);
            // A refused submit never calls the completion, so nothing else would free this:
            // IOUSBHostPipe::io returns kIOReturnSuccess only if the completion will be called.
            if (result != kIOReturnSuccess)
                IOFree(delayed, sizeof(*delayed));
            return result;
        }
    }
    return FunctionCast(patched_pipe_io, orig_pipe_io)(that, dataBuffer, bytesTransferred, completion, completionTimeoutMs);
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
        // On by default. Lilu disables the whole plugin when -ibtpoff is present
        // (bootargOff above); patchScanning is the only remaining user switch.
        patchScanning = !checkKernelArgument("-btlenoscan");
        parkScanning = checkKernelArgument("-ibtppark");
        widenScan = checkKernelArgument("-ibtpwiden");
        privacyFix = checkKernelArgument("-ibtpprivacy");
        noteHook("IbtpParkEnabled", parkScanning ? 1 : 0);
        noteHook("IbtpWidenEnabled", widenScan ? 1 : 0);
        noteHook("IbtpPrivacyEnabled", privacyFix ? 1 : 0);
        // Arm the feature-word watch here, not when an ask shows up. The ask is a host command this
        // hook may never be called for, and both 2.7.17 and 2.7.18 armed on it: if it never came, the
        // watch stayed dead for the whole boot and reported nothing, which looks the same as "the
        // answer is not on a read". Reads inside the window are passed through untouched.
        featureWatchSpanNs = kFeatureWatchNs;
        featureWatchArmedNs = nowNanoseconds();
        __atomic_store_n(&featureWatchReads, kFeatureWatchReads, __ATOMIC_RELAXED);
        noteHook("IbtpFeatureArmed", 1);
        lilu.onPatcherLoadForce([](void *, KernelPatcher &patcher) {
            KernelPatcher::RouteRequest route("_cs_validate_page", patched_cs_validate_page, orig_cs_validate);
            if (!patcher.routeMultipleLong(KernelPatcher::KernelID, &route, 1))
                SYSLOG("ibtp", "failed to route cs validation");
        });
        hciLock = IOLockAlloc();
        lengthLock = IOSimpleLockAlloc();
        if (!lengthLock)
            SYSLOG("ibtp", "no lock for the data length fix, it stays off");
        lilu.onKextLoadForce(&usbHostInfo, 1, [](void *, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
            if (index != usbHostInfo.loadIndex || !hciLock)
                return;
            KernelPatcher::RouteRequest route(
                "__ZN15IOUSBHostDevice13deviceRequestEP9IOServiceRN11StandardUSB13DeviceRequestEPvP18IOMemoryDescriptorRjP19IOUSBHostCompletionj",
                patched_device_request, orig_device_request);
            const bool routed = patcher.routeMultiple(index, &route, 1, address, size);
            if (!routed) {
                SYSLOG("ibtp", "failed to route device request");
                patcher.clearError();
            }
            noteHook("IbtpHookRouted", routed ? 1 : 0);
            // The LE Data Length Change fix also needs the pipe object and its completions.
            KernelPatcher::RouteRequest ioRoute("__ZN13IOUSBHostPipe2ioEP18IOMemoryDescriptorjP19IOUSBHostCompletionj",
                                                patched_pipe_io, orig_pipe_io);
            const bool ioRouted = patcher.routeMultiple(index, &ioRoute, 1, address, size);
            if (!ioRouted) {
                SYSLOG("ibtp", "failed to route pipe io");
                patcher.clearError();
            }
            // 26.5 and later take the configuration descriptor where older builds take the
            // endpoint companion one. Only one of the two exists, so a miss is not an error.
            KernelPatcher::RouteRequest configRoute("__ZN13IOUSBHostPipe28initWithDescriptorsAndOwnersEPKN11StandardUSB18EndpointDescriptorEPKNS0_23ConfigurationDescriptorEP22AppleUSBHostControllerP15IOUSBHostDeviceP18IOUSBHostInterfaceht",
                                                    patched_init_pipe_config, orig_init_pipe_config);
            const bool configRouted = patcher.routeMultiple(index, &configRoute, 1, address, size);
            if (!configRouted)
                patcher.clearError();
            KernelPatcher::RouteRequest initRoute("__ZN13IOUSBHostPipe28initWithDescriptorsAndOwnersEPKN11StandardUSB18EndpointDescriptorEPKNS0_37SuperSpeedEndpointCompanionDescriptorEP22AppleUSBHostControllerP15IOUSBHostDeviceP18IOUSBHostInterfaceht",
                                                  patched_init_pipe, orig_init_pipe);
            const bool initRouted = patcher.routeMultiple(index, &initRoute, 1, address, size);
            if (!initRouted)
                patcher.clearError();
            if (!configRouted && !initRouted)
                SYSLOG("ibtp", "failed to route pipe creation");
            noteHook("IbtpPipeHooks", (configRouted ? 1U : 0U) | (initRouted ? 2U : 0U) | (ioRouted ? 4U : 0U));
        });
    }
};

// Match Xcode's kext lifecycle. The kernel runs C++ initializers; libkmod
// forwards the start/stop callbacks through _realmain and _antimain.
extern "C" {
extern kern_return_t _start(kmod_info_t *, void *);
extern kern_return_t _stop(kmod_info_t *, void *);
extern kern_return_t IntelBTPatcher_kern_start(kmod_info_t *, void *);
extern kern_return_t IntelBTPatcher_kern_stop(kmod_info_t *, void *);
#ifdef IBTP_XCODE_KEXT_INFO
// Xcode's kernel-extension target emits _kmod_info (name and version taken from the
// MODULE_NAME / MODULE_VERSION build settings), so all that is left here are the entry points.
kern_return_t _start(kmod_info_t *ki, void *data) { return IntelBTPatcher_kern_start(ki, data); }
kern_return_t _stop(kmod_info_t *ki, void *data) { return IntelBTPatcher_kern_stop(ki, data); }
#else
// The kmod name must equal CFBundleIdentifier in the built Info.plist.
__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL(com.zxystd.IntelBTPatcher, "2.4.1", _start, _stop)
__attribute__((visibility("hidden"))) kmod_start_func_t *_realmain = IntelBTPatcher_kern_start;
__attribute__((visibility("hidden"))) kmod_stop_func_t *_antimain = IntelBTPatcher_kern_stop;
__attribute__((visibility("hidden"))) int _kext_apple_cc = __APPLE_CC__;
#endif
}
