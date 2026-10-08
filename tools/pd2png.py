import sys, zlib, struct
w, h = 512, 342
raw = open(sys.argv[1], 'rb').read()   # 1 = white
rows = b''.join(b'\0' + raw[y*64:(y+1)*64] for y in range(h))
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
open(sys.argv[2], 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 1, 0, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
