#!/usr/bin/env python3
# PlistBuddy 的替身（只模拟 Print / Set，保留原文件的 XML / 二进制格式），只用于在 Linux 上测 scripts/au-version.sh。
import plistlib, sys
args = sys.argv[1:]
assert args[0] == "-c" and len(args) == 3, args
cmd, path = args[1], args[2]
verb, _, rest = cmd.partition(" ")
entry, _, value = rest.partition(" ")
with open(path, "rb") as f:
    raw = f.read()
fmt = plistlib.FMT_BINARY if raw.startswith(b"bplist") else plistlib.FMT_XML
root = plistlib.loads(raw)
keys = [k for k in entry.split(":") if k]
def missing():
    print(f'{verb}: Entry, "{entry}", Does Not Exist'); sys.exit(1)
node, parent, last = root, None, None
for k in keys:
    parent, last = node, k
    if isinstance(node, list):
        if not k.isdigit() or int(k) >= len(node): missing()
        node = node[int(k)]
    elif isinstance(node, dict):
        if k not in node: missing()
        node = node[k]
    else: missing()
if verb == "Print":
    print(node if not isinstance(node, bool) else str(node).lower()); sys.exit(0)
if verb == "Set":
    t = type(node)
    new = int(value) if t is int else (value == "true") if t is bool else float(value) if t is float else value
    if isinstance(parent, list): parent[int(last)] = new
    else: parent[last] = new
    with open(path, "wb") as f: plistlib.dump(root, f, fmt=fmt)
    sys.exit(0)
print("unsupported", verb); sys.exit(2)
