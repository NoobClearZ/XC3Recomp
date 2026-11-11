"""RomFS manifest: aggregate per-directory counts/bytes plus a sha256 over sorted "path<TAB>size" lines.

Only aggregates and a hash are stored (Constitution art. I): no list of game file names is published.
"""
import hashlib
import os

# Files an OS adds to a folder it browsed, never part of the game: macOS Finder (.DS_Store, AppleDouble "._" files)
# and Windows Explorer (Thumbs.db, desktop.ini). A dump copied through a Mac otherwise failed the check with one
# extra file at the root.
OS_JUNK = frozenset({'.ds_store', 'thumbs.db', 'desktop.ini'})


def is_os_junk(name):
    return name.lower() in OS_JUNK or name.startswith('._')


def build_manifest(romfs):
    dirs, lines = {}, []
    for root, _, files in os.walk(romfs):
        for f in files:
            if is_os_junk(f):
                continue
            p = os.path.join(root, f)
            rel = os.path.relpath(p, romfs).replace(os.sep, '/')
            size = os.path.getsize(p)
            top = rel.split('/', 1)[0] if '/' in rel else '.'
            d = dirs.setdefault(top, [0, 0])
            d[0] += 1
            d[1] += size
            lines.append(f'{rel}\t{size}')
    lines.sort()
    h = hashlib.sha256('\n'.join(lines).encode('utf-8')).hexdigest()
    return {'dirs': dict(sorted(dirs.items())), 'files': len(lines), 'bytes': sum(v[1] for v in dirs.values()), 'sha256': h}


if __name__ == '__main__':
    import json
    import sys
    print(json.dumps(build_manifest(sys.argv[1]), indent=1))
