#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Fetch members of an Apple software-update (OTA) ZIP without downloading the whole archive.

Apple's CDN serves OTA ZIPs that support HTTP range requests (with a MobileAsset user agent). This reads the ZIP's
central directory remotely, then streams single members, inflating and checking size and CRC32 against the directory.

  ota.py list    SOURCE [--grep TEXT]
  ota.py get     SOURCE MEMBER... --out DIR
  ota.py payload SOURCE --out DIR [--jobs N]     all AssetData/payloadv2/payload.NNN chunks (resumable)

SOURCE is a name from sources.json next to this script (e.g. 25A353-full, 25A353-intel) or a URL.
"""
import argparse
import concurrent.futures
import io
import json
import struct
import sys
import urllib.request
import zipfile
import zlib
from pathlib import Path

UA = 'MobileAsset/1.0'
HERE = Path(__file__).resolve().parent


def source_url(name):
    if name.startswith('https://'):
        return name
    sources = json.loads((HERE / 'sources.json').read_text())
    if name not in sources:
        raise SystemExit(f'unknown source {name}; known: {", ".join(sources)}')
    return sources[name]['url']


def http_range(url, start, end, timeout=120):
    req = urllib.request.Request(url, headers={'User-Agent': UA, 'Range': f'bytes={start}-{end}'})
    return urllib.request.urlopen(req, timeout=timeout)


class RemoteFile(io.RawIOBase):
    def __init__(self, url):
        self.url, self.pos = url, 0
        req = urllib.request.Request(url, method='HEAD', headers={'User-Agent': UA})
        with urllib.request.urlopen(req, timeout=60) as r:
            self.size = int(r.headers['Content-Length'])

    def seekable(self):
        return True

    def readable(self):
        return True

    def tell(self):
        return self.pos

    def seek(self, off, whence=0):
        self.pos = off if whence == 0 else self.pos + off if whence == 1 else self.size + off
        return self.pos

    def read(self, n=-1):
        n = self.size - self.pos if n < 0 else min(n, self.size - self.pos)
        if n <= 0:
            return b''
        for attempt in range(5):
            try:
                with http_range(self.url, self.pos, self.pos + n - 1) as r:
                    data = r.read()
                break
            except OSError:
                if attempt == 4:
                    raise
        if len(data) != n:
            raise OSError(f'short range read at {self.pos}')
        self.pos += n
        return data


def open_zip(url):
    remote = RemoteFile(url)
    return remote, zipfile.ZipFile(remote)


def data_offset(remote, info):
    remote.seek(info.header_offset)
    hdr = remote.read(30)
    name_len, extra_len = struct.unpack_from('<HH', hdr, 26)
    return info.header_offset + 30 + name_len + extra_len


def fetch(url, remote, info, dest):
    dest = Path(dest)
    if dest.exists() and dest.stat().st_size == info.file_size and crc_of(dest) == info.CRC:
        return f'SKIP {dest.name}'
    start = data_offset(remote, info)
    part = dest.with_name(dest.name + '.part')
    dest.parent.mkdir(parents=True, exist_ok=True)
    for attempt in range(5):
        try:
            dec = zlib.decompressobj(-15) if info.compress_type == zipfile.ZIP_DEFLATED else None
            crc = total = got = 0
            with http_range(url, start, start + info.compress_size - 1, 900) as r, open(part, 'wb') as out:
                while block := r.read(1 << 20):
                    got += len(block)
                    data = dec.decompress(block) if dec else block
                    out.write(data)
                    crc, total = zlib.crc32(data, crc), total + len(data)
                if dec:
                    tail = dec.flush()
                    out.write(tail)
                    crc, total = zlib.crc32(tail, crc), total + len(tail)
            break
        except OSError as e:
            print(f'retry {dest.name} ({attempt + 1}): {e}', flush=True)
    else:
        raise SystemExit(f'{dest.name}: download failed')
    if got != info.compress_size or total != info.file_size or (crc & 0xffffffff) != info.CRC:
        raise SystemExit(f'{dest.name}: size/CRC mismatch')
    part.replace(dest)
    return f'OK {dest.name} {total}'


def crc_of(path):
    crc = 0
    with open(path, 'rb') as f:
        while block := f.read(1 << 22):
            crc = zlib.crc32(block, crc)
    return crc & 0xffffffff


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cmd', choices=['list', 'get', 'payload'])
    ap.add_argument('source')
    ap.add_argument('members', nargs='*')
    ap.add_argument('--out', default='.')
    ap.add_argument('--grep')
    ap.add_argument('--jobs', type=int, default=4)
    a = ap.parse_args()
    url = source_url(a.source)
    remote, z = open_zip(url)
    if a.cmd == 'list':
        for i in z.infolist():
            if not a.grep or a.grep in i.filename:
                print(f'{i.file_size:>14} {i.filename}')
        return
    if a.cmd == 'get':
        names = {i.filename: i for i in z.infolist()}
        for m in a.members:
            if m not in names:
                raise SystemExit(f'no member {m}')
            print(fetch(url, remote, names[m], Path(a.out) / Path(m).name), flush=True)
        return
    chunks = [i for i in z.infolist()
              if i.filename.startswith('AssetData/payloadv2/payload.') and i.filename.rsplit('.', 1)[-1].isdigit()]
    chunks.sort(key=lambda i: int(i.filename.rsplit('.', 1)[-1]))
    print(f'{len(chunks)} chunks, {sum(i.file_size for i in chunks)} bytes', flush=True)
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        # each worker needs its own remote handle for header reads
        futs = [ex.submit(lambda i: fetch(url, RemoteFile(url), i, Path(a.out) / i.filename.rsplit('/', 1)[-1]), i)
                for i in chunks]
        for f in concurrent.futures.as_completed(futs):
            print(f.result(), flush=True)
    print('all payload chunks verified')


if __name__ == '__main__':
    sys.exit(main())
