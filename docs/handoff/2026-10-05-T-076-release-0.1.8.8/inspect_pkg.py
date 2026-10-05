#!/usr/bin/env python3
"""在没有 xar / bsdtar 的机器上拆 macOS 扁平安装包（.pkg = xar），核对：包结构、各组件版本与安装位置、
主二进制的 Mach-O 架构、Info.plist 版本。只读，解到临时目录，用完删除。

用法：inspect_pkg.py <包> [--names]      （--names 只列条目名，供两个版本对照结构）
"""
import gzip
import os
import plistlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zlib

CPU = {0x0100000C: "arm64", 0x01000007: "x86_64"}


def read_xar(path):
    blob = open(path, "rb").read()
    magic, hsize, ver, toc_c, toc_u, alg = struct.unpack(">4sHHQQI", blob[:28])
    assert magic == b"xar!", "不是 xar"
    toc = zlib.decompress(blob[hsize:hsize + toc_c])
    heap = blob[hsize + toc_c:]
    root = ET.fromstring(toc).find("toc")
    entries = {}

    def walk(node, prefix):
        for f in node.findall("file"):
            name = prefix + f.findtext("name")
            data = f.find("data")
            if data is not None and f.findtext("type") == "file":
                off, ln = int(data.findtext("offset")), int(data.findtext("length"))
                style = data.find("encoding").get("style") if data.find("encoding") is not None else ""
                raw = heap[off:off + ln]
                entries[name] = zlib.decompress(raw) if style.endswith("gzip") else raw
            else:
                entries[name + "/"] = None
            walk(f, name + "/")

    walk(root, "")
    return entries


def macho_arch(path):
    head = open(path, "rb").read(32)
    if head[:4] == b"\xcf\xfa\xed\xfe":
        return [CPU.get(struct.unpack("<i", head[4:8])[0], "?")]
    if head[:4] == b"\xca\xfe\xba\xbe":
        n = struct.unpack(">I", head[4:8])[0]
        full = open(path, "rb").read(8 + 20 * n)
        return [CPU.get(struct.unpack(">i", full[8 + 20 * i:12 + 20 * i])[0], "?") for i in range(n)]
    return ["非 Mach-O"]


def main():
    pkg = sys.argv[1]
    names_only = "--names" in sys.argv
    ent = read_xar(pkg)
    if names_only:
        for n in sorted(ent):
            print(n)
        return
    print(f"== {os.path.basename(pkg)}（{os.path.getsize(pkg)} 字节，{sum(1 for v in ent.values() if v is not None)} 个文件条目）")
    dist = ent["Distribution"].decode("utf-8")
    print("  Distribution 标题:", re.findall(r"<title>(.*?)</title>", dist))
    print("  最低系统:", re.findall(r'os-version min="([^"]+)"', dist), "| 宿主架构限制:", re.findall(r'hostArchitectures="([^"]+)"', dist))
    print("  pkg-ref 版本:", sorted(set(re.findall(r'<pkg-ref id="([^"]+)" version="([^"]+)"', dist))))
    for comp in sorted({n.split("/")[0] for n in ent if n.endswith(".pkg/") or ".pkg/" in n}):
        info = ET.fromstring(ent[f"{comp}/PackageInfo"].decode("utf-8"))
        print(f"  [{comp}] identifier={info.get('identifier')} version={info.get('version')} install-location={info.get('install-location')}")
        tmp = tempfile.mkdtemp(prefix="pkgchk.")
        try:
            cpio = gzip.decompress(ent[f"{comp}/Payload"])
            subprocess.run(["cpio", "-idm", "--quiet"], input=cpio, cwd=tmp, check=True, stderr=subprocess.DEVNULL)
            bundles = [d for d in os.listdir(tmp) if not d.startswith("._")]  # AppleDouble 元数据影子文件不是 bundle
            for b in bundles:
                plist_path = os.path.join(tmp, b, "Contents", "Info.plist")
                exe_dir = os.path.join(tmp, b, "Contents", "MacOS")
                pl = plistlib.load(open(plist_path, "rb"))
                exes = os.listdir(exe_dir)
                archs = {e: macho_arch(os.path.join(exe_dir, e)) for e in exes}
                extra = ""
                if "AudioComponents" in pl:
                    extra = " AU:" + str([(c.get("type"), c.get("subtype"), c.get("manufacturer"), c.get("version")) for c in pl["AudioComponents"]])
                nfiles = sum(len(fs) for _, _, fs in os.walk(os.path.join(tmp, b)))
                sig = os.path.isdir(os.path.join(tmp, b, "Contents", "_CodeSignature"))
                print(f"     {b}: 版本 {pl.get('CFBundleShortVersionString')} / 构建号 {pl.get('CFBundleVersion')}，架构 {archs}，{nfiles} 个文件，_CodeSignature={'有' if sig else '无'}{extra}")
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


main()
