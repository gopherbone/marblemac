"""Compare test_thumb output against GNU as.  usage: check_thumb.py <test binary> <scratch dir>"""
import os, subprocess, sys
exe, scratch = sys.argv[1], sys.argv[2]
lines = subprocess.run([exe], capture_output=True, text=True, check=True).stdout.strip().split('\n')
bad = 0
for line in lines:
    asm, ours = line.rsplit('\t', 1)
    src = os.path.join(scratch, 't.s'); obj = os.path.join(scratch, 't.o'); bin_ = os.path.join(scratch, 't.bin')
    with open(src, 'w') as f:
        f.write('.syntax unified\n.thumb\n.cpu cortex-m7\n' + asm + '\n')
    subprocess.run(['arm-none-eabi-as', '-o', obj, src], check=True)
    subprocess.run(['arm-none-eabi-objcopy', '-O', 'binary', '-j', '.text', obj, bin_], check=True)
    data = open(bin_, 'rb').read()
    theirs = ''.join('%04x' % (data[i] | data[i + 1] << 8) for i in range(0, len(data), 2))
    if theirs != ours:
        bad += 1
        print(f'MISMATCH {asm!r}: ours {ours} as {theirs}')
print(f'{len(lines) - bad}/{len(lines)} encodings match')
sys.exit(1 if bad else 0)
