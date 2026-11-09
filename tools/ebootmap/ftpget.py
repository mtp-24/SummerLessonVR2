#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""ftpget.py REMOTE_DIR [LOCAL_DIR [MAX_BYTES]]: a folder from the console's FTP server, files
over MAX_BYTES (150 MB) left out; with no LOCAL_DIR, only listed. The console is PS5_HOST
(tools/local.env, as testrun.sh), port PS5_FTP_PORT or 2121. Read-only on the console.

A PS4 game's files are served unlocked while it is open, its eboot.bin among them:
    tools/ebootmap/ftpget.py /mnt/sandbox/pfsmnt/CUSA07434-app0 dumps/CUSA07434/app0
"""
import ftplib
import os
import sys


def host():
    h = os.environ.get("PS5_HOST")
    env = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "local.env")
    if not h and os.path.exists(env):
        for line in open(env):
            k, _, v = line.strip().partition("=")
            if k.replace("export ", "") == "PS5_HOST":
                h = v.strip("\"'")
    if not h:
        sys.exit("PS5_HOST is not set (tools/local.env)")
    return h


def walk(f, path):
    lines = []
    f.retrlines("LIST " + path, lines.append)
    for l in lines:
        p = l.split(None, 8)
        if len(p) < 9 or p[8] in (".", ".."):
            continue
        if l.startswith("l"):
            continue  # a symlink: its name is "name -> target"
        full = path.rstrip("/") + "/" + p[8]
        if l.startswith("d"):
            yield from walk(f, full)
        else:
            yield full, int(p[4])


def main():
    root = sys.argv[1]
    dest = sys.argv[2] if len(sys.argv) > 2 else ""
    cap = int(sys.argv[3]) if len(sys.argv) > 3 else 150 << 20
    f = ftplib.FTP()
    f.connect(host(), int(os.environ.get("PS5_FTP_PORT", "2121")), timeout=30)
    f.login()
    total = 0
    for full, size in walk(f, root):
        rel = full[len(root):].lstrip("/")
        take = size <= cap
        print("get " if take and dest else "skip" if not take else "    ", size, rel)
        if take and dest:
            out = os.path.join(dest, rel)
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with open(out, "wb") as o:
                f.retrbinary("RETR " + full, o.write)
            total += size
    if dest:
        print("fetched", total, "bytes")
    f.quit()


if __name__ == "__main__":
    main()
