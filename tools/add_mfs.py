"""Copy the files from an old MFS (400K) Mac floppy into a folder on an HFS
disk image.  Takes raw images or DiskCopy 4.2 (.dc42/.image) files.

usage: python3 add_mfs.py <hfs disk.img> <mfs floppy> <folder path, e.g. Games:Seven Cities of Gold> [out.img]

The floppy's own System and Finder are skipped: the HFS disk has its own.
Writes in place unless out.img is given.  (needs: pip install machfs)
"""
import struct, sys, machfs

SKIP = {'System', 'Finder', 'Imagewriter', 'Clipboard File', 'Scrapbook File', 'Note Pad File',
        'DeskTop', 'StartupScreen'}   # (the floppy's own boot leftovers)


def floppy_bytes(path):
    d = open(path, 'rb').read()
    if len(d) > 84 and d[82:84] == b'\x01\x00':          # DiskCopy 4.2 magic
        dlen = struct.unpack_from('>I', d, 64)[0]
        return d[84:84 + dlen]
    return d


def read_mfs(img):
    mdb = img[1024:1024 + 64]
    if mdb[:2] != b'\xd2\xd7':
        raise SystemExit('not an MFS volume')
    (nfiles, dir_st, dir_len, nblocks, blk_size, _clp, al_st) = struct.unpack_from('>HHHHIIH', mdb, 12)
    vname = mdb[36 + 1:36 + 1 + mdb[36]].decode('mac_roman')

    # Allocation block map: 12 bits per block (from block 2), right after the MDB
    raw = img[1024 + 64:1024 + 64 + (nblocks * 3 + 1) // 2]

    def next_block(n):
        i = n - 2
        b = raw[(i * 3) // 2:(i * 3) // 2 + 2]
        v = b[0] << 8 | b[1]
        return v >> 4 if i % 2 == 0 else v & 0xfff

    def fork(start, length):
        out = bytearray()
        b = start
        while b > 1 and len(out) < length:
            off = al_st * 512 + (b - 2) * blk_size
            out += img[off:off + blk_size]
            b = next_block(b)
        return bytes(out[:length])

    files = []
    for blk in range(dir_st, dir_st + dir_len):
        p, end = blk * 512, blk * 512 + 512
        while p < end and img[p] & 0x80:
            (flags, _typ, ftype, creator, fflags, loc_v, loc_h, _fldr, _num, st, lg, _py,
             rst, rlg, _rpy, crdat, mddat) = struct.unpack_from('>BB4s4sHhhHIHIIHIIII', img, p)
            n = img[p + 50]
            name = img[p + 51:p + 51 + n].decode('mac_roman')
            files.append(dict(name=name, type=ftype, creator=creator, flags=fflags,
                              data=fork(st, lg), rsrc=fork(rst, rlg), crdat=crdat, mddat=mddat))
            p += 51 + n
            p += p & 1
    return vname, files


def main():
    disk_path, floppy, folder_path = sys.argv[1:4]
    out = sys.argv[4] if len(sys.argv) > 4 else disk_path

    vname, files = read_mfs(floppy_bytes(floppy))
    old = open(disk_path, 'rb').read()
    vol = machfs.Volume()
    vol.read(old)
    vol.name = old[1024 + 37:1024 + 37 + old[1024 + 36]].decode('mac_roman')   # (not kept by read)

    parent = vol
    parts = folder_path.split(':')
    for part in parts[:-1]:
        parent = parent[part]
    folder = parent[parts[-1]] = machfs.Folder()

    for f in files:
        if f['name'] in SKIP:
            continue
        item = machfs.File()
        item.type, item.creator = f['type'], f['creator']
        item.flags = f['flags'] & ~0x0100       # not "inited": let the Finder place it
        item.data, item.rsrc = f['data'], f['rsrc']
        item.crdate, item.mddate = f['crdat'], f['mddat']
        folder[f['name']] = item
        print(f"  {f['name']!r}: {f['type'].decode('mac_roman')}/{f['creator'].decode('mac_roman')}, "
              f"data {len(f['data'])}, rsrc {len(f['rsrc'])}")

    open(out, 'wb').write(vol.write(size=len(old), bootable=True))
    print(f'{out}: added {len(folder)} files from "{vname}" to {folder_path}')


main()
