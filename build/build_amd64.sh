#!/bin/sh
# NTFS3NT for Windows XP x64 / Server 2003 x64 (amd64), with GCC
# (gcc-mingw-w64-x86-64): ntfs.sys and the tools (ntfsinst, ntfsfmt, ntfschk).
#
#   sh build/build_amd64.sh [OUTDIR]
#
# XP64_EXPORTS / XP64_USER_EXPORTS: optional lists of the names XP x64's
# ntoskrnl/hal and kernel32/advapi32 export, to check the imports against.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
SRC="$ROOT/src"
OUT="${1:-/tmp/ntfsnt-amd64}"
mkdir -p "$OUT"

GCC=x86_64-w64-mingw32-gcc
INC="-I$SRC -I/usr/x86_64-w64-mingw32/include/ddk -I/usr/x86_64-w64-mingw32/include"
# The mingw DDK headers want the Vista level to be self-consistent; only
# functions XP x64 exports are imported (checked below).
FLAGS="-c -O1 -ffreestanding -fno-stack-protector -fno-builtin -mno-stack-arg-probe -fno-strict-aliasing \
 -D_AMD64_ -DWIN32 -D_WIN64 -D_WIN32_WINNT=0x0600 -DNTDDI_VERSION=0x06000000 -DNTFS_AMD64 -w $EXTRACFLAGS"

echo "Compiling driver sources (amd64)..."
rm -f "$OUT"/*.o
for f in ntfsinit ntfsdisp ntfsio ntfsmft ntfsfsctl ntfscreate ntfsread ntfswrite ntfsclose ntfsdir ntfsinfo ntfsres ntfssup ntfscache ntfsalloc ntfsvol ntfsidx ntfsext ntfsjnl ntfssec ntfsea ntfsfeat; do
    $GCC $FLAGS $INC "$SRC/$f.c" -o "$OUT/$f.o"
done
$GCC $FLAGS $INC "$HERE/ntcrt.c" -o "$OUT/ntcrt.o"

echo "Linking ntfs.sys (native, subsystem 5.02)..."
$GCC -shared -nostdlib -nostartfiles \
    -Wl,--subsystem,native -Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,2 \
    -Wl,--major-os-version,5 -Wl,--minor-os-version,2 \
    -Wl,--major-image-version,1 -Wl,--minor-image-version,1 \
    -Wl,--image-base,0x10000 -Wl,--entry,DriverEntry -Wl,--file-alignment,0x200 \
    -Wl,--section-alignment,0x1000 \
    -o "$OUT/ntfs.sys" "$OUT"/*.o -lntoskrnl -lhal -lgcc
x86_64-w64-mingw32-strip --strip-all "$OUT/ntfs.sys"
python3 "$HERE/pechecksum.py" "$OUT/ntfs.sys"
rm -f "$OUT"/*.o

if [ -f "$XP64_EXPORTS" ]; then
    echo "Checking imports against XP x64..."
    x86_64-w64-mingw32-objdump -p "$OUT/ntfs.sys" | awk '/^\t[0-9a-f]+\t +[0-9]+ +[A-Za-z_]/{print $3}' | while read s; do
        grep -qx "$s" "$XP64_EXPORTS" || echo "  NOT EXPORTED BY XP x64: $s"
    done
fi
echo "Built: $OUT/ntfs.sys"
[ -n "$DRIVER_ONLY" ] && exit 0

# ----------------------------------------------------------------------
# Tools (console, no C runtime DLL)
# ----------------------------------------------------------------------
EXEBASE="-O2 -w -ffreestanding -nostdlib -nostartfiles -mno-stack-arg-probe -e mainCRTStartup \
 -Wl,--subsystem,console -Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,2 \
 -Wl,--major-os-version,5 -Wl,--minor-os-version,2 -D_WIN32 -D_CONSOLE"
TOOLCRT="-include $HERE/toolcrt.h \
 -D_INC_STDIO -D_INC_STDLIB -D_INC_STRING -D_INC_CTYPE -D_INC_TIME \
 -D_STDIO_H_=1 -D_STDLIB_H=1 -D_STRING_H_=1 -D_CTYPE_H_=1 -D_TIME_H_=1 \
 -Dmain=appmain -Dprintf=fat_printf -Dfprintf=fat_fprintf -Dsprintf=fat_sprintf -Dsnprintf=fat_snprintf \
 -Dmalloc=fat_malloc -Dfree=fat_free -Dcalloc=fat_calloc -Drealloc=fat_realloc \
 -Dstrtoul=fat_strtoul -Dtime=fat_time -Dlocaltime=fat_localtime -Dtoupper=fat_toupper -D_stricmp=fat_stricmp"

echo "Building ntfschk.exe..."
$GCC $EXEBASE $TOOLCRT -I"$ROOT/src" -o "$OUT/ntfschk.exe" \
    "$ROOT/chk/ntfschk.c" "$ROOT/src/ntfssup.c" "$HERE/toolcrt.c" -lkernel32 -lgcc
python3 "$HERE/pechecksum.py" "$OUT/ntfschk.exe"

echo "Building ntfsinst.exe..."
$GCC $EXEBASE -D_STDIO_H_=1 -D_STRING_H_=1 -D_INC_STDIO -D_INC_STRING \
    -Dmain=appmain -Dprintf=fat_printf -Dsprintf=fat_sprintf -D_stricmp=fat_stricmp -Dstrlen=fat_strlen \
    -o "$OUT/ntfsinst.exe" "$ROOT/inst/ntfsinst.c" "$HERE/nocrt.c" -ladvapi32 -lkernel32
python3 "$HERE/pechecksum.py" "$OUT/ntfsinst.exe"

echo "Building ntfsfmt.exe..."
$GCC $EXEBASE -D_STDIO_H_=1 -D_INC_STDIO \
    -Dmain=appmain -Dprintf=fat_printf \
    -I"$ROOT/fmt" -o "$OUT/ntfsfmt.exe" "$ROOT/fmt/ntfsfmt.c" "$HERE/nocrt.c" -lkernel32 -lgcc
python3 "$HERE/pechecksum.py" "$OUT/ntfsfmt.exe"

if [ -f "$XP64_USER_EXPORTS" ]; then
    echo "Checking the tools' imports against XP x64..."
    for f in "$OUT"/*.exe; do
        x86_64-w64-mingw32-objdump -p "$f" | awk '/^\t[0-9a-f]+\t +[0-9]+ +[A-Za-z_]/{print $3}' | while read s; do
            grep -qx "$s" "$XP64_USER_EXPORTS" || echo "  $(basename "$f"): $s is not in XP x64"
        done
    done
fi
echo "Built: $OUT/ntfs.sys $OUT/ntfschk.exe $OUT/ntfsinst.exe $OUT/ntfsfmt.exe"
