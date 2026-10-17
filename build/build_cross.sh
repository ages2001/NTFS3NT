#!/bin/sh
# NTFS3NT cross build: a native NT 3.1 ntfs.sys with GCC + the mingw-w64 DDK
# headers/import libs, no Visual C++. For producing and boot-testing an NT 3.1
# binary on Linux. (The shipping build would use Visual C++ like FATNT's.)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
SRC="$ROOT/src"
OUT="${1:-/tmp/ntfsnt-cross}"
mkdir -p "$OUT"

GCC=i686-w64-mingw32-gcc
DLLTOOL=i686-w64-mingw32-dlltool
NM=i686-w64-mingw32-nm
INC="-I$SRC -I/usr/i686-w64-mingw32/include/ddk -I/usr/i686-w64-mingw32/include"
FLAGS="-c -ffreestanding -fno-stack-protector -fno-builtin -march=i486 -mtune=i486 \
 -D_X86_ -DWIN32 -D_WIN32_WINNT=0x0600 -DNTDDI_VERSION=0x06000000 -DNTFS_CROSS -w $EXTRACFLAGS"

echo "Compiling driver sources..."
rm -f "$OUT"/*.o
for f in ntfsinit ntfsdisp ntfsio ntfsmft ntfsfsctl ntfscreate ntfsread ntfswrite ntfsclose ntfsdir ntfsinfo ntfsres ntfssup ntfscache ntfsalloc ntfsvol ntfsidx ntfsext; do
    $GCC $FLAGS $INC "$SRC/$f.c" -o "$OUT/$f.o"
done
$GCC $FLAGS $INC "$HERE/ntcrt.c" -o "$OUT/ntcrt.o"

echo "Building the NT 3.1 ntoskrnl import library..."
$NM "$OUT"/*.o 2>/dev/null | awk '/ U /{print $2}' | grep '^__imp_' \
    | sed 's/^__imp_//; s/^_//' | sort -u > "$OUT/need.txt"
{ echo "LIBRARY ntoskrnl.exe"; echo "EXPORTS"; cat "$OUT/need.txt"; } > "$OUT/ntoskrnl.def"
$DLLTOOL --kill-at -d "$OUT/ntoskrnl.def" -l "$OUT/libntoskrnl_nt31.a"

echo "Linking ntfs.sys (native, subsystem 3.10, base 0x10000)..."
$GCC -shared -nostdlib -nostartfiles \
    -Wl,--subsystem,native -Wl,--major-subsystem-version,3 -Wl,--minor-subsystem-version,10 \
    -Wl,--major-image-version,1 -Wl,--minor-image-version,0 \
    -Wl,--major-os-version,1 -Wl,--minor-os-version,0 \
    -Wl,--image-base,0x10000 -Wl,--entry,_DriverEntry@8 -Wl,--file-alignment,0x200 \
    -o "$OUT/ntfs.sys" "$OUT"/*.o "$OUT/libntoskrnl_nt31.a" -lgcc
i686-w64-mingw32-strip --strip-all "$OUT/ntfs.sys"
python3 "$HERE/pechecksum.py" "$OUT/ntfs.sys"
echo "Built: $OUT/ntfs.sys"
