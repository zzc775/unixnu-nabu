// SPDX-License-Identifier: BSD-2-Clause-Patent
// nabu (Xiaomi Pad 5 / SM8150) board backend for the unchanged original
// Legacy framebuffer family.
//
// drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp is board agnostic: it
// derives from the original IOMobileFramebufferLegacy and asks its provider
// for one memory resource that exactly covers the scanout window the firmware
// already lit. It never powers the panel, never programs DPU/DSI and never
// invents a mode: the panel is up because the firmware left it up.
//
// This file supplies what the driver cannot know on its own:
//   * the IOPlatformDevice that carries boot_args' framebuffer as a real
//     device memory range (native_legacy_display_late_probe),
//   * the metaclass load/postModLoad sequence for the driver's class
//     constructor, which the archive staging strips out of __mod_init_func
//     (native_legacy_display_probe),
//   * four observation hooks, and empty stubs for the instruments nabu does
//     not use.
//
// The seven IOSurface getters the driver imports (native_legacy_surface_*) are
// not defined here: scripts/build-native-legacy-display.py binds them, by
// absolute address, to the original getters in the unchanged original
// kernelcache. Defining them here would shadow that binding.

#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSData.h>
#include <kern/debug.h>
#include <kern/task.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

extern "C" kmod_info_t g_kernel_kmod_info;
// The driver's class constructor. OSDefineMetaClassAndStructors puts it in
// __DATA,__mod_init_func; llvm-objcopy removes that section and globalises the
// symbol, so exactly one call site exists and it is the one below.
extern "C" void native_legacy_display_static_init(void) __asm__("__GLOBAL__sub_I_NativeLegacyDisplay.cpp");
// The single symbol the driver exports.
extern "C" IOService *native_legacy_display_allocate(void);

// Created and init()'d by the probe, not yet attached: late_probe consumes it.
static IOService *pendingLegacyDisplay;
// Attached and started. Both are held for the life of the boot: the display
// keeps its provider, and the provider must outlive every user client.
static IOService *activeLegacyDisplay;
static IOService *activeLegacyNub;
static bool driverMetaclassesLoaded;

// The driver's only input. It re-checks the same bounds before it maps the
// resource, but rejecting a bad window here keeps a nub with a bogus extent
// from ever entering the registry.
static bool legacyVideoWindow(PE_Video *video) {
    if (PE_current_console(video)) return false;
    return video->v_baseAddr && video->v_width && video->v_height &&
           video->v_depth == 32 && !video->v_rotate &&
           video->v_width <= 16384 && video->v_height <= 16384 &&
           video->v_rowBytes >= video->v_width * 4 &&
           video->v_rowBytes <= UINT64_MAX / video->v_height;
}

// A second postModLoad would register the same metaclasses twice and OSMetaClass
// rejects that, so the sequence runs exactly once per boot.
static bool loadDriverMetaclasses(void) {
    if (driverMetaclassesLoaded) return true;
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) return false;
    native_legacy_display_static_init();
    if (OSMetaClass::postModLoad(handle)) return false;
    driverMetaclassesLoaded = true;
    return true;
}

// One unstarted display over the firmware window, or nothing.
static IOService *createDisplay(void) {
    PE_Video video = {};
    if (!legacyVideoWindow(&video)) return nullptr;
    IOService *display = native_legacy_display_allocate();
    if (!display) return nullptr;
    if (!display->init()) { display->release(); return nullptr; }
    return display;
}

extern "C" {

// ---------------------------------------------------------------------------
// Probe: create and keep.
//
// The sheng board gates this on offline-legacy-phase and releases the object
// unless the phase is 2, 3 or 4. nabu has none of those selectors and needs
// none: the object is retained for late_probe, which attaches it once the real
// IOBSD resource exists. Nothing here depends on a boot argument.
// ---------------------------------------------------------------------------
void native_legacy_display_probe(void) {
    if (pendingLegacyDisplay) return;
    if (!loadDriverMetaclasses()) return;
    pendingLegacyDisplay = createDisplay();
}

// ---------------------------------------------------------------------------
// Late probe, called from bsd_autoconf() after IOKitBSDInit().
//
// It also creates the display when the load probe never ran: the only upstream
// call site for native_legacy_display_probe() sits behind
// offline-mobile-graphics-load, and this entry has no such gate.
// ---------------------------------------------------------------------------
void native_legacy_display_late_probe(void) {
    if (!pendingLegacyDisplay) native_legacy_display_probe();
    IOService *display = pendingLegacyDisplay;
    pendingLegacyDisplay = nullptr;
    if (!display) return;

    PE_Video video = {};
    IOService *parent = IOService::getResourceService();
    if (!legacyVideoWindow(&video) || !parent) { display->release(); return; }

    // The firmware's own framebuffer, published as one device memory range of
    // exactly rowBytes*height bytes starting at v_baseAddr. That is precisely
    // what provider->mapDeviceMemoryWithIndex(0) has to return: the driver
    // refuses the mapping unless the physical address and the length match.
    //
    // The nub is attached to the resource service and to nothing else. It is
    // deliberately not a child of /arm-io: the original display code
    // enumerates that node's children, and an extra display-subsystem entry
    // there made SpringBoard find no display and crash. No compatible string,
    // no panel timing and no vendor register block is invented here.
    IOPlatformDevice *nub = new IOPlatformDevice;
    IODeviceMemory *memory = IODeviceMemory::withRange(video.v_baseAddr, video.v_rowBytes * video.v_height);
    OSArray *ranges = OSArray::withCapacity(1);
    if (!nub || !memory || !ranges || !nub->init()) {
        OSSafeReleaseNULL(ranges);
        OSSafeReleaseNULL(memory);
        OSSafeReleaseNULL(nub);
        display->release();
        return;
    }
    ranges->setObject(memory);
    nub->setDeviceMemory(ranges);        // retains the array, which retains the range
    OSSafeReleaseNULL(ranges);
    OSSafeReleaseNULL(memory);
    nub->setName("NativeGOPFramebuffer");

    bool nubAttached = nub->attach(parent);
    bool nubStarted = nubAttached && nub->start(parent);
    SInt32 score = 0;
    bool attached = nubStarted && display->probe(nub, &score) == display && display->attach(nub);
    bool started = attached && display->start(nub);
    if (started) {
        activeLegacyDisplay = display;
        activeLegacyNub = nub;
        return;
    }

    // Unwind in the reverse order of construction. The display object is
    // dropped either way: a display that did not start has no resource and no
    // client, so keeping it would only leave a half-attached IOService.
    if (attached) display->detach(nub);
    if (nubStarted) nub->stop(parent);
    if (nubAttached) nub->detach(parent);
    OSSafeReleaseNULL(nub);
    display->release();
}

// ---------------------------------------------------------------------------
// Guard for the abstract backend leaves the port does not implement. The
// original family dispatches these from paths a real platform subclass would
// have handled; reaching one means the caller expected hardware we do not
// drive, so it stops the boot with the leaf number instead of continuing.
// ---------------------------------------------------------------------------
__attribute__((noreturn, noinline)) void native_legacy_backend_guard(unsigned slot, void *object, void *caller) {
    panic("NativeLegacyDisplay: backend leaf %u is not implemented (object %p caller %p)",
          slot, object, caller);
}

// ---------------------------------------------------------------------------
// The mapping check. The driver calls this with the scanout mapping it just
// took from our nub, after it has already verified physical address and length
// against boot_args. So this only confirms the mapping is usable, and reports
// that the firmware window is now the system's display: every swap follows
// from that, and none of it goes through a boot argument.
// ---------------------------------------------------------------------------
bool native_legacy_gop_mapping_observed(IOService *object, IOMemoryMap *mapping, uint32_t width, uint32_t height) {
    if (!object || !mapping || !width || !height) return false;
    if (!mapping->getVirtualAddress()) return false;
    if (mapping->getLength() < (uint64_t)width * 4 * height) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Instruments. nabu runs the display path without the diagnostics the sheng
// board carries; these keep the driver's call sites linkable and do nothing.
// Only prepare_scanout and gpu_offer have a real answer to give.
// ---------------------------------------------------------------------------

// The driver asks before every scanout store whether a diagnostic writer still
// owns the page. nabu has no such writer, so the frame is always available.
bool native_legacy_prepare_scanout(void) { return true; }

// false selects the driver's own CPU copy of the surface into the scanout
// buffer. nabu has no GPU hand-off presenter, so every frame is copied.
bool native_legacy_gpu_offer(void *, IOMemoryDescriptor *, uint64_t, uint32_t, uint32_t) { return false; }

void native_legacy_registration_observed(IOService *) {}
void native_legacy_swap_stage(unsigned) {}
void native_legacy_client_enter(task_t, uint32_t) {}
void native_legacy_client_observed(IOReturn, IOUserClient *) {}
void native_legacy_pm_observed(unsigned, IOReturn, unsigned long) {}
void native_legacy_frame_observed(unsigned, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {}
void native_legacy_frame_colors_observed(const uint32_t *, const uint64_t *, uint64_t) {}
void native_legacy_swap_validation_observed(bool, unsigned, const void *, const void *) {}
void native_legacy_surface_rejected(uint64_t, uint64_t, uint32_t, uint32_t, uint64_t, uint64_t, uint64_t) {}
void native_legacy_transfer_observed(IOReturn, const void *, uint64_t) {}
void native_legacy_copy_timing_observed(uint64_t, uint64_t) {}
void native_legacy_retirement_observed(IOService *) {}
void native_legacy_gpu_fallback_observed(int) {}

}  // extern "C"
