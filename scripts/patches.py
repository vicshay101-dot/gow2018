"""Compile selected shadPS4/GoldHEN XML patches into out/patches.bin for the loader.

Patch addresses are PS4 virtual addresses (eboot base 0x400000); the loader's
image places eboot vaddr 0 at image offset 0. Only literal writes are supported
(bytes, bytes16/32/64, float32/64, utf8, utf16); pattern ("mask") patches are rejected.
The built-in file's lines carry signatures (Pattern/At) that find each site in any build.
"""
import argparse
import hashlib
import json
import os
import re
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

EBOOT_BASE=0x400000


def game_app_version(game):
    """APP_VER of the game folder's param.sfo ("01.02"), or None when it cannot be read."""
    return game_sfo(game).get('APP_VER')


PT_SCE_RELRO=0x61000010


def eboot_segments(elf):
    phoff,=struct.unpack_from('<Q',elf,0x20)
    phentsize,phnum=struct.unpack_from('<HH',elf,0x36)
    segments=[]
    for i in range(phnum):
        kind,_,_,vaddr,_,_,memsz,_=struct.unpack_from('<IIQQQQQQ',elf,phoff+i*phentsize)
        if kind in (1, PT_SCE_RELRO): segments.append((vaddr,vaddr+memsz))
    return segments


def eboot_image(elf):
    """The PT_LOAD and PT_SCE_RELRO file bytes of eboot.elf at their virtual addresses."""
    phoff,=struct.unpack_from('<Q',elf,0x20)
    phentsize,phnum=struct.unpack_from('<HH',elf,0x36)
    loads=[struct.unpack_from('<IIQQQQQQ',elf,phoff+i*phentsize) for i in range(phnum)]
    loads=[(vaddr,offset,filesz) for kind,_,offset,vaddr,_,filesz,_,_ in loads if kind in (1, PT_SCE_RELRO)]
    image=bytearray(max(vaddr+filesz for vaddr,_,filesz in loads))
    for vaddr,offset,filesz in loads: image[vaddr:vaddr+filesz]=elf[offset:offset+filesz]
    return bytes(image)


# Signatures find a patch site in any build whose code matches, wherever the code moved: the
# original bytes around the site, "??" where a build may differ (rip-relative displacements,
# rel32 branch targets). tools/make_signatures.py writes them from the reference build.
def parse_pattern(text):
    return [None if b=='??' else int(b,16) for b in text.split()]


def find_pattern(image, pattern, limit=2):
    """Image offsets where pattern matches, at most limit of them."""
    runs,start=[],None
    for i,b in enumerate(pattern+[None]):
        if b is not None and start is None: start=i
        if b is None and start is not None: runs.append((start,i)); start=None
    if not runs: raise ValueError('pattern without fixed bytes')
    a,b=max(runs,key=lambda r:r[1]-r[0])
    anchor=bytes(pattern[a:b])
    found,at=[],image.find(anchor)
    while at>=0 and len(found)<limit:
        s=at-a
        if s>=0 and s+len(pattern)<=len(image) and all(p is None or image[s+i]==p for i,p in enumerate(pattern)):
            found.append(s)
        at=image.find(anchor,at+1)
    return found


def locate(image, element, prefix=''):
    """实际映像必须唯一匹配特征码；无映像的参考编译才允许 Address。"""
    pattern=element.get(prefix+'Pattern')
    if not pattern:
        if image is not None:
            raise ValueError('missing signature')
        return int(element.get('Address'),0)-EBOOT_BASE
    found=find_pattern(image,parse_pattern(pattern))
    if len(found)!=1:
        raise LookupError('signature not found' if not found else 'signature found more than once')
    return found[0]+int(element.get(prefix+'At','0'),0)


def resolve_targets(image, meta, sites):
    """Image offsets of a patch's named Targets: a site (Pattern/At), or the address a rip-relative
    or rel32 operand points to (Ref: the operand's offset in its Pattern; Tail: the instruction's
    bytes after the operand)."""
    targets={}
    for target in meta.iter('Target'):
        name=target.get('Name')
        if target.get('Line'):
            index=int(target.get('Line'))
            if not 0 <= index < len(sites) or sites[index] is None:
                raise ValueError('invalid target line reference')
            targets[name]=sites[index]
            continue
        at=locate(image,target)
        if target.get('Ref'):
            if at < 0 or at + 4 > len(image):
                raise ValueError('reference operand outside the image')
            disp,=struct.unpack_from('<i',image,at)
            at=at+4+int(target.get('Tail','0'))+disp
        targets[name]=at
    return targets


def fill(value, at, targets):
    """Value bytes with each {rel:NAME:TAIL} replaced by the rel32 to target NAME, measured from the
    end of the instruction (TAIL bytes after the operand)."""
    out=bytearray()
    for part in re.split(r'(\{[^}]*\})',value.replace(' ','')):
        if not part.startswith('{'):
            out+=bytes.fromhex(part)
            continue
        _,name,tail=part[1:-1].split(':')
        out+=struct.pack('<i',targets[name]-(at+len(out)+4+int(tail)))
    return bytes(out)


def encode(line):
    kind,value=line.get('Type'),line.get('Value')
    if kind=='bytes': return bytes.fromhex(value.replace(' ',''))
    if kind in ('bytes16','bytes32','bytes64'):
        return int(value,0).to_bytes(int(kind[5:])//8,'little')
    if kind=='float32': return struct.pack('<f',float(value))
    if kind=='float64': return struct.pack('<d',float(value))
    if kind=='utf8': return value.encode()+b'\0'
    if kind=='utf16': return value.encode('utf-16-le')+b'\0\0'
    raise ValueError(f'unsupported patch type {kind!r}')


def compile_patches(xml, names, app_version, segments, image=None, skipped=None,
                    expand_video_arena=True):
    """Writes of the named patches. With the game's image, each site is found by its signature
    (any build); without it, at the reference build's Address. A Line with a Target writes at that
    Target instead: data the code points to, such as a .bss variable (no bytes of its own to find).
    A patch whose sites are not all found is left out whole, with the reason in skipped[name].
    app_version None: any version."""
    found={}
    for meta in ET.parse(xml).getroot().iter('Metadata'):
        if meta.get('Name') in names and app_version in (None,meta.get('AppVer')) and meta.get('AppElf','eboot.bin')=='eboot.bin':
            found[meta.get('Name')]=meta
    writes=[]
    for name in dict.fromkeys(names):
        try:
            if name not in found:
                raise ValueError(f'patch not found for app version {app_version}')
            lines=list(found[name].iter('Line'))
            if not lines:
                raise ValueError('empty patch')
            sites=[(None if line.get('Target') else locate(image,line)) if image is not None else int(line.get('Address'),0)-EBOOT_BASE
                   for line in lines]
            targets=(resolve_targets(image,found[name],sites) if image is not None else
                     {t.get('Name'):int(t.get('Address'),0)-EBOOT_BASE for t in found[name].iter('Target')})
            if any(not any(start <= target < end for start,end in segments)
                   for target in targets.values()):
                raise ValueError('target outside the eboot')
            sites=[targets[line.get('Target')] if line.get('Target') else site for line,site in zip(lines,sites)]
            ours=[]
            for line,offset in zip(lines,sites):
                data=fill(line.get('Value'),offset,targets) if '{' in line.get('Value','') else encode(line)
                if not 0 < len(data) <= 4096 or not any(start<=offset and offset+len(data)<=end for start,end in segments):
                    raise ValueError(f'address {offset+EBOOT_BASE:#x} or write length is outside the eboot')
                if expand_video_arena or line.get('VideoArena') != 'true':
                    ours.append((offset,data))
        except (LookupError, ValueError, TypeError, struct.error, OverflowError) as error:
            if skipped is None: raise ValueError(f'{name}: {error}')
            skipped[name]=str(error)
            continue
        # 整项验证完成后才提交，避免后续行失败时留下部分写入。
        writes.extend(ours)
    return writes


PATCHES_DIR=Path(__file__).resolve().parent.parent/'patches'
# Built-in patch file per game: its title IDs, the AppVer its metadata is tagged with and the
# title that also recognizes a release under an ID not listed. The signatures find the sites in
# any region or version whose code matches, so the eboot.bin build is not checked.
GAMES=[({'CUSA01623','CUSA01715','CUSA01720'},PATCHES_DIR/'God_of_War_III_Remastered.xml','01.02',
        'God of War III')]
# The build the built-in Address values and third-party patch files are written for.
REFERENCE_SHA256='d85c8135d330c3b601bf5dc3f1dd86bd6119fb8a9fce372bed2c9785f9a79299'
DMEM_RETAIL_MB=5056  # runtime_memory.c POOL_SIZE default


def game_sfo(game):
    """The values of the game folder's param.sfo, or {} when it cannot be read (prepare.read_sfo)."""
    if not game:
        return {}
    try:
        from prepare import read_sfo
    except ImportError:
        return {}
    return read_sfo(Path(game) / 'sce_sys/param.sfo') or {}


def game_title_id(game):
    return game_sfo(game).get('TITLE_ID')


def game_profile(title_id, title=None):
    """(title IDs, built-in XML, metadata AppVer, title) for a known game, else None."""
    title=(title or '').replace('®','').replace('™','')
    return next((g for g in GAMES if title_id in g[0] or (title and g[3] in title)),None)


def game_profile_for(game):
    """The profile of a game folder, by title ID or by the title in its param.sfo."""
    values=game_sfo(game) if game else {}
    return game_profile(values.get('TITLE_ID'),values.get('TITLE'))
_SHA_CACHE={}


def eboot_sha256(game):
    try:
        path=Path(game)/'eboot.bin'
        stat=path.stat()
        key=(str(path),stat.st_mtime_ns,stat.st_size)
        if key not in _SHA_CACHE:
            _SHA_CACHE[key]=hashlib.sha256(path.read_bytes()).hexdigest()
        return _SHA_CACHE[key]
    except (OSError, TypeError):
        return None


def game_patchable(game):
    """Whether the built-in patches are tried on this game folder (any region and version)."""
    return bool(game) and bool(game_profile_for(game))


def patch_profile_status(game):
    """Safe, non-mutating evidence for the built-in patch profile: title, version, the eboot digest
    and why the patches would be skipped. Never treats an unknown title or an unreadable eboot as
    patchable, so launch_env.py can gate on it.

    The patch sites are located by signature (Pattern/At) in any build whose code matches, so
    unlike the pre-signature design this does not compare eboot.bin against a reference hash:
    'validated' means "a known title at the version the built-in XML is tagged with, and the
    eboot is readable". compile_patches() proves each individual site when it builds patches.bin
    and reports the ones it could not find through 'skipped'.
    """
    title=game_title_id(game)
    version=game_app_version(game)
    profile=game_profile(title)
    result={'title_id':title,'app_version':version,'supported':bool(profile),
            'eboot_sha256':None,'eboot_matches':None,'patch_sites_match':None,'reason':None}
    result['eboot_sha256']=eboot_sha256(game)
    if result['eboot_sha256'] is None:
        result['reason']='missing-or-unreadable-eboot'
        return result
    if not profile:
        result['reason']='unsupported-title'
    elif version!=profile[2]:
        result['reason']='unsupported-app-version'
    else:
        result['eboot_matches']=True
        result['patch_sites_match']=True
        result['reason']='validated'
    return result


def patch_status(out):
    """The last start's patch report (patches.py writes it): {'applied': [...], 'skipped': {...}}."""
    try:
        return json.loads((Path(out)/'patch_status.json').read_text())
    except (OSError, ValueError):
        return None


def available_patches(xml, app_version):
    """Names of the patches the XML has for this app version."""
    return {m.get('Name') for m in ET.parse(xml).getroot().iter('Metadata') if m.get('AppVer')==app_version}


# God of War III notes: the texture fix and the resolution patches both resize the video arena;
# enable one, never both. A chosen resolution patch replaces the default texture fix.
EXCLUSIVE_PREFIX='Resolution Patch'
REPLACED_BY_EXCLUSIVE={'Bug Fix - Texture Corruption Fix'}
# Fixes with nothing to choose: not offered in the launcher and applied even with its saved list,
# which predates them (the startup black screen fix sets a value the game overwrites at once).
ALWAYS_ON={'Bug Fix - Startup Black Screen'}


def selected_patches(xml, app_version, extra, only=False, startup_black_screen=True):
    """The file's isEnabled patches for this version, then the names in extra (";"-separated).
    only: extra is the whole selection (the launcher's), without the file's defaults but with
    the ALWAYS_ON ones."""
    metas=[m for m in ET.parse(xml).getroot().iter('Metadata') if m.get('AppVer')==app_version]
    names=[] if only else [m.get('Name') for m in metas if m.get('isEnabled','false').lower()=='true']
    names+=[n for n in (n.strip() for n in extra.split(';')) if n and n not in names]
    names+=[m.get('Name') for m in metas if m.get('Name') in ALWAYS_ON and m.get('Name') not in names]
    exclusive=[n for n in names if n.startswith(EXCLUSIVE_PREFIX)]
    if len(exclusive)>1:
        raise ValueError(f'choose one resolution patch, not {exclusive}')
    return [n for n in names if not (exclusive and n in REPLACED_BY_EXCLUSIVE)
            and (startup_black_screen or n != 'Bug Fix - Startup Black Screen')]


def patch_requirements(xml, names, app_version, expand_video_arena=True):
    """(direct memory MiB, VBlank Hz) the selected patches' notes ask for, None when not needed.
    shadPS4 notes say "Requires setting DMEM to N MB": N is on top of the retail pool."""
    dmem=vblank=None
    for meta in ET.parse(xml).getroot().iter('Metadata'):
        if meta.get('Name') not in names or meta.get('AppVer')!=app_version: continue
        note=meta.get('Note','')
        arena_only = any(line.get('VideoArena') == 'true' for line in meta.iter('Line'))
        if expand_video_arena or not arena_only:
            if m:=re.search(r'DMEM to (\d+) MB',note): dmem=max(dmem or 0,DMEM_RETAIL_MB+int(m.group(1)))
        if m:=re.search(r'VBlank to (\d+) FPS',note): vblank=max(vblank or 0,int(m.group(1)))
    return dmem,vblank


def external_patches(directory, app_version, exclude, ids):
    """[(key, file, metadata)] of eboot patches for this version in directory/*.xml (third-party
    shadPS4/GoldHEN files in the data directory's patches/ folder).
    key is "<file name>/<patch name>" (the launcher's selection, patches.json)."""
    found=[]
    directory=Path(directory)
    for path in sorted(directory.glob('*.xml')) if directory.is_dir() else []:
        if path.resolve()==Path(exclude).resolve() or path.resolve() in {g[1].resolve() for g in GAMES}:
            continue  # built-in files (patches/ in a checkout) are not also offered as external
        try:
            root=ET.parse(path).getroot()
        except ET.ParseError as error:
            print(f'Patches: {path.name}: {error}',file=sys.stderr)
            continue
        file_ids={e.text.strip() for e in root.iter('ID') if e.text}
        if file_ids and not file_ids&ids: continue
        for meta in root.iter('Metadata'):
            if meta.get('AppVer')==app_version and meta.get('AppElf','eboot.bin')=='eboot.bin':
                found.append((f'{path.name}/{meta.get("Name")}',path,meta))
    return found


def external_selection(found, config):
    """Selected external patches: patches.json {"enabled": [...], "disabled": [...]} overrides
    each file's isEnabled."""
    settings={}
    if config and Path(config).is_file():
        settings=json.loads(Path(config).read_text())
    enabled,disabled=set(settings.get('enabled',[])),set(settings.get('disabled',[]))
    return [(key,path,meta) for key,path,meta in found
            if key in enabled or (key not in disabled and meta.get('isEnabled','false').lower()=='true')]


def compile_external(selected, segments):
    """Writes of the selected external patches; a patch with unsupported lines is skipped whole."""
    writes=[]
    for key,_,meta in selected:
        try:
            ours=[]
            for line in meta.iter('Line'):
                offset=int(line.get('Address') or '',0)-EBOOT_BASE
                data=encode(line)
                if not any(start<=offset and offset+len(data)<=end for start,end in segments):
                    raise ValueError(f'address {line.get("Address")} is outside the eboot')
                ours.append((offset,data))
        except ValueError as error:
            print(f'Patches: skipped {key}: {error}',file=sys.stderr)
            continue
        writes+=ours
        print(f'Patches: external {key} ({len(ours)} writes)')
    return writes



# Verified directly against the CUSA01715 01.02 retail SELF (SHA-256 below), by
# extracting its PT_LOAD segments and disassembling the original x86-64 code.
# These are the ACTUAL instruction operand addresses in that ELF, not the old
# shadPS4 XML's reference-build Address values. XML Pattern/At still resolves
# each site first; then the literal original operand and site address are checked.
CUSA01715_0102_ANDROID_SHA = '6e72330e9092e5164049b865e5568f2d450904001a73fe0a2f4b39194c195b3c'
CUSA01715_0102_SITES = (0x005f0c5b, 0x005f0c63, 0x005f0c6c, 0x005f0c73,
                         0x005643e3, 0x005643f0, 0x005ff889, 0x005ff88f)
CUSA01715_0102_FIELDS = ('width', 'height', 'width', 'height',
                          'height', 'width', 'width', 'height')


def android_native_resolution_writes(xml, image, segments, sha, render_res, patch_names):
    """Return exact-byte-verified CUSA01715 startup-resize overrides for this ELF.

    Applies ONLY to the supplied 01.02 executable, and never silently treats
    a selected '720p' setting as an actually applied 640x360 scene. All eight
    dimensions were checked in disassembly; the ninth XML write adjusts DMEM
    and is deliberately left untouched. No Bloodborne offsets are used.
    """
    if not render_res:
        return {}, None
    if sha != CUSA01715_0102_ANDROID_SHA:
        raise ValueError('Direct resolution requires verified CUSA01715 01.02 eboot.bin')
    if 'Resolution Patch - 720p' not in patch_names:
        raise ValueError('Direct resolution requires verified 720p patch template')
    match = re.fullmatch(r'([0-9]{2,4})x([0-9]{2,4})', render_res)
    if not match:
        raise ValueError(f'invalid direct resolution {render_res!r}')
    width, height = map(int, match.groups())
    if (width, height) not in ((1280, 720), (960, 540), (854, 480), (752, 424),
                              (640, 360), (426, 240)):
        raise ValueError(f'unsupported verified direct resolution: {render_res}')
    template = compile_patches(xml, ['Resolution Patch - 720p'], '01.02',
                               segments, image)
    if len(template) != 9:
        raise ValueError('incomplete 720p template: expected eight geometry operands and DMEM')
    overrides = {}
    for i, (offset, patched) in enumerate(template[:8]):
        address = offset + EBOOT_BASE
        expected_address = CUSA01715_0102_SITES[i]
        if address != expected_address:
            raise ValueError(f'Resolution operand {i}: got {address:#x}, expected {expected_address:#x}')
        field = CUSA01715_0102_FIELDS[i]
        original = (1920 if field == 'width' else 1080).to_bytes(len(patched), 'little')
        if image[offset:offset + len(patched)] != original:
            raise ValueError(f'Resolution operand {i}: unexpected original instruction operand at {address:#x}')
        if patched != (1280 if field == 'width' else 720).to_bytes(len(patched), 'little'):
            raise ValueError(f'Resolution operand {i}: 720p patch template mismatch at {address:#x}')
        overrides[offset] = (width if field == 'width' else height).to_bytes(len(patched), 'little')
    return overrides, {'render': f'{width}x{height}', 'output': f'{width}x{height}',
                       'verified_eboot_sha256': sha,
                       'verified_sites': [hex(site) for site in CUSA01715_0102_SITES]}


GOW2018_RES_SIGNATURES = (
    # Master PS4 Base resolution initializer (mov r13d, 1920; mov r15d, 1080).
    ('master_base',
     '83 3d ?? ?? ?? ?? 01 75 15 41 bd 00 0f 00 00 41 bf 70 08 00 00 '
     'c6 05 ?? ?? ?? ?? 01 eb 0c 41 bd 80 07 00 00 41 bf 38 04 00 00',
     ((32, 'w'), (38, 'h'))),
    # Internal render-target table allocator width loops (mov eax, 1920 before cmovg).
    ('rt_loop_1',
     '3d ff 03 00 00 b8 80 07 00 00 0f 4f 44 24 0c 31 d2 8d 44 07 ff f7 f7 41 89 c2 85 c9 7f 0b',
     ((6, 'w'),)),
    ('rt_loop_2',
     '3d ff 03 00 00 b8 80 07 00 00 0f 4f 44 24 0c 31 d2 8d 44 07 ff f7 f7 41 89 c2 85 c9 7f 0f',
     ((6, 'w'),)),
    # Mirror/refraction literal 1920x1080 entries in the render-target table.
    ('rt_literals',
     '1d 00 00 00 00 00 00 00 ?? ?? ?? ?? ?? ?? ?? ?? 80 07 00 00 38 04 00 00 05 00 00 00 00 00 00 00 '
     '1e 00 00 00 00 00 00 00 ?? ?? ?? ?? ?? ?? ?? ?? 80 07 00 00 38 04 00 00 1d 00 00 00 00 00 00 00 '
     '1f 00 00 00 00 00 00 00 ?? ?? ?? ?? ?? ?? ?? ?? 80 07 00 00 38 04 00 00 1d 00 00 00 00 00 00 00 '
     '20 00 00 00 00 00 00 00 ?? ?? ?? ?? ?? ?? ?? ?? 80 07 00 00 38 04 00 00 05 00 00 00 00 00 00 00',
     ((16, 'w'), (20, 'h'), (48, 'w'), (52, 'h'), (80, 'w'), (84, 'h'), (112, 'w'), (116, 'h'))),
    # Constant-folded internal width integers (W=1920, W/8=240, W/16=120) paired with g_is_initialized
    # (PS4 Base and PS4 Pro checkerboard both share internal width 1920, so Clang folded width while
    # keeping height dynamic; patching these keeps compute grids, culling, and viewports consistent).
    ('width_init_check',
     '00 8a 05 0e 05 9a 02 31 c9 bb 80 07 00 00 44 8b 3d 04 05 9a 02 84 c0 0f',
     ((10, 'w'),)),
    ('width_div8_cull',
     '44 00 01 89 43 20 45 31 f6 b9 f0 00 00 00 4c 89 ff 8a 05 98 6c 80 02 84',
     ((10, 'w8'),)),
    ('width_render_a',
     '62 ad 00 c5 fc 57 c0 31 c9 ba 80 07 00 00 c5 f1 ef c9 4d 89 1e c4 c1 7c',
     ((10, 'w'),)),
    ('width_div8_post',
     'fa 10 0d 1b 44 89 00 31 d2 bf f0 00 00 00 41 b9 03 00 00 00 c5 f2 5e c0',
     ((10, 'w8'),)),
    ('width_tile_w',
     '15 00 00 8a 05 19 60 6b 02 b9 80 07 00 00 84 c0 41 0f 44 ce 89 0b 8b 05',
     ((10, 'w'),)),
    ('width_tile_div8_16',
     '00 c5 fa 10 0d 8d 30 89 00 be f0 00 00 00 bf 78 00 00 00 8d 4a 01 41 0f',
     ((10, 'w8'), (15, 'w16'))),
    ('width_cs_grid8',
     'c1 75 e0 c5 fc 57 c0 31 db bf f0 00 00 00 b9 00 00 00 00 c4 c1 7c 11 47',
     ((10, 'w8'),)),
    ('width_cs_grid16',
     '02 8b 15 3d 53 6b 02 84 c0 b8 78 00 00 00 8d 72 07 0f 45 c8 c1 fe 1f c1',
     ((10, 'w16'),)),
    ('width_render_b',
     'e8 8c 95 f2 ff 49 89 46 18 ba 80 07 00 00 c5 fc 57 c0 8a 05 7d 51 6b 02',
     ((10, 'w'),)),
    ('width_render_c',
     '00 8a 05 f9 e0 6a 02 84 c0 b8 80 07 00 00 41 0f 44 c6 c5 f9 6e c0 c4 e3',
     ((10, 'w'),)),
)

# Exclusive .rodata float constants (1920.0f = W, 960.0f = W/2) referenced ONLY by internal-width
# code gated on g_is_initialized. Located via the RIP-relative disp32 at delta+12 of each 20-byte
# signature (vmovss at delta+8, instruction length 8 -> rodata_off = base + 16 + disp32).
GOW2018_RES_RODATA_FLOAT_SIGNATURES = (
    ('flt_half_2052a3', '75 06 c5 f8 57 c0 eb 08 c5 fa 10 05 b9 d0 aa 00 8b 05 37 16', 'wf2'),
    ('flt_full_231656', 'c5 e8 59 c9 c5 e8 58 c9 c5 fa 10 15 32 10 a8 00 c5 f8 59 c1', 'wf'),
    ('flt_half_2d06f6', '75 06 c5 e8 57 d2 eb 08 c5 fa 10 15 32 23 9e 00 c4 c1 72 5c', 'wf2'),
    ('flt_full_2f9f41', '75 06 c5 f8 57 c0 eb 08 c5 fa 10 05 6f 8c 9b 00 c5 fa 11 45', 'wf'),
    ('flt_full_30516a', '75 06 c5 f8 57 c0 eb 08 c5 fa 10 05 92 da 9a 00 c4 41 30 57', 'wf'),
    ('flt_full_330b11', 'fa 71 00 e9 5f e9 ff ff c5 fa 10 0d 93 22 98 00 8a 05 c9 80', 'wf'),
    ('flt_full_33f81c', 'c3 01 0f 84 74 fd ff ff c5 fa 10 15 b8 35 97 00 c5 f8 57 05', 'wf'),
    ('flt_full_35128f', '75 06 c5 f8 57 c0 eb 08 c5 fa 10 05 a9 1c 96 00 c5 fa 11 85', 'wf'),
    ('flt_full_35202c', '75 06 c5 f8 57 c0 eb 08 c5 fa 10 05 24 0f 96 00 c5 fa 11 45', 'wf'),
    ('flt_full_352de0', '75 06 c5 e0 57 db eb 08 c5 fa 10 1d 8c 01 96 00 c5 fa 59 c2', 'wf'),
    ('flt_half_36189f', '75 06 c5 e8 57 d2 eb 08 c5 fa 10 15 fd 16 95 00 c5 f2 5c 8b', 'wf2'),
    ('flt_full_41f55d', '75 06 c5 f8 57 c0 eb 08 c5 fa 10 05 1f 44 89 00 c5 fa 10 0d', 'wf'),
    ('flt_full_420173', '75 67 6b 02 84 c0 74 08 c5 fa 10 05 19 38 89 00 8b 05 67 67', 'wf'),
    ('flt_full_420903', '75 06 c5 f8 57 c0 eb 08 c5 fa 10 05 91 30 89 00 c5 fa 10 0d', 'wf'),
    ('flt_full_423c27', '75 06 c5 f8 57 c0 eb 08 c5 fa 10 05 8d fd 88 00 44 8b 25 b2', 'wf'),
    ('flt_full_52d5db', '75 06 c5 e8 57 d2 eb 08 c5 fa 10 15 91 70 78 00 c4 e3 79 04', 'wf'),
    ('flt_full_59ca63', 'c5 e0 59 d2 c5 e0 58 d2 c5 fa 10 1d d1 81 71 00 c5 f0 59 ca', 'wf'),
    ('flt_full_75575c', '75 06 c5 d8 57 e4 eb 08 c5 fa 10 25 4c fe 55 00 c5 d2 2a 35', 'wf'),
    ('flt_half_874175', 'ea 58 f8 e9 32 03 00 00 c5 fa 10 05 7b 20 44 00 c5 fa 10 25', 'wf2'),
)


def compile_gow2018_resolution_patches(image, segments, render_res):
    """Return signature-verified native resolution writes for God of War (2018) CUSA07408."""
    if not render_res:
        return []
    match = re.fullmatch(r'([0-9]{3,4})x([0-9]{3,4})', render_res.strip())
    if not match:
        raise ValueError(f'invalid CUSA07408 resolution {render_res!r}')
    width, height = map(int, match.groups())
    if (width, height) == (1920, 1080):
        return []
    if not (256 <= width <= 1920 and 144 <= height <= 1080 and width % 16 == 0 and height % 8 == 0):
        raise ValueError(f'CUSA07408 resolution must be 16x8-aligned (got {width}x{height})')
    int_expected = {'w': 1920, 'h': 1080, 'w8': 240, 'w16': 120}
    int_target = {'w': width, 'h': height, 'w8': width // 8, 'w16': width // 16}
    writes = []
    seen_offsets = set()
    for name, pattern_text, fields in GOW2018_RES_SIGNATURES:
        found = find_pattern(image, parse_pattern(pattern_text))
        if len(found) != 1:
            raise LookupError(f'CUSA07408 {name} signature match count={len(found)}')
        base = found[0]
        for delta, kind in fields:
            off = base + delta
            if not any(start <= off and off + 4 <= end for start, end in segments):
                raise ValueError(f'CUSA07408 {name}+{delta} outside PT_LOAD segments')
            expected = int_expected[kind]
            actual, = struct.unpack_from('<I', image, off)
            if actual != expected:
                raise ValueError(f'CUSA07408 {name}+{delta}: expected {expected}, got {actual}')
            if off not in seen_offsets:
                seen_offsets.add(off)
                writes.append((off, struct.pack('<I', int_target[kind])))
    flt_expected = {'wf': 1920.0, 'wf2': 960.0}
    flt_target = {'wf': float(width), 'wf2': float(width) * 0.5}
    for name, pattern_text, kind in GOW2018_RES_RODATA_FLOAT_SIGNATURES:
        found = find_pattern(image, parse_pattern(pattern_text))
        if len(found) != 1:
            raise LookupError(f'CUSA07408 {name} float signature match count={len(found)}')
        base = found[0]
        disp, = struct.unpack_from('<i', image, base + 12)
        ro_off = base + 16 + disp
        if not any(start <= ro_off and ro_off + 4 <= end for start, end in segments):
            raise ValueError(f'CUSA07408 {name} rodata {ro_off:#x} outside segments')
        actual, = struct.unpack_from('<f', image, ro_off)
        if actual != flt_expected[kind]:
            raise ValueError(
                f'CUSA07408 {name} rodata {ro_off:#x}: expected {flt_expected[kind]}, got {actual}')
        if ro_off not in seen_offsets:
            seen_offsets.add(ro_off)
            writes.append((ro_off, struct.pack('<f', flt_target[kind])))
    return writes


def compile_gow2018_motion_blur_patches(image, segments):
    """Disable God of War (2018) CUSA07408 Motion Blur by zeroing its global multiplier."""
    set_found = find_pattern(
        image, parse_pattern('c5 fa 59 05 ?? ?? ?? ?? 31 c0 c5 fa 11 05 ?? ?? ?? ?? 5d c3'))
    if len(set_found) != 1:
        raise LookupError(f'CUSA07408 SetUserMotionBlur match count={len(set_found)}')
    set_base = set_found[0]
    set_disp, = struct.unpack_from('<i', image, set_base + 14)
    data_off = set_base + 18 + set_disp
    if not any(start <= data_off and data_off + 4 <= end for start, end in segments):
        raise ValueError(f'CUSA07408 MotionBlur global {data_off:#x} outside PT_LOAD segments')
    initial, = struct.unpack_from('<f', image, data_off)
    if initial != 1.0:
        raise ValueError(f'CUSA07408 MotionBlur global {data_off:#x}: expected 1.0, got {initial}')

    load_found = find_pattern(image, parse_pattern('c5 fa 11 0d ?? ?? ?? ?? c5 da 2a c8'))
    if len(load_found) != 1:
        raise LookupError(f'CUSA07408 LoadUserMotionBlur match count={len(load_found)}')
    load_base = load_found[0]
    load_disp, = struct.unpack_from('<i', image, load_base + 4)
    if load_base + 8 + load_disp != data_off:
        raise ValueError(
            f'CUSA07408 LoadUserMotionBlur target mismatch: {load_base + 8 + load_disp:#x} != {data_off:#x}')

    nops = b'\x90' * 8
    return [
        (data_off, struct.pack('<f', 0.0)),
        (set_base + 10, nops),
        (load_base, nops),
    ]


GOW2018_SSAO_INIT_PATTERN = (
    'c5 f8 10 05 ?? ?? ?? ?? '
    'c6 05 ?? ?? ?? ?? 01 '
    'c6 05 ?? ?? ?? ?? 01 '
    'c6 05 ?? ?? ?? ?? 01 '
    'c7 05 ?? ?? ?? ?? 00 00 00 00'
)


def _find_unique_gow2018(image, pattern_text, label):
    found = find_pattern(image, parse_pattern(pattern_text))
    if len(found) != 1:
        raise LookupError(f'CUSA07408 {label} match count={len(found)}')
    return found[0]


def _check_segment_range(segments, off, size, label):
    if not any(start <= off and off + size <= end for start, end in segments):
        raise ValueError(f'CUSA07408 {label} {off:#x} outside PT_LOAD segments')


def compile_gow2018_ssao_patches(image, segments):
    """Disable God of War (2018) CUSA07408 SSAO+SSR pass using its built-in skip path."""
    base = _find_unique_gow2018(image, GOW2018_SSAO_INIT_PATTERN, 'SSAO init')
    disp, = struct.unpack_from('<i', image, base + 10)
    flag_addr = base + 15 + disp
    _check_segment_range(segments, base + 14, 1, 'SSAO init flag')
    if image[base + 14] != 1:
        raise ValueError(f'CUSA07408 SSAO init flag {base + 14:#x}: expected 1, got {image[base + 14]}')

    s_base = _find_unique_gow2018(
        image, '20 c1 88 4b 08 80 3d ?? ?? ?? ?? 00 0f 84', 'PassSSAOAndReflections::Setup')
    s_disp, = struct.unpack_from('<i', image, s_base + 7)
    if s_base + 12 + s_disp != flag_addr:
        raise ValueError(
            f'CUSA07408 SSAO Setup target mismatch: {s_base + 12 + s_disp:#x} != {flag_addr:#x}')

    e_base = _find_unique_gow2018(
        image, '4c 8b 2e 80 3d ?? ?? ?? ?? 00 0f 84', 'PassSSAOAndReflections::Execute')
    e_disp, = struct.unpack_from('<i', image, e_base + 5)
    if e_base + 10 + e_disp != flag_addr:
        raise ValueError(
            f'CUSA07408 SSAO Execute target mismatch: {e_base + 10 + e_disp:#x} != {flag_addr:#x}')

    return [(base + 14, b'\x00')]


def compile_gow2018_ssr_patches(image, segments):
    """Disable God of War (2018) CUSA07408 SSR by NOPing the sole write to its strength global."""
    init_base = _find_unique_gow2018(image, GOW2018_SSAO_INIT_PATTERN, 'SSR init')
    init_disp, = struct.unpack_from('<i', image, init_base + 31)
    ssr_addr = init_base + 39 + init_disp

    w_base = _find_unique_gow2018(
        image, 'c5 d8 58 25 ?? ?? ?? ?? 89 05 ?? ?? ?? ?? 8b 43 50 89 05 ?? ?? ?? ??', 'SSR write')
    w_disp, = struct.unpack_from('<i', image, w_base + 10)
    if w_base + 14 + w_disp != ssr_addr:
        raise ValueError(
            f'CUSA07408 SSR write target mismatch: {w_base + 14 + w_disp:#x} != {ssr_addr:#x}')
    _check_segment_range(segments, w_base + 8, 6, 'SSR write')

    return [(w_base + 8, b'\x90' * 6)]


def compile_gow2018_bloom_patches(image, segments):
    """Disable God of War (2018) CUSA07408 Bloom by NOPing the sole write to its strength global."""
    w_base = _find_unique_gow2018(
        image,
        'c5 f8 11 25 ?? ?? ?? ?? '
        'c5 7a 11 0d ?? ?? ?? ?? '
        'c7 05 ?? ?? ?? ?? 00 00 00 00 '
        'c7 05 ?? ?? ?? ?? 00 00 00 00',
        'Bloom write')
    w_disp, = struct.unpack_from('<i', image, w_base + 12)
    bloom_addr = w_base + 16 + w_disp
    _check_segment_range(segments, w_base + 8, 8, 'Bloom write')

    s_base = _find_unique_gow2018(
        image, 'c5 f1 ef c9 c5 f9 6e 05 ?? ?? ?? ?? c5 f8 2e c1 75 06 0f 8b', 'CombinedPost::Setup')
    s_disp, = struct.unpack_from('<i', image, s_base + 8)
    if s_base + 12 + s_disp != bloom_addr:
        raise ValueError(
            f'CUSA07408 Bloom Setup target mismatch: {s_base + 12 + s_disp:#x} != {bloom_addr:#x}')

    e_base = _find_unique_gow2018(
        image, 'c5 fa 10 05 ?? ?? ?? ?? c5 f1 57 c9 4c 89 7c 24 10 c5 f8 2e c1 75 06 0f 8b',
        'CombinedPost::Execute')
    e_disp, = struct.unpack_from('<i', image, e_base + 4)
    if e_base + 8 + e_disp != bloom_addr:
        raise ValueError(
            f'CUSA07408 Bloom Execute target mismatch: {e_base + 8 + e_disp:#x} != {bloom_addr:#x}')

    return [(w_base + 8, b'\x90' * 8)]


def write_patches(out, writes):
    # G3PATCH2: the patch base, so the loader can rebase pointers the patches write into
    # relocated slots.
    blob=struct.pack('<8sQQ',b'G3PATCH2',EBOOT_BASE,len(writes))
    for offset,data in writes: blob+=struct.pack('<QQ',offset,len(data))+data
    (out/'patches.bin').write_bytes(blob)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--patches-dir',type=Path,help='third-party patch XML files (shadPS4 format)')
    p.add_argument('--patches-config',type=Path,help='patches.json: enabled/disabled external patches')
    p.add_argument('--extra',default='',help='additional patch names, separated by ";"')
    p.add_argument('--out',type=Path,default=Path(__file__).resolve().parent.parent/'out')
    p.add_argument('--game-dir',type=Path,default=Path(os.environ.get('GOW3_GAME_DIR','../CUSA01623')))
    a=p.parse_args()
    title=game_title_id(a.game_dir)
    profile=game_profile_for(a.game_dir)
    if not profile:
        writes = []
        if title == 'CUSA07408':
            elf_path = a.out / 'eboot.elf'
            if elf_path.is_file():
                elf = elf_path.read_bytes()
                img = eboot_image(elf)
                segs = eboot_segments(elf)
                if os.environ.get('GOW3_GOW2018_EBOOT_RES') == '1':
                    res = os.environ.get('GOW3_PATCH_INPUT_RES', '')
                    if res:
                        try:
                            writes.extend(compile_gow2018_resolution_patches(img, segs, res))
                            print(f'Patches: CUSA07408 native render {res} ({len(writes)} writes)')
                        except (ValueError, LookupError) as error:
                            print(f'Patches: CUSA07408 resolution skipped ({error})', file=sys.stderr)
                for env_var, label, compiler in (
                    ('GOW3_GOW2018_DISABLE_MOTION_BLUR', 'motion blur', compile_gow2018_motion_blur_patches),
                    ('GOW3_GOW2018_DISABLE_SSR', 'SSR', compile_gow2018_ssr_patches),
                    ('GOW3_GOW2018_DISABLE_SSAO', 'SSAO+SSR', compile_gow2018_ssao_patches),
                    ('GOW3_GOW2018_DISABLE_BLOOM', 'bloom', compile_gow2018_bloom_patches),
                ):
                    if os.environ.get(env_var, '1') != '0':
                        try:
                            effect_writes = compiler(img, segs)
                            writes.extend(effect_writes)
                            print(f'Patches: CUSA07408 {label} disabled ({len(effect_writes)} writes)')
                        except (ValueError, LookupError) as error:
                            print(f'Patches: CUSA07408 {label} skipped ({error})', file=sys.stderr)
        write_patches(a.out, writes)
        if not writes:
            print(f'Patches: no patch profile for {title}: none applied')
        return
    ids,xml,tag,_name=profile
    sha=eboot_sha256(a.game_dir)
    print(f'Patches: {title} {game_app_version(a.game_dir)}, eboot.bin sha256 {sha}')
    names=selected_patches(xml,tag,a.extra,os.environ.get('GOW3_PATCHES_ONLY')=='1',
                          startup_black_screen=os.environ.get('GOW3_STARTUP_BLACK_SCREEN', '1') != '0')
    elf=(a.out/'eboot.elf').read_bytes()
    segments=eboot_segments(elf)
    skipped={}
    image=eboot_image(elf)
    expand_arena = os.environ.get('GOW3_EXPAND_VIDEO_ARENA', '1') != '0'
    writes=compile_patches(xml,names,None,segments,image,skipped,
                           expand_video_arena=expand_arena)
    print(f'Video arena: {"expanded (resolution preset)" if expand_arena else "original (expansion disabled)"}')
    native_input = os.environ.get('GOW3_PATCH_INPUT_RES', '')
    native_info = None
    if native_input:
        # If the 720p patch signature fails, DO NOT report that the new input
        # resolution was installed with incompatible operands.
        if 'Resolution Patch - 720p' in skipped:
            raise ValueError('cannot apply direct resolution: 720p template skipped')
        overrides, native_info = android_native_resolution_writes(
            xml,image,segments,sha,native_input,names)
        unmatched = set(overrides)
        remapped = []
        for offset,data in writes:
            if offset in overrides:
                data=overrides[offset]
                unmatched.discard(offset)
            remapped.append((offset,data))
        if unmatched:
            raise ValueError(f'direct resolution overrides missing from patch stream: {sorted(unmatched)}')
        writes=remapped
        print(f"Patches: verified CUSA01715 DIRECT render {native_info['render']} "
              f"({len(overrides)} operand writes)")
    for name,reason in skipped.items():
        print(f'Patches: {name} skipped ({reason} in this eboot.bin)')
    if a.patches_dir and sha==REFERENCE_SHA256:
        # Third-party files use the absolute addresses of the reference build, so they are only
        # applied there. After the built-in ones: an external patch of the same bytes wins.
        writes+=compile_external(external_selection(external_patches(a.patches_dir,tag,xml,ids),
                                                    a.patches_config),segments)
    write_patches(a.out,writes)
    applied=[n for n in names if n not in skipped]
    (a.out/'patch_status.json').write_text(json.dumps({'sha256':sha,'applied':applied,
        'skipped':skipped,'android_native_resolution':native_info,
        'expand_video_arena':expand_arena}))
    print(f'Patches: {title}; {len(writes)} writes from {applied or "none"}')

if __name__=='__main__':
    main()
