"""Build one bootable HFS disk from the Mac Plus pack: System 6 plus every
game, so the emulator (which has a single drive) can reach all of them.

usage: python3 make_disk.py <pack dir> <out.img> [free KB]   (needs: pip install machfs)

Free space defaults to 1MB: the image ships inside the pdx, and Playdates
don't have much room.
"""
import os, struct, sys, machfs

# Desktop pattern (8x8, 1 = black, top row first).  The stock 50% grey
# shimmers badly when the Playdate's viewport scrolls; a sparse grid of
# little crosses gives you landmarks and stays put.
DESKTOP_PATTERN = bytes([0x00, 0x00, 0x10, 0x38, 0x10, 0x00, 0x00, 0x00])

def set_resource(rsrc, rtype, rid, data):
    """Overwrite an existing resource's bytes in place (same length)."""
    rsrc = bytearray(rsrc)
    data_off, map_off = struct.unpack_from('>II', rsrc, 0)
    types_off = map_off + struct.unpack_from('>H', rsrc, map_off + 24)[0]
    ntypes = struct.unpack_from('>H', rsrc, types_off)[0] + 1
    for t in range(ntypes):
        tname, n, refs = struct.unpack_from('>4sHH', rsrc, types_off + 2 + t * 8)
        if tname != rtype:
            continue
        for r in range(n + 1):
            ref = types_off + refs + r * 12
            res_id = struct.unpack_from('>h', rsrc, ref)[0]
            off = struct.unpack_from('>I', rsrc, ref + 4)[0] & 0xFFFFFF
            if res_id == rid:
                at = data_off + off
                if struct.unpack_from('>I', rsrc, at)[0] != len(data):
                    raise ValueError('size mismatch')
                rsrc[at + 4:at + 4 + len(data)] = data
                return bytes(rsrc)
    raise KeyError(f'{rtype} {rid} not found')

pack, out = sys.argv[1], sys.argv[2]

def load(path):
    v = machfs.Volume()
    v.read(open(path, 'rb').read())
    return v

def size_of(item):
    if isinstance(item, (machfs.Folder, machfs.Volume)):
        return sum(size_of(i) for i in item.values())
    return len(item.data) + len(item.rsrc)

disk = machfs.Volume()
disk.name = 'Marble HD'

system = load(os.path.join(pack, 'System disks', 'System disk v6.dsk'))
disk['System Folder'] = system['System Folder']
sysfile = disk['System Folder']['System']
sysfile.rsrc = set_resource(sysfile.rsrc, b'PAT ', 16, DESKTOP_PATTERN)
for keep in ('TeachText', 'Read Me', 'Disk First Aid'):
    disk[keep] = system[keep]

games = machfs.Folder()
disk['Games'] = games

ark = load(os.path.join(pack, 'Games', 'Boot disks', 'Arkanoid.dsk'))
games['Arkanoid'] = folder = machfs.Folder()
for name, item in ark.items():
    if name != 'System Folder':
        folder[name] = item

comp = os.path.join(pack, 'Games', 'Compilations (no boot)')
for fn in sorted(os.listdir(comp)):
    if not fn.upper().endswith('.DSK'):
        continue
    for name, item in load(os.path.join(comp, fn)).items():
        if name in games:
            print('skipping duplicate', name, 'from', fn)
            continue
        games[name] = item

used = size_of(disk)
free = int(sys.argv[3]) * 1024 if len(sys.argv) > 3 else 1024 * 1024
size = (used * 21 // 20 + free + 0xFFFF) & ~0xFFFF     # ~5% for HFS overhead
open(out, 'wb').write(disk.write(size=size, bootable=True))
print(f'{out}: {size // 1024}K, {used // 1024}K used, {len(games)} items in Games')
