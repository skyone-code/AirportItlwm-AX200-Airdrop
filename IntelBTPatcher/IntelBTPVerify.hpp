// SPDX-License-Identifier: GPL-3.0-only
// Keeps APFS's integrity check working on the bluetoothd pages this plugin rewrites (#62).
//
// The rewrites below happen in the shared file cache, on each page as it is validated, so the page
// bluetoothd maps is the one that was changed. bluetoothd is stored compressed on the sealed system
// volume, and a compressed block is decompressed and checked as a whole: when macOS later drops one
// page of such a block and reloads it, decmpfs decompresses only the missing page and hands APFS the
// whole block to hash - the pages that are still rewritten included. The hash no longer matches, the
// reload is refused ("Verification failed with error 94"), and bluetoothd dies with SIGBUS
// (KERN_MEMORY_ERROR). It looks like Bluetooth switching itself off hours into a session.
//
// So every page a rewrite changed is recorded here, original bytes and rewritten bytes. When APFS is
// about to check a buffer that still holds rewritten bytes, it is handed a copy with the original
// bytes put back. What bluetoothd maps keeps the rewrites; only the check sees the file as it is on
// disk.
//
// Kept free of kernel types, so the matching can be exercised on the host.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
namespace Ibtp {
// The granularity APFS checks and cs_validate_page reports: one page.
static constexpr size_t kVerifyPage = 4096;
// Tahoe's bluetoothd has six rewritten pages in this build (four of them the capability sites).
// The spare room covers a reload that lands on a page the first pass never saw.
static constexpr size_t kMaxPatchedPages = 12;
struct PatchedPage {
    uint64_t offset;                    // file offset of the page
    uint8_t original[kVerifyPage];      // as it came from disk
    uint8_t patched[kVerifyPage];       // as the rewrites left it
};
struct PatchedPages {
    PatchedPage page[kMaxPatchedPages];
    size_t count;
    uint32_t full;                      // pages dropped for lack of room
};
// Records a page the rewrites changed. A page seen again (dropped, then reloaded) replaces its own
// record; a page that came back identical to its original drops out of the set.
inline bool recordPatchedPage(PatchedPages &pages, uint64_t offset, const uint8_t *before, const uint8_t *after) {
    if (!before || !after || (offset % kVerifyPage))
        return false;
    size_t i = 0;
    while (i < pages.count && pages.page[i].offset != offset)
        i++;
    if (i == pages.count) {
        if (pages.count == kMaxPatchedPages) {
            pages.full++;
            return false;
        }
        pages.count++;
    }
    if (!memcmp(before, after, kVerifyPage)) {
        // Back to the bytes on disk: nothing to put back for this page any more.
        for (size_t j = i + 1; j < pages.count; j++)
            pages.page[j - 1] = pages.page[j];
        pages.count--;
        return false;
    }
    pages.page[i].offset = offset;
    memcpy(pages.page[i].original, before, kVerifyPage);
    memcpy(pages.page[i].patched, after, kVerifyPage);
    return true;
}
// The part of a recorded page inside [foffset, foffset + size): where it starts in the buffer, where
// in the page, and how long. False when they do not overlap.
struct Overlap {
    size_t bufferStart, pageStart, length;
};
inline bool overlapOf(const PatchedPage &p, uint64_t foffset, size_t size, Overlap &o) {
    const uint64_t start = p.offset > foffset ? p.offset : foffset;
    const uint64_t pageEnd = p.offset + kVerifyPage, bufferEnd = foffset + size;
    const uint64_t end = pageEnd < bufferEnd ? pageEnd : bufferEnd;
    if (!size || start >= end)
        return false;
    o = {size_t(start - foffset), size_t(start - p.offset), size_t(end - start)};
    return true;
}
// True when the buffer holds a recorded page as the rewrites left it, so the check needs the
// original bytes. A page that is already the original (decompressed just now), or neither, is left
// alone - that keeps the hook out of the way of every ordinary read.
inline bool holdsPatchedBytes(const PatchedPages &pages, uint64_t foffset, const uint8_t *buf, size_t size) {
    if (!buf)
        return false;
    for (size_t i = 0; i < pages.count; i++) {
        Overlap o;
        const PatchedPage &p = pages.page[i];
        if (overlapOf(p, foffset, size, o) && memcmp(p.original + o.pageStart, p.patched + o.pageStart, o.length) &&
            !memcmp(buf + o.bufferStart, p.patched + o.pageStart, o.length))
            return true;
    }
    return false;
}
// In a copy of the buffer, puts the original bytes of every recorded page found rewritten back.
// Returns how many it put back.
inline size_t restoreOriginalBytes(const PatchedPages &pages, uint64_t foffset, uint8_t *copy, size_t size) {
    size_t restored = 0;
    if (!copy)
        return 0;
    for (size_t i = 0; i < pages.count; i++) {
        Overlap o;
        const PatchedPage &p = pages.page[i];
        if (overlapOf(p, foffset, size, o) && memcmp(p.original + o.pageStart, p.patched + o.pageStart, o.length) &&
            !memcmp(copy + o.bufferStart, p.patched + o.pageStart, o.length)) {
            memcpy(copy + o.bufferStart, p.original + o.pageStart, o.length);
            restored++;
        }
    }
    return restored;
}
}
