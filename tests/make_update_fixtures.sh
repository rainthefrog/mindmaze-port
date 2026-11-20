#!/bin/sh
# Archives for tests/test_update.c: the same small MindMaze/ folder as .tar.gz (GNU tar) and .zip (Python zipfile,
# deflated, unix permissions and a symlink like the macOS app zip). Usage: make_update_fixtures.sh <dir>
set -e
out=$1
rm -rf "$out" && mkdir -p "$out/src/MindMaze/sub"
printf hello > "$out/src/MindMaze/a.txt"
python3 -c "import sys; sys.stdout.buffer.write(bytes((i * 7) & 255 for i in range(70000)))" > "$out/src/MindMaze/sub/b.bin"
printf '#!/bin/sh\n' > "$out/src/MindMaze/run.sh" && chmod 755 "$out/src/MindMaze/run.sh"
ln -s a.txt "$out/src/MindMaze/link"
tar -C "$out/src" -czf "$out/t.tar.gz" MindMaze
python3 -I - "$out" <<'EOF'
import os, stat, sys, zipfile
out = sys.argv[1]
with zipfile.ZipFile(os.path.join(out, "t.zip"), "w", zipfile.ZIP_DEFLATED) as z:
    for root, dirs, files in os.walk(os.path.join(out, "src")):
        for d in sorted(dirs):
            p = os.path.join(root, d)
            i = zipfile.ZipInfo(os.path.relpath(p, os.path.join(out, "src")) + "/")
            i.external_attr = (stat.S_IFDIR | 0o755) << 16
            i.create_system = 3
            z.writestr(i, b"")
        for f in sorted(files):
            p = os.path.join(root, f)
            i = zipfile.ZipInfo(os.path.relpath(p, os.path.join(out, "src")))
            i.create_system = 3
            if os.path.islink(p):
                i.external_attr = (stat.S_IFLNK | 0o777) << 16
                z.writestr(i, os.readlink(p))
            else:
                i.external_attr = (stat.S_IFREG | (os.stat(p).st_mode & 0o777)) << 16
                i.compress_type = zipfile.ZIP_DEFLATED
                z.writestr(i, open(p, "rb").read())
EOF
