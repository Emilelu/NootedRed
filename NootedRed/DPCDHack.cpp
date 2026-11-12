// AMD Display DPCD overrides
//
// Apple's DC picks the link rate from the receiver-capability block (DPCD 0x000..0x00F).
// Its legacy MAX_LINK_RATE byte at 0x001 is what falls back to HBR2, even when the panel
// advertises HBR3 in the eDP SUPPORTED_LINK_RATES table at 0x220. This patches only the
// in-memory copy of that reply, so neither the panel nor the link hardware is touched.
//
// Copyright (C) 2026  ChefKiss.
//
// Licensed under the ChefKiss Open Knowledge License version 1.
//
// This work is licensed to you under the terms of the ChefKiss Open Knowledge
// License, either version 1 of the License, or (at your option) any later
// version published by ChefKiss.
//
// This work may not be used to train or develop artificial intelligence,
// and may not be used with artificial intelligence in any other way. See
// the ChefKiss Open Knowledge License for details.
//
// This work is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// ChefKiss Open Knowledge License for more details.
//
// You should have received a copy of the ChefKiss Open Knowledge License
// along with this work.  If not, see https://git.chefkiss.dev/ChefKiss/OpenKnowledge/raw/branch/master/LICENSE-v1.md.

#include <DPCDHack.hpp>
#include <Headers/kern_patcher.hpp>
#include <Headers/kern_util.hpp>
#include <Kexts.hpp>
#include <PenguinWizardry/PatcherPlus.hpp>
#include <libkern/OSTypes.h>

namespace DPCDHack
{

namespace
{

[[maybe_unused]] constexpr UInt8 kLegacyMaxRateHBR3 = 0x1E;  // 8.1 Gbps/lane
constexpr UInt32 kDpcdReceiverCaps  = 0x000; // 0x000..0x00F
[[maybe_unused]] constexpr UInt32 kEdpLinkRateTable  = 0x220; // eDP 1.4, 16 bytes, 8 entries x 2 bytes LE, 200 kHz units
[[maybe_unused]] constexpr UInt16 kHBR3RateCode      = 40500; // 0x9E34 = 8.1 Gbps

bool  enabled      = false;
void* embeddedLink = nullptr;

// DC status is 0 on success, and so is kIOReturnSuccess, so a plain zero check covers both.
using ReadDpcdFn      = UInt32 (*)(void* link, UInt32 address, UInt8* data, UInt32 size);
ReadDpcdFn orgReadDpcd = nullptr;

[[maybe_unused]] bool tableHasHBR3(const UInt8* data, const UInt32 size)
{
    for (UInt32 off = 0; off + 1 < size; off += 2) {
        const UInt16 code = static_cast<UInt16>(data[off]) | (static_cast<UInt16>(data[off + 1]) << 8);
        if (code == kHBR3RateCode) { return true; }
    }

    return false;
}

UInt32 wrapReadDpcd(void* link, UInt32 address, UInt8* data, UInt32 size)
{
    const UInt32 ret = FunctionCast(wrapReadDpcd, orgReadDpcd)(link, address, data, size);
    if (ret != 0 || data == nullptr) { return ret; }

    // Log only, never modify: this is what the panel actually advertises.
    if (address == kEdpLinkRateTable && size >= 16) {
        DBGLOG("dpcd", "eDP link-rate table: %02x %02x %02x %02x %02x %02x %02x %02x "
                       "%02x %02x %02x %02x %02x %02x %02x %02x (HBR3 %s)",
               data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7],
               data[8], data[9], data[10], data[11], data[12], data[13], data[14], data[15],
               tableHasHBR3(data, size) ? "present" : "absent");
    }

    if (!enabled || link != embeddedLink) { return ret; }

    if (address == kDpcdReceiverCaps && size >= 0x10) {
        const UInt8 legacy = data[0x01];
        const UInt8 lanes  = static_cast<UInt8>(data[0x02] & 0x1F);
        if (lanes != 4) {
            DBGLOG("dpcd", "sink reports %u lanes, leaving HBR3 alone", lanes);
        }
        else if (legacy != 0 && legacy < kLegacyMaxRateHBR3) {
            DBGLOG("dpcd", "legacy MAX_LINK_RATE %#x -> %#x", legacy, kLegacyMaxRateHBR3);
            data[0x01] = kLegacyMaxRateHBR3;
        }
    }

    return ret;
}

} // namespace

void init()
{
    enabled = checkKernelArgument("-nrhbr3");

    if (enabled) { DBGLOG("dpcd", "HBR3 override enabled"); }
}

void noteEdpLink(void* link)
{
    embeddedLink = link;
    DBGLOG("dpcd", "embedded link recorded for HBR3 override: %p", link);
}

void processKext(KernelPatcher& patcher, const size_t id, const mach_vm_address_t slide, const size_t size)
{
    if (!enabled || kextRadeonX6000Framebuffer.loadIndex != id) { return; }

    PenguinWizardry::PatternRouteRequest requests[] = {
        {"_core_link_read_dpcd", wrapReadDpcd, orgReadDpcd},
    };

    if (!PenguinWizardry::PatternRouteRequest::routeAll(patcher, id, requests, slide, size)) {
        // Symbol stripped on this build: fall back to a pattern route, but never panic.
        // A missing optional link-rate override must not take the whole boot down.
        SYSLOG("dpcd", "failed to route _core_link_read_dpcd, HBR3 override inactive");
        patcher.clearError();
    }
}

} // namespace DPCDHack
