#!/usr/bin/env python3
"""Turn raw Symbian OS 9 firmware images (one core ROM, any number of ROFS/ROFx
images) into the ROM + RPKG pair the device installer takes.

    semc_fw_to_rpkg.py --rom CORE.bin --rofs ROFS1 [--rofs ROFS2 ...] --out-dir OUT

ROFS images are layered in the order given: a later image replaces files of an
earlier one, as the composite file system does on the phone.
"""
import argparse
import os
import re
import struct
import sys

ROFS_MODERN_VERSION = 0x200
ROFS_FILE_HIDDEN = 0xFFFFFFFF


def u16(b, o):
    return struct.unpack_from('<H', b, o)[0]


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def rom_files(rom):
    """Yield (path, attrib, data) for every file in an EKA2 core ROM."""
    base = u32(rom, 0x8C)
    root_list = u32(rom, 0x94) - base
    seen = set()

    def walk(dir_addr, prefix):
        off = dir_addr - base
        if off in seen or off < 0 or off + 4 > len(rom):
            return
        seen.add(off)
        size = u32(rom, off)
        pos, end = off + 4, off + 4 + size
        while pos < end:
            esize, addr, att, nlen = struct.unpack_from('<IIBB', rom, pos)
            name = rom[pos + 10:pos + 10 + nlen * 2].decode('utf-16-le')
            pos = (pos + 10 + nlen * 2 + 3) & ~3
            if att & 0x10:
                yield from walk(addr, prefix + name + '\\')
            else:
                foff = addr - base
                yield prefix + name, att, rom[foff:foff + esize]

    for i in range(u32(rom, root_list)):
        yield from walk(u32(rom, root_list + 8 + i * 8), '')


def rofs_files(img, base=b''):
    """Yield (path, attrib, data) for every file in a ROFS/ROFx image.

    A ROFx extension shares one address space with the ROFS it extends, and
    its directory can name files whose data still lives in that base image.
    """
    if img[:4] not in (b'ROFS', b'ROFx'):
        raise ValueError('not a ROFS image')
    header_size = img[4]
    version = u16(img, 6)
    dir_tree = u32(img, 8)
    # ROFx addresses continue where the image it extends ended.
    adjust = dir_tree - header_size if img[:4] == b'ROFx' else 0

    def entry(pos, end):
        start = pos
        size = u16(img, pos)
        pos += 2
        if version >= ROFS_MODERN_VERSION:
            pos += 16
        name_off, att, fsize, faddr, att_extra, nlen = struct.unpack_from('<BBIIBB', img, pos)
        if size < pos + 12 - start or start + size > len(img) or start + name_off + nlen * 2 > end or name_off + nlen * 2 > size:
            raise ValueError(f'invalid ROFS directory entry at {start:#x}: {size=} {end=:#x} {name_off=} {nlen=}')
        name = img[start + name_off:start + name_off + nlen * 2].decode('utf-16-le')
        return size, name, att, fsize, faddr

    def walk(addr, prefix):
        off = addr - adjust
        size, _, first, fblock, fblock_size = struct.unpack_from('<HBBII', img, off)
        if off < 0 or size < 12 or off + size > len(img):
            raise ValueError('invalid ROFS directory block')
        if fblock:
            pos = fblock - adjust
            end = pos + fblock_size
            if pos < 0 or end > len(img):
                raise ValueError('invalid ROFS file block')
            while pos < end:
                esize, name, att, fsize, faddr = entry(pos, end)
                pos += esize
                if faddr == ROFS_FILE_HIDDEN:
                    yield prefix + name, att, None
                    continue
                if faddr >= adjust:
                    data = img[faddr - adjust:faddr - adjust + fsize]
                else:
                    data = base[faddr:faddr + fsize]
                if len(data) != fsize:
                    raise ValueError('%s%s: data outside the supplied images' % (prefix, name))
                yield prefix + name, att, data
        pos = off + (first or 12)
        while pos < off + size:
            esize, name, att, fsize, faddr = entry(pos, off + size)
            yield from walk(faddr, prefix + name + '\\')
            pos += esize

    yield from walk(dir_tree, '')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--rom', required=True)
    ap.add_argument('--rofs', action='append', default=[])
    ap.add_argument('--out-dir', required=True)
    ap.add_argument('--machine-uid', type=lambda v: int(v, 0),
                    help='HALData::EMachineUid; read from ProductInfo.ini when omitted')
    ap.add_argument('--list', action='store_true', help='print the file list instead of writing')
    args = ap.parse_args()

    rom = open(args.rom, 'rb').read()
    files = {}
    order = []

    def add(layer, items):
        count = 0
        for path, att, data in items:
            key = path.lower()
            if data is None:
                if key in files:
                    del files[key]
                    order.remove(key)
            else:
                if key not in files:
                    order.append(key)
                files[key] = (path, att, data)
            count += 1
        print('%-8s %6d files' % (layer, count), file=sys.stderr)

    add('rom', rom_files(rom))
    base = b''
    for path in args.rofs:
        img = open(path, 'rb').read()
        add(os.path.basename(path)[:8], rofs_files(img, base))
        if img[:4] == b'ROFS':
            base = img

    if args.list:
        for key in order:
            path, att, data = files[key]
            print('%9d %02x z:\\%s' % (len(data), att, path))
        return

    machine_uid = args.machine_uid
    if machine_uid is None:
        # Sony Ericsson UIQ 3 keeps the HAL attributes as "<attribute>=<value>" lines; 5 is EMachineUid.
        info = files.get('resource\\apps\\productinfo.ini')
        text = ''
        if info:
            text = info[2].decode('utf-16' if info[2][:2] == b'\xff\xfe' else 'latin-1')
        match = re.search(r'(?:^|[\r\n])5=(\d+)', text)
        machine_uid = int(match.group(1)) if match else 0
    print('machine uid 0x%08X' % machine_uid, file=sys.stderr)

    os.makedirs(args.out_dir, exist_ok=True)
    with open(os.path.join(args.out_dir, 'SYM.ROM'), 'wb') as f:
        f.write(rom)

    # TRomHeader::iVersion sits after the three 32-bit fields that follow iKernelLimit.
    major, minor, build = struct.unpack_from('<BBH', rom, 0xA8)
    with open(os.path.join(args.out_dir, 'SYM.RPKG'), 'wb') as f:
        f.write(struct.pack('<4I', ord('R'), ord('P'), ord('K'), ord('2')))
        f.write(struct.pack('<BBHIII', major, minor, build, len(order), 32, machine_uid))
        for key in order:
            path, att, data = files[key]
            full = 'Z:\\' + path
            f.write(struct.pack('<QQQ', att, 0, len(full)))
            f.write(full.encode('utf-16-le'))
            f.write(struct.pack('<Q', len(data)))
            f.write(data)
    print('wrote %d files' % len(order), file=sys.stderr)


if __name__ == '__main__':
    main()
