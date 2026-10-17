#!/bin/sh
# Build the NTFS3NT user-mode tools as freestanding NT 3.1 console programs
# (native subsystem 3.10, 486-safe, no C runtime), the same way FATNT builds its
# tools. Produces ntfschk.exe and ntfsinst.exe in the output dir.
#
# Needs: gcc-mingw-w64-i686.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
OUT="${1:-/tmp/ntfsnt-tools}"
mkdir -p "$OUT"

GCC=i686-w64-mingw32-gcc
EXEBASE="-O2 -w -ffreestanding -nostdlib -nostartfiles -march=i486 -mtune=i486 -e _mainCRTStartup \
 -Wl,--subsystem,console -Wl,--major-subsystem-version,3 -Wl,--minor-subsystem-version,10 \
 -Wl,--major-os-version,1 -Wl,--minor-os-version,0 -D_WIN32 -D_CONSOLE"
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

echo "Built: $OUT/ntfschk.exe $OUT/ntfsinst.exe $OUT/ntfsfmt.exe"
