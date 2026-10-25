"""xiso.py - read files out of an Xbox disc image (XDVDFS), pure Python.

Takes either a bare game partition ("XISO", what extract-xiso and most
dumping tools make) or a full Redump-style image, whose game partition starts
0x18300000 bytes in. Only reading: list the tree, copy files out.

usage (library):
    with XIso(path) as iso:
        for path, size in iso.files():     # 'maps/c20.map', ...
            ...
        iso.extract('maps/c20.map', 'out/c20.map')

usage (command line):
    xiso.py <image>                        lists the files
    xiso.py <image> <folder> [name ...]    copies maps/<name>.map out (every map
                                           when no name is given), e.g. a
                                           dedicated server's maps folder
    xiso.py                                self-test"""
import os, struct

SECTOR = 2048
MAGIC = b'MICROSOFT*XBOX*MEDIA'
PARTITION_OFFSETS = (0, 0x18300000, 0xFD90000, 0x89D80000, 0x2080000)


class XIso:
    def __init__(self, path):
        self.f = open(path, 'rb')
        self.base = None
        for offset in PARTITION_OFFSETS:
            self.f.seek(offset + 32 * SECTOR)
            if self.f.read(len(MAGIC)) == MAGIC:
                self.base = offset
                break
        if self.base is None:
            self.f.close()
            raise ValueError('%s: not an Xbox disc image (no XDVDFS volume found)' % path)
        self.f.seek(self.base + 32 * SECTOR + 20)
        self.root_sector, self.root_size = struct.unpack('<II', self.f.read(8))

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.f.close()

    def _read(self, sector, size):
        self.f.seek(self.base + sector * SECTOR)
        return self.f.read(size)

    def _walk(self, sector, size, prefix):
        table = self._read(sector, size)
        out = []
        stack = [0]
        seen = set()
        while stack:
            offset = stack.pop()
            if offset in seen or offset + 14 > len(table):
                continue
            seen.add(offset)
            left, right, start, length, attributes, name_length = struct.unpack_from('<HHIIBB', table, offset)
            if left == 0xFFFF and right == 0xFFFF:
                continue
            name = table[offset + 14: offset + 14 + name_length].decode('latin-1')
            path = prefix + name
            if attributes & 0x10:
                if length:
                    out += self._walk(start, length, path + '/')
            else:
                out.append((path, start, length))
            if left and left != 0xFFFF:
                stack.append(left * 4)
            if right and right != 0xFFFF:
                stack.append(right * 4)
        return out

    def files(self):
        """[(path, size)] with '/' separators"""
        return [(p, n) for p, s, n in self._walk(self.root_sector, self.root_size, '')]

    def extract(self, path, destination, each=None):
        """path copied to destination; each(chunk), when given, sees every
        chunk as it is written (the guided bake hashes and counts them)"""
        for p, start, length in self._walk(self.root_sector, self.root_size, ''):
            if p.lower() == path.lower():
                os.makedirs(os.path.dirname(os.path.abspath(destination)), exist_ok=True)
                with open(destination, 'wb') as out:
                    self.f.seek(self.base + start * SECTOR)
                    left = length
                    while left:
                        chunk = self.f.read(min(left, 4 << 20))
                        if not chunk:
                            raise IOError('%s: image ends inside %s' % (destination, path))
                        out.write(chunk)
                        if each:
                            each(chunk)
                        left -= len(chunk)
                return length
        raise FileNotFoundError(path)


def make_test_image(path, files, root_files=None):
    """a minimal XDVDFS image of {name: bytes} in one folder 'maps', and
    root_files ({name: bytes}) beside it (for testing the reader; directory
    entries in a degenerate right-leaning tree)"""
    root_files = root_files or {}
    names = sorted(files)
    data_sector = 40
    entries = []
    layout = []
    sector = data_sector
    for n in names:
        layout.append((n, sector, len(files[n])))
        sector += (len(files[n]) + SECTOR - 1) // SECTOR
    root_layout = []
    for n in sorted(root_files):
        root_layout.append((n, sector, len(root_files[n])))
        sector += (len(root_files[n]) + SECTOR - 1) // SECTOR
    def table(items):
        blob = bytearray()
        offsets = []
        for i, (n, s, l, attr) in enumerate(items):
            offsets.append(len(blob))
            e = struct.pack('<HHIIBB', 0, 0, s, l, attr, len(n)) + n.encode()
            while len(e) % 4:
                e += b'\xff'
            blob += e
        for i in range(len(items) - 1):
            struct.pack_into('<H', blob, offsets[i] + 2, offsets[i + 1] // 4)
        while len(blob) % SECTOR:
            blob += b'\xff'
        return bytes(blob)
    maps_table = table([(n, s, l, 0x20) for n, s, l in layout])
    maps_sector = 36
    root_table = table([('maps', maps_sector, len(maps_table), 0x10)] + [(n, s, l, 0x20) for n, s, l in root_layout])
    root_sector = 34
    image = bytearray(sector * SECTOR)
    vd = MAGIC + struct.pack('<II', root_sector, len(root_table))
    image[32 * SECTOR: 32 * SECTOR + len(vd)] = vd
    image[32 * SECTOR + 0x7EC: 32 * SECTOR + 0x7EC + len(MAGIC)] = MAGIC
    image[root_sector * SECTOR: root_sector * SECTOR + len(root_table)] = root_table
    image[maps_sector * SECTOR: maps_sector * SECTOR + len(maps_table)] = maps_table
    for n, s, l in layout:
        image[s * SECTOR: s * SECTOR + l] = files[n]
    for n, s, l in root_layout:
        image[s * SECTOR: s * SECTOR + l] = root_files[n]
    open(path, 'wb').write(bytes(image))


if __name__ == '__main__':
    import sys, tempfile
    if len(sys.argv) > 2:
        os.makedirs(sys.argv[2], exist_ok=True)
        with XIso(sys.argv[1]) as iso:
            maps = {p.lower(): p for p, n in iso.files() if p.lower().startswith('maps/') and p.lower().endswith('.map')}
            wanted = ['maps/%s.map' % name.lower() for name in sys.argv[3:]] or sorted(maps)
            missing = [w for w in wanted if w not in maps]
            if missing:
                sys.exit('xiso: the image has no %s' % ', '.join(missing))
            for w in wanted:
                iso.extract(maps[w], os.path.join(sys.argv[2], os.path.basename(w)))
                print('xiso: %s' % os.path.join(sys.argv[2], os.path.basename(w)))
    elif len(sys.argv) > 1:
        with XIso(sys.argv[1]) as iso:
            for p, n in sorted(iso.files()):
                print('%12d  %s' % (n, p))
    else:
        d = tempfile.mkdtemp()
        files = {'c20.map': os.urandom(5000), 'ui.map': b'hello' * 1000, 'a10.map': b''}
        make_test_image(os.path.join(d, 't.iso'), files, {'default.xbe': b'XBEH' * 300})
        with XIso(os.path.join(d, 't.iso')) as iso:
            listed = dict(iso.files())
            assert set(listed) == {'maps/' + n for n in files} | {'default.xbe'}, listed
            for n, b in files.items():
                seen = []
                iso.extract('maps/' + n, os.path.join(d, n), seen.append)
                assert open(os.path.join(d, n), 'rb').read() == b == b''.join(seen)
            iso.extract('default.xbe', os.path.join(d, 'default.xbe'))
            assert open(os.path.join(d, 'default.xbe'), 'rb').read() == b'XBEH' * 300
        print('xiso self-test passed')
