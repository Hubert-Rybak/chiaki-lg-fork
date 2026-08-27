#!/usr/bin/env python3
"""Download and verify one member of a remote LG source-release ZIP.

LG's LM21U BSP is a single large member in a multi-gigabyte ZIP.  Reading the
central directory over HTTP ranges lets CI fetch only that member while still
checking the ZIP metadata, CRC32, uncompressed size, and a pinned SHA-256.
"""

import argparse
import binascii
import hashlib
import io
import json
import os
import struct
import sys
import urllib.parse
import urllib.request
import zipfile
import zlib


RANGE_CACHE_SIZE = 2 * 1024 * 1024
COPY_CHUNK_SIZE = 1024 * 1024


class HttpRangeReader(io.RawIOBase):
    def __init__(self, url):
        self.url = url
        self.position = 0
        self.cache_start = 0
        self.cache = b""

        request = urllib.request.Request(url, headers={"Range": "bytes=0-0"})
        with urllib.request.urlopen(request, timeout=60) as response:
            content_range = response.headers.get("Content-Range", "")
            if response.status != 206 or "/" not in content_range:
                raise RuntimeError("LG source server did not honor a size probe")
            self.size = int(content_range.rsplit("/", 1)[1])

    def readable(self):
        return True

    def seekable(self):
        return True

    def tell(self):
        return self.position

    def seek(self, offset, whence=os.SEEK_SET):
        if whence == os.SEEK_SET:
            position = offset
        elif whence == os.SEEK_CUR:
            position = self.position + offset
        elif whence == os.SEEK_END:
            position = self.size + offset
        else:
            raise ValueError(f"unsupported seek mode: {whence}")
        if position < 0:
            raise ValueError("negative seek position")
        self.position = position
        return position

    def read(self, size=-1):
        if self.position >= self.size:
            return b""
        if size is None or size < 0:
            size = self.size - self.position
        if size == 0:
            return b""

        cache_end = self.cache_start + len(self.cache)
        if self.cache_start <= self.position and self.position + size <= cache_end:
            offset = self.position - self.cache_start
            data = self.cache[offset : offset + size]
            self.position += len(data)
            return data

        fetch_size = max(size, RANGE_CACHE_SIZE)
        end = min(self.size - 1, self.position + fetch_size - 1)
        request = urllib.request.Request(
            self.url, headers={"Range": f"bytes={self.position}-{end}"}
        )
        with urllib.request.urlopen(request, timeout=120) as response:
            if response.status != 206:
                raise RuntimeError("LG source server stopped honoring byte ranges")
            self.cache = response.read()
        self.cache_start = self.position
        data = self.cache[:size]
        self.position += len(data)
        return data


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for chunk in iter(lambda: source.read(COPY_CHUNK_SIZE), b""):
            digest.update(chunk)
    return digest.hexdigest()


def request_release_url(args):
    form = urllib.parse.urlencode(
        {
            "osSeq": args.os_seq,
            "modelName": args.model,
            "fileType": "Op",
            "fileIdx": args.file_index,
        }
    ).encode("ascii")
    request = urllib.request.Request(args.api, data=form)
    with urllib.request.urlopen(request, timeout=60) as response:
        result = json.load(response)
    if result.get("success") is not True or not result.get("data"):
        raise RuntimeError(f"LG source URL request failed: {result!r}")
    return result["data"]


def read_local_header(url, offset):
    request = urllib.request.Request(
        url, headers={"Range": f"bytes={offset}-{offset + 29}"}
    )
    with urllib.request.urlopen(request, timeout=60) as response:
        if response.status != 206:
            raise RuntimeError("LG source server did not return the ZIP header range")
        header = response.read()
    if len(header) != 30:
        raise RuntimeError(f"short ZIP local header: {len(header)} bytes")
    fields = struct.unpack("<IHHHHHIIIHH", header)
    if fields[0] != 0x04034B50:
        raise RuntimeError("invalid ZIP local-header signature")
    return fields


def download_member(args):
    expected_sha256 = args.sha256.lower()
    if os.path.isfile(args.output):
        size = os.path.getsize(args.output)
        if size == args.size and sha256_file(args.output) == expected_sha256:
            print(f"Reusing verified {args.output}")
            return

    url = request_release_url(args)
    remote = HttpRangeReader(url)
    with zipfile.ZipFile(remote) as archive:
        info = archive.getinfo(args.member)

    if info.file_size != args.size:
        raise RuntimeError(
            f"unexpected member size {info.file_size}, expected {args.size}"
        )
    if info.CRC != args.crc32:
        raise RuntimeError(
            f"unexpected member CRC {info.CRC:08x}, expected {args.crc32:08x}"
        )
    if info.compress_type != zipfile.ZIP_DEFLATED:
        raise RuntimeError(f"unsupported ZIP compression method {info.compress_type}")
    if info.flag_bits & 0x1:
        raise RuntimeError("encrypted ZIP members are not supported")

    fields = read_local_header(url, info.header_offset)
    flags = fields[2]
    method = fields[3]
    name_length = fields[9]
    extra_length = fields[10]
    if flags & 0x1 or method != zipfile.ZIP_DEFLATED:
        raise RuntimeError("ZIP local header disagrees with the central directory")
    data_start = info.header_offset + 30 + name_length + extra_length
    data_end = data_start + info.compress_size - 1

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    temporary = args.output + ".new"
    request = urllib.request.Request(
        url, headers={"Range": f"bytes={data_start}-{data_end}"}
    )
    decompressor = zlib.decompressobj(-15)
    digest = hashlib.sha256()
    crc32 = 0
    compressed = 0
    uncompressed = 0
    next_progress = 64 * 1024 * 1024

    try:
        with urllib.request.urlopen(request, timeout=180) as response:
            if response.status != 206:
                raise RuntimeError("LG source server did not return the member range")
            with open(temporary, "wb") as output:
                while True:
                    chunk = response.read(COPY_CHUNK_SIZE)
                    if not chunk:
                        break
                    compressed += len(chunk)
                    data = decompressor.decompress(chunk)
                    if data:
                        output.write(data)
                        digest.update(data)
                        crc32 = binascii.crc32(data, crc32)
                        uncompressed += len(data)
                    if compressed >= next_progress:
                        print(
                            f"Downloaded {compressed // (1024 * 1024)} / "
                            f"{info.compress_size // (1024 * 1024)} MiB",
                            flush=True,
                        )
                        next_progress += 64 * 1024 * 1024

                data = decompressor.flush()
                output.write(data)
                digest.update(data)
                crc32 = binascii.crc32(data, crc32)
                uncompressed += len(data)

        if compressed != info.compress_size:
            raise RuntimeError(
                f"short compressed member: {compressed} of {info.compress_size} bytes"
            )
        if uncompressed != info.file_size:
            raise RuntimeError(
                f"wrong expanded size: {uncompressed} of {info.file_size} bytes"
            )
        if crc32 & 0xFFFFFFFF != info.CRC:
            raise RuntimeError(
                f"CRC mismatch: {crc32 & 0xFFFFFFFF:08x} != {info.CRC:08x}"
            )
        actual_sha256 = digest.hexdigest()
        if actual_sha256 != expected_sha256:
            raise RuntimeError(
                f"SHA-256 mismatch: {actual_sha256} != {expected_sha256}"
            )
        os.replace(temporary, args.output)
    except BaseException:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise

    print(f"Verified {args.output} ({expected_sha256})")


def parse_crc32(value):
    try:
        return int(value, 16)
    except ValueError as error:
        raise argparse.ArgumentTypeError("CRC32 must be hexadecimal") from error


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--api", required=True)
    parser.add_argument("--os-seq", required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--file-index", required=True)
    parser.add_argument("--member", required=True)
    parser.add_argument("--size", required=True, type=int)
    parser.add_argument("--crc32", required=True, type=parse_crc32)
    parser.add_argument("--sha256", required=True)
    parser.add_argument("output")
    args = parser.parse_args()
    if len(args.sha256) != 64 or any(
        c not in "0123456789abcdefABCDEF" for c in args.sha256
    ):
        parser.error("--sha256 must contain 64 hexadecimal characters")
    download_member(args)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"download-lg-zip-member.py: {error}", file=sys.stderr)
        raise SystemExit(1)
