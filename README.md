# NTFS3NT - NTFS File System Driver for Windows NT 3.1 - 4.0

NTFS3NT is a from-scratch NTFS driver that brings the **modern NTFS on-disk
format** - the NTFS 3.x that Windows 2000 through Windows 11 write - to
Windows NT 3.1, 3.5, 3.51 and 4.0, read and write. It also keeps working with
the older **NTFS 1.x** volumes those systems make themselves, so one
`ntfs.sys` serves both the disks of its own era and disks from any later
Windows.

It is an installable file system (IFS) built like its siblings
[FATNT](https://github.com/ages2001/FATNT) and
[EXFATNT](https://github.com/ages2001/EXFATNT): one source tree, one universal
binary for every NT from 3.1 to 4.0, a kernel-free NTFS engine shared with the
tools and the Linux tests, and an NT driver layer on top.

## At a glance

| | |
|---|---|
| **Version** | 1.0 |
| **File systems** | NTFS 3.0 / 3.1 (Windows 2000 - 11), NTFS 1.1 / 1.2 (Windows NT 3.1 - 4.0) |
| **Windows** | NT 3.1, 3.5, 3.51, 4.0 (x86); also runs on Windows 2000 |
| **Access** | read/write |
| **Paging file** | yes (tested on Windows 2000) |
| **Tools** | `ntfsinst` (installer), `ntfsfmt` (formatter), `ntfschk` (checker) |
| **Binary** | one `ntfs.sys`: native subsystem 3.10, 486-safe, imports only what NT 3.1's kernel exports |

## Why

NT 3.1 - 4.0 shipped NTFS 1.x. A volume formatted by Windows 2000 or later is
NTFS 3.x, and Windows 2000 upgrades an NTFS 1.x volume to 3.x the first time
it mounts it; NT 3.1 - 3.51 and NT 4.0 before Service Pack 4 cannot mount
such a volume at all. NTFS3NT lets them use a disk shared with, or formatted
by, a newer Windows - and gives NT 3.1 - 4.0 a newer NTFS driver for their own
volumes.

Windows 2000 and later ship their own NTFS driver, which should be used there.
NTFS3NT runs on Windows 2000 too (it was validated there), but it is meant for
NT 3.1 - 4.0.

## Features

**Reading**

- Boot sector and geometry, `$MFT` bootstrapped from its own run list, any
  record size and index block size, update-sequence (fixup) checks on every
  `FILE` and `INDX` record.
- Resident and non-resident attributes, data runs with sparse holes, files
  larger than 4 GB, `$ATTRIBUTE_LIST` with extension records (heavily
  fragmented files).
- **Compressed files** (LZNT1, per compression unit), read byte-exact.
- Directories of any size: `$INDEX_ROOT` and the `$INDEX_ALLOCATION` B-tree,
  wildcard searches, long Unicode names and 8.3 short names.
- **Named streams** (`file:stream`), opened and read, listed through
  `FileStreamInformation`.
- **Reparse points**: the tag is reported and `FSCTL_GET_REPARSE_POINT`
  returns the data (links and junctions are not followed).
- DOS attributes and times from `$STANDARD_INFORMATION`, the volume label,
  free space from `$Bitmap`. System files are hidden from listings, as on
  Windows.

**Writing**

- Create, overwrite, append, extend and truncate files; `md` / `rd`; delete;
  rename and move (also across directories); attributes and times.
- Small files stay resident in their MFT record and move to clusters as they
  grow; clusters come from `$Bitmap`, run lists are re-encoded and extended.
- Directory index insert and remove at any depth, with leaf splits, merges
  and the root moving from `$INDEX_ROOT` to an `INDX` B-tree.
- `$MFT` grows when it is full; fragmented files get an `$ATTRIBUTE_LIST`
  and extension records.
- New files get an 8.3 short name next to their long name, and inherit their
  directory's security (`$Secure` id on 3.x, a security descriptor on 1.x).
- On an NTFS 1.x volume new records are made the 1.x way (update sequence
  array at 0x2A, 0x30-byte `$STANDARD_INFORMATION`), so NT 3.1 - 4.0's own
  driver and `CHKDSK` accept them.
- Writes go through to the disk, so the volume is consistent after every
  operation.

**The driver**

- Cache Manager integration (cached reads, fast I/O, memory-mapped files and
  executables), paging I/O and **a paging file on NTFS**.
- Volume lock, unlock and dismount, volume (DASD) handles for tools such as
  `CHKDSK` and `ntfsfmt`, flush and shutdown.
- One binary for NT 3.1 - 4.0: the executive-resource API is bound to the
  running kernel at load time (the legacy API on NT 3.1, the Lite one on 3.51
  and later), the dispatch table stops at the IRP majors NT 3.1 has, and the
  image is stripped and checksummed for the strict NT 3.51+ loader.

## Tested

Under QEMU on real installs, each with NTFS3NT in place of the system's
`ntfs.sys`:

| System | Volume | What was done |
|---|---|---|
| Windows NT 3.1 | NTFS 1.1, formatted and filled by NT 3.1 | read byte-exact (`fc /b`), create, 142-file wildcard copy (index split, `$MFT` growth), rename, delete, `rd`; then NT 3.1's own `ntfs.sys` reads every new file byte-exact and its `CHKDSK` reports no errors |
| Windows NT 3.1 | NTFS 3.1 (`mkntfs`) | `vol`, `dir`, byte-exact reads of resident, non-resident and compressed files, bulk copy, rename, delete; `ntfsfix` clean and `ntfscat` matches afterwards |
| Windows NT 3.51 | NTFS 1.2, formatted and filled by NT 3.51 | the same run as above; NT 3.51's own driver reads everything back byte-exact |
| Windows NT 3.51 | NTFS 3.1 (`mkntfs`) | byte-exact reads, compressed files included |
| Windows NT 4.0 | NTFS 1.2, formatted and filled by NT 4.0 | the same run; NT 4.0's own driver reads everything back byte-exact and its `CHKDSK` reports no errors (851 files) |
| Windows NT 4.0 | NTFS 3.1 (`mkntfs`) | mount, listing, byte-exact reads |
| Windows 2000 | NTFS 3.x | create, overwrite, append, copy, delete, `md` / `rd`, rename / move, attributes, 8.3 names, large directories (a multi-level index B-tree), `$MFT` growth, heavily fragmented files, a paging file on NTFS3NT; Windows 2000's own `CHKDSK` then reports the volume clean |

NT 3.51's `CHKDSK` prints a "minor inconsistencies" note on volumes NT 3.51
itself formats and fills; after NTFS3NT has written to such a volume the note
is the same and nothing else is reported.

Windows XP has not been tried; it has its own NTFS driver anyway.

## Installing

Copy `bin\ntfs.sys` and `bin\ntfsinst.exe` to one folder and run, from an
administrator account:

```
ntfsinst                    install: keep the system's Ntfs.sys as Ntfs.sav and
                            put NTFS3NT in its place (every NTFS volume, the
                            boot volume included)
ntfsinst /DISABLE           NT 3.5 and later: install NTFS3NT as its own
                            boot-start "ntfsnt" service and disable the Ntfs
                            service, overwriting no file
ntfsinst /DISABLE /SYSTEM   the same, system start (data volumes only)
ntfsinst /DISABLE /KEEPFAT  the same, leaving the Ntfs service enabled
ntfsinst /U                 uninstall: restore Ntfs.sav, undo /DISABLE
```

A restart applies the change.

**Why replacing is the default.** NT 3.1's OS loader loads the boot volume's
file system by the fixed file name `Ntfs.sys`, before the service database is
read, and on NT 3.5 and later the `Ntfs` service loads that same file. Taking
its place is the one method that works the same on every NT from 3.1 to 4.0.
`ntfsinst` also makes sure the `Ntfs` service exists and is enabled - through
the registry API, so it works on NT 3.1 too, whose hive format offline
editors cannot change. The original is kept once as `Ntfs.sav`, and
`ntfsinst /U` puts it back.

`/DISABLE` overwrites nothing, but it is not enough for an NT 3.1 boot volume
(the loader still loads `Ntfs.sys`); on NT 3.1 use it for data volumes only.

`ntfsinst` needs no C runtime DLL, so it runs on a bare NT 3.1.

## Tools

```
ntfsfmt X: [/L:label] [/A:size]     format as NTFS 3.1
ntfschk X: [/V]                     check (read-only)
```

Both also take an image file, and both run on NT 3.1 and later without a C
runtime DLL.

- **ntfsfmt** lays out a new NTFS 3.1 volume, like `mkntfs`: the boot sector
  and its backup, all system files (`$MFT`, `$MFTMirr`, `$LogFile`, `$Volume`,
  `$AttrDef`, the root directory, `$Bitmap`, `$Boot`, `$BadClus`, `$Secure`,
  `$UpCase`, `$Extend`), a generated `$UpCase` table and the bitmaps.
  Cluster sizes from 512 bytes to 64 KB (`/A:4K` is the default, `/A:64K`
  the largest); the default label is `NTFS3NT`. It locks and dismounts the
  volume first and only buffers the metadata region, so large partitions
  format with little memory. Volumes it makes are clean for `ntfsfix`, and
  `ntfs-3g` reads and writes them.
- **ntfschk** checks the boot sector, bootstraps `$MFT`, walks every record in
  use (signature, fixups, attribute bounds) and the root index, and reports
  what it finds. It does not repair; use Windows' `CHKDSK` (through NTFS3NT on
  NT 3.1 - 4.0, or on a later Windows) for that.

## Limitations

- **No journal.** `$LogFile` is neither replayed nor written; every change is
  written through to the disk instead. A volume a later Windows left dirty
  (after a crash) should be checked there with `CHKDSK` before NT 3.1 - 4.0
  writes to it.
- Compressed files are read, not written (writing to one is refused).
- Encrypted (EFS) files are out of scope.
- A resident named stream cannot grow past its MFT record, and attribute
  lists cover the unnamed data stream only.
- Change notification is held pending rather than delivered (File Manager
  and Explorer refresh on demand, not by themselves) - it never spins.
- Reparse points are reported, not followed; no hard-link creation, quotas,
  object ids or extended attributes.
- No boot-time check (`autochk`) of its own.

## Building

On Linux with `gcc-mingw-w64-i686`, `binutils` and `python3` (no Visual C++
or DDK):

```
sh build/build_cross.sh [OUTDIR]    # ntfs.sys
sh build/build_tools.sh [OUTDIR]    # ntfsinst.exe, ntfsfmt.exe, ntfschk.exe
```

The driver is built with `-march=i486`, as native subsystem 3.10 with image
base `0x10000`, then stripped and given a valid PE checksum
(`build/pechecksum.py`); the NT 3.51+ loader rejects a driver without one as
"missing or corrupt". The tools are freestanding console programs with a
tiny built-in C runtime (`build/toolcrt.c`), importing only `KERNEL32` and
`ADVAPI32`. Ready binaries are in `bin\`.

## Testing

`python3 test/runtests.py` (needs gcc and ntfs-3g / ntfsprogs) builds the
engine in user mode and runs it against real NTFS images made with `mkntfs`:
boot sector, `$MFT`, system files and directories, resident, non-resident and
compressed data, named streams; then writing - in place, growing, resident to
non-resident, create and delete, a 45-file directory leaf split, a small
resident index - and a volume made by `ntfsfmt`. After every step
`ntfsfix -n` must report the volume clean and `ntfs-3g` must read back what
was written. This complements the tests on real Windows above.

## Source layout

| Path | What |
|---|---|
| `src/ntfsdisk.h` | on-disk structures (boot sector, MFT record, attributes, index) |
| `src/ntfssup.c` | the engine: geometry, fixups, attributes, data runs, index, LZNT1 |
| `src/ntfsidx.c` | directory index B-tree: lookup, insert / split, delete / merge |
| `src/ntfsext.c` | `$ATTRIBUTE_LIST` and extension records |
| `src/ntfsalloc.c` | cluster and MFT record allocation, `$MFT` growth, `$UpCase` |
| `src/ntfsfsctl.c` | mount, file system controls, reparse points |
| `src/ntfscreate.c` | open and create by path |
| `src/ntfsread.c`, `ntfsio.c` | reads: resident, non-resident, compressed, paging |
| `src/ntfswrite.c` | writes, create / delete / rename, set information, 8.3 names |
| `src/ntfscache.c` | Cache Manager integration and fast I/O |
| `src/ntfsdir.c` | directory listing, change notification |
| `src/ntfsinfo.c` | file and volume information |
| `src/ntfsvol.c` | volume handles, lock / dismount, disk IOCTL pass-through |
| `src/ntfsres.c` | resource API bound to the running kernel |
| `fmt/`, `chk/`, `inst/` | the tools |
| `build/` | build scripts, the tools' C runtime, PE checksum |
| `test/` | user-mode tests on Linux |

## License

GNU General Public License v3.0, see [LICENSE](LICENSE).

`src/NT/ntifs.h` is the free `ntifs.h` (release 58) by Bo Branten, GPL v2 or
later, included unchanged.
