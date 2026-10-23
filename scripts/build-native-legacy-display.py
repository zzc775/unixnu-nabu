#!/usr/bin/env python3
"""Bind the nabu board backend to the unchanged original Legacy framebuffer lifecycle.

Produces build/native-legacy-display/libNativeLegacyDisplay.late.a, which
scripts/build-xnu-4k-baseline.sh force-loads when XNU_NATIVE_LEGACY_DISPLAY=1.
The link refuses the archive unless build-report.json still describes it: the
archive digest must match, and so must every source listed there.
"""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/native-legacy-display'
OUT.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(ROOT / 'kits/NativePrelink'))
from native_prelink import macho, backed_offset, exports

KIT = ROOT / 'kits/nabu'          # board.toml kit = "nabu": the tree links package/ here
ARCHIVE = OUT / 'libNativeLegacyDisplay.late.a'
PROBE = KIT / 'LegacyDisplayProbe.cpp'
ABI = OUT / 'OriginalLegacyFramebufferABI.h'


def run(cmd, **kw):
    return subprocess.check_output([str(x) for x in cmd], timeout=90, **kw)


def require(value, why):
    if not value:
        raise SystemExit(why)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


# 1. The audit resolves the original class against the original kernelcache: its
#    vtable, its abstract leaves and the address of every slot. The header below
#    is generated from that, so the owned subclass fills exactly the slots the
#    original leaves abstract and inherits the rest from the real metadata.
print(run(['python3', ROOT / 'scripts/audit-mobile-graphics-legacy.py'], text=True), end='')
audit = json.loads((ROOT / 'build/display-analysis/mobile-graphics-legacy-abi.json').read_text())
pure = audit['abstract_slots']
methods = audit['methods']

header = '''// SPDX-License-Identifier: BSD-2-Clause-Patent
// Opaque ABI storage for the exact original class, not source-level access to its private fields. The
// actual original metadata supplies its full Framebuffer/Service/IOService ancestry. Only qualified base
// prototypes may be called from source; opaque entries are original dispatch targets only.
#pragma once
#include <IOKit/IOService.h>
#include <libkern/libkern.h>
struct OriginalLegacySwapRequest;
class IOMobileFramebufferLegacy : public IOService {
 OSDeclareAbstractStructors(IOMobileFramebufferLegacy)
 unsigned char originalPrivate[0x18a0-sizeof(IOService)];
public:
 void installBootDimensions(uint32_t width,uint32_t height) {
  *reinterpret_cast<uint32_t *>(reinterpret_cast<unsigned char *>(this)+5808)=width;
  *reinterpret_cast<uint32_t *>(reinterpret_cast<unsigned char *>(this)+5812)=height;
 }
 const OriginalLegacySwapRequest *pendingSwapRequest() const {
  const OriginalLegacySwapRequest *request=nullptr;
  memcpy(&request,reinterpret_cast<const unsigned char *>(this)+5480,sizeof(request));return request;
 }
 // The queue is a tail queue (head +5480, tail +5488, each request's next +2960, previous-link +2968).
 // Take the head and move the next request, if a client already queued one behind it, up; the original
 // completion body then releases the current entry.
 bool activateOnlyPendingSwap(const OriginalLegacySwapRequest *request) {
  auto self=reinterpret_cast<unsigned char *>(this);
  auto bytes=reinterpret_cast<const unsigned char *>(request);
  const void *current=nullptr,*head=nullptr,*tail=nullptr,*next=nullptr,*previous=nullptr;
  memcpy(&current,self+5472,8);memcpy(&head,self+5480,8);memcpy(&tail,self+5488,8);
  if(!request || current || head!=request)return false;
  memcpy(&next,bytes+2960,8);memcpy(&previous,bytes+2968,8);
  if(previous!=self+5480)return false;
  if(next) {
   const void *link=self+5480;
   memcpy(const_cast<unsigned char *>(reinterpret_cast<const unsigned char *>(next))+2968,&link,8);
  } else if(tail!=bytes+2960)return false;
  memcpy(self+5472,&request,8);
  if(next) {memcpy(self+5480,&next,8);}
  else {const void *empty=nullptr,*emptyTail=self+5480;memcpy(self+5480,&empty,8);memcpy(self+5488,&emptyTail,8);}
  return true;
 }
 void free() override;
 bool init(OSDictionary *properties=nullptr) override;
 IOReturn setProperties(OSObject *) override;
 void systemWillShutdown(IOOptionBits) override;
 IOService *probe(IOService *,SInt32 *) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 IOWorkLoop *getWorkLoop() const override;
 IOReturn newUserClient(task_t,void *,UInt32,IOUserClient **) override;
 IOReturn setPowerState(unsigned long,IOService *) override;
'''
# Slots whose prototype the driver calls or overrides from source, then the remaining
# abstract leaves, then opaque guards for the rest.
named = {184: 'bool isMainDisplay();', 188: 'IOReturn getDotPitch(uint32_t *);',
         213: 'IOReturn supportedFrameInfo(uint32_t,uint32_t *,char *,uint32_t *)=0;',
         249: 'void getDisplayDimensions(uint32_t *,uint32_t *);', 288: 'bool startHardware();',
         290: 'void kickSwapQueue();', 295: 'void completeCurrentSwap();',
         296: 'bool validateSwap(const OriginalLegacySwapRequest *);',
         337: 'IOReturn collectCompletionReports(uint32_t,void *,uint32_t *,uint32_t)=0;'}
slots = []
for i in range(168, 358):
    if i in named: slots.append(' virtual ' + named[i])
    elif i in pure: slots.append(' virtual __attribute__((noreturn)) void opaque%d()=0;' % i)
    else: slots.append(' virtual __attribute__((deprecated("Unclassified ABI: never call from source"))) void opaque%d();' % i)
slots.append('};')
slots.append('static_assert(sizeof(IOMobileFramebufferLegacy)==0x18a0,"Actual original instance extent");')
ABI.write_text(header + '\n'.join(slots) + '\n')

# 2. The driver, compiled with the flags of the kernel's own IOPlatformExpert.
print(run(['python3', ROOT / 'scripts/build-arm-driver.py', 'NativeLegacyDisplay'], text=True), end='')
obj = OUT / 'NativeLegacyDisplay.o'
raw = obj.read_bytes()
image = macho(raw)

# 3. Read the owned vtable out of the object and resolve every slot through its relocations. A slot
#    that resolves to a NativeLegacyDisplay method is ours; one that resolves to an
#    IOMobileFramebufferLegacy symbol is the original's and must be bound by absolute address;
#    anything else must be an inherited native entry at the address the audit observed.
st = next(p for c, p, _ in image['commands'] if c == 2)
sy, count, strings, _ = struct.unpack_from('<4I', raw, st + 8)
symbols = []
for i in range(count):
    ix, kind, section, _, va = struct.unpack_from('<IBBHQ', raw, sy + i * 16)
    symbols.append((raw[strings + ix:raw.index(b'\0', strings + ix)].decode(), va, kind))
vtable = next(va for name, va, _ in symbols if name == '__ZTV19NativeLegacyDisplay')
relocations = {}
for segment in image['segments']:
    for sec in segment['sections']:
        if sec['va'] <= vtable < sec['va'] + sec['size']:
            ro, rc = struct.unpack_from('<2I', raw, sec['command_offset'] + 56)
            for i in range(rc):
                offset, bits = struct.unpack_from('<2I', raw, ro + i * 8)
                if bits >> 27 & 1 and bits >> 28 == 0 and bits >> 25 & 3 == 3:
                    relocations[sec['va'] + offset] = symbols[bits & 0xffffff][0]
compiled = [relocations.get(vtable + 16 + i * 8) for i in range(358)]
require(all(compiled) and vtable + 16 + 358 * 8 not in relocations, 'Exact owned 358-entry vtable')

# 4. The original kernelcache as the prelink step observed it. Every address below belongs to the
#    unchanged original code at slide -0x1000000, so it does not move with our kernel.
candidate = ROOT / 'build/native-prelink/native-owner-mobile-graphics-candidate.macho'
if not candidate.exists():          # pruned: any later candidate carries the identical original family
    later = sorted((ROOT / 'build/native-prelink').glob('native-owner-ufs-cpu-desktop-*-candidate.macho'),
                   key=lambda f: f.stat().st_mtime)
    require(later, 'No reference candidate available')
    candidate = later[-1]
old = candidate.read_bytes()
old_image = macho(old)
names = {n: v for v, group in exports(old, old_image).items() for n in group}


def read(va, n):
    at = backed_offset(old_image['segments'], va, n, len(old))
    return old[at:at + n]


bindings = {'__ZN25IOMobileFramebufferLegacyC2EPK11OSMetaClass': 0xfffffff0057865cc,
            '__ZN25IOMobileFramebufferLegacyD2Ev': 0xfffffff005786618,
            '__ZN25IOMobileFramebufferLegacy10gMetaClassE': 0xfffffff006b35940,
            '__ZTV25IOMobileFramebufferLegacy': int(audit['legacy_vptr'], 16) - 16}
owned = []
native = []
for i, name in enumerate(compiled):
    expected = int(methods[i]['address'], 16)
    if name.startswith(('__ZN19NativeLegacyDisplay', '__ZNK19NativeLegacyDisplay')):
        owned.append(i)
        require(i in (0, 1, 7, 14, 84, 86, 87, 141, 155, 184, 188, 288, 290, 296) or i in pure,
                'Only qualified own methods and guarded leaves override')
    elif '25IOMobileFramebufferLegacy' in name:
        require(i not in pure, 'No unresolved original abstract backend target')
        bindings[name] = expected
    else:
        require(i < 168 and names.get(name) == expected, 'Actual inherited native base symbol %d' % i)
        native.append(i)
require(owned == sorted([0, 1, 7, 14, 84, 86, 87, 141, 155, 184, 188, 288, 290, 296] + pure),
        'Exact owned method set')
# The board file must not define the surface getters: those are original IOSurface code, reached
# through the real swap request, not through our object layout.
undefined = {n for n, _, k in symbols if k == 1 and 'IOMobileFramebufferLegacy' in n}
require(undefined == set(bindings), 'All and only actual original-class imports qualified')
for name, address, field, wide in [
        ('descriptor', 0xfffffff004b9ead0, 48, True), ('width', 0xfffffff004b9ebbc, 88, True),
        ('height', 0xfffffff004b9ebc4, 96, True), ('stride', 0xfffffff004b9ebd4, 104, True),
        ('allocation', 0xfffffff004b9ebfc, 144, True), ('format', 0xfffffff004b9ebac, 128, False),
        ('planes', 0xfffffff004b9ebf4, 152, False)]:
    expected = (0xf9400000 if wide else 0xb9400000) | ((field // (8 if wide else 4)) << 10)
    require(read(address, 8) == struct.pack('<2I', expected, 0xd65f03c0), 'Exact original surface getter ' + name)
    bindings['_native_legacy_surface_' + name] = address

# 5. Absolute definitions for those imports, and the driver object staged so its instructions stay
#    byte-identical: only the initialiser section moves.
asm = OUT / 'original-bindings.S'
asm.write_text(''.join('.globl %s\n.set %s, %#x\n' % (name, name, va) for name, va in sorted(bindings.items())))
sdk = run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk, '-c', asm, '-o', OUT / 'original-bindings.o'])
llvm = Path(run(['brew', '--prefix', 'llvm'], text=True).strip()) / 'bin'
late = OUT / 'NativeLegacyDisplay.late.o'
run([llvm / 'llvm-objcopy', '--remove-section=__DATA,__mod_init_func',
     '--globalize-symbol=__GLOBAL__sub_I_NativeLegacyDisplay.cpp', obj, late])


def code(data, info):
    sec = next(s for g in info['segments'] for s in g['sections'] if s['name'] == '__text')
    return data[sec['fileoff']:sec['fileoff'] + sec['size']]


require(code(raw, image) == code(late.read_bytes(), macho(late.read_bytes())),
        'Only owned initialiser section registration changes')

# 6. The board file, compiled with the recipe the kernel used for IOPlatformExpert.
recipe = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json')
                    .read_text().strip().rstrip(','))
cmd = []
args = recipe['arguments']
i = 0
while i < len(args):
    value = args[i]
    if value == recipe['file']: cmd.append(str(PROBE))
    elif value == '-o': cmd += ['-o', str(OUT / 'LegacyDisplayProbe.o')]; i += 1
    elif value == '-Xclang' and args[i + 1].startswith('-W'): i += 1
    elif not value.startswith('-W'): cmd.append(value)
    i += 1
cmd += ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
        '-Wno-unnecessary-virtual-specifier', '-Wno-c99-designator', '-Wno-reorder-init-list',
        '-Wno-deprecated-enum-enum-conversion', '-I', str(ROOT / '.cache/xnu-work/osfmk')]
run(cmd, cwd=recipe['directory'])

# 7. The archive, then the report the link gate reads.
ARCHIVE.unlink(missing_ok=True)
run(['xcrun', 'ar', 'rcs', ARCHIVE, late, OUT / 'original-bindings.o', OUT / 'LegacyDisplayProbe.o'])
sources = [ROOT / 'drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp',
           ROOT / 'drivers/NativeMobileDisplay/NativeLegacyRequest.h', PROBE, ABI]
(OUT / 'build-report.json').write_text(json.dumps(dict(
    source_sha256={str(p.relative_to(ROOT)): sha(p) for p in sources},
    generated_abi_sha256=sha(ABI),
    original_candidate_sha256=sha(candidate),
    original_audit_sha256=sha(ROOT / 'build/display-analysis/mobile-graphics-legacy-abi.json'),
    compiled_object_sha256=sha(obj), late_object_sha256=sha(late), archive_sha256=sha(ARCHIVE),
    bindings={n: hex(v) for n, v in bindings.items()}, virtual_entries=358,
    owned_entries=owned, inherited_native_entries=native,
    guarded_backend_leaves=[s for s in pure if s not in (213, 337)],
    original_instructions_modified=False, driver_initialized=False,
    display_presented=False, desktop=False), indent=2) + '\n')
print('Built the nabu board backend over the genuine original Legacy inheritance/dispatch')
