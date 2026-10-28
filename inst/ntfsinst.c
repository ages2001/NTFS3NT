/*
 * NTFS3NT - ntfsinst: install or remove the NTFS3NT file system driver.
 *
 * NTFS3NT is meant for Windows NT 3.1, 3.5, 3.51 and 4.0, and runs on Windows
 * 2000 and XP (x86 and x64) too, which ship an NTFS driver of their own.
 *
 * On Windows 2000 and XP the system's Ntfs.sys is protected by Windows File
 * Protection, which puts it back as soon as it is overwritten. There NTFS3NT is
 * copied as drivers\ntfs3nt.sys and the Ntfs service's ImagePath is pointed at
 * it (the system's file stays as it is); /U removes the ImagePath again. As the
 * Ntfs service then loads NTFS3NT for every NTFS volume, an NTFS system volume
 * would be NTFS3NT's too: ntfsinst refuses that unless /FORCE is given.
 *
 *   ntfsinst            Install (default): back up the system Ntfs.sys to
 *                      Ntfs.sav and put NTFS3NT in its place, so NTFS3NT becomes
 *                      the NTFS file system for every NTFS volume, the boot volume
 *                      included. This "replace" method is uniform across NT 3.1
 *                      - 4.0: the NT 3.1 OS loader loads the boot file system by
 *                      the fixed name Ntfs.sys, and on NT 3.5+ the Ntfs
 *                      service loads that same file, so standing in for it is
 *                      what hands the boot volume to NTFS3NT everywhere.
 *   ntfsinst /DISABLE   NT 3.5+ alternative that overwrites no file: install
 *                      NTFS3NT as a separate boot-start "ntfsnt" service and set
 *                      the Ntfs service to disabled. (Not effective for the
 *                      NT 3.1 boot volume - see the README.)
 *   ntfsinst /SYSTEM    With /DISABLE: system-start instead of boot-start, so it
 *                      does not touch the boot volume (data volumes only).
 *   ntfsinst /KEEPFAT   With /DISABLE: leave the Ntfs service enabled too.
 *   ntfsinst /U         Uninstall: restore Ntfs.sav, and undo a /DISABLE
 *                      install (disable ntfsnt, re-enable Ntfs); on 2000/XP
 *                      give the Ntfs service back its own driver.
 *   ntfsinst /FORCE     2000/XP: install even though the system volume is NTFS.
 *
 * Everything is done with a file copy and the registry; the original Ntfs.sys
 * is preserved as Ntfs.sav and restored by /U. ntfsinst links against no C
 * runtime, so it needs no msvcrt/crtdll on NT 3.1. A restart applies it.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#define SVC_NTFS3NT   "SYSTEM\\CurrentControlSet\\Services\\ntfsnt"
#define SVC_NTFS "SYSTEM\\CurrentControlSet\\Services\\Ntfs"

#ifdef NTFSINST_LOG
static void LOGs(const char *s)
{
    HANDLE h = CreateFileA("C:\\NTFSINST.LOG", GENERIC_WRITE, 0, 0, OPEN_ALWAYS, 0, 0);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w; SetFilePointer(h, 0, 0, FILE_END);
        WriteFile(h, s, lstrlenA(s), &w, 0);
        CloseHandle(h);
    }
}
#define LOG(s) LOGs(s)
#else
#define LOG(s) ((void)0)
#endif

#define SERVICE_BOOT_START      0
#define SERVICE_SYSTEM_START    1
#define SERVICE_DISABLED        4
#define SERVICE_FILE_SYSTEM     2

static LONG SetDword(HKEY key, const char *name, DWORD val)
{
    return RegSetValueExA(key, name, 0, REG_DWORD, (const BYTE *)&val, sizeof(val));
}

static LONG SetSz(HKEY key, const char *name, const char *val)
{
    return RegSetValueExA(key, name, 0, REG_SZ, (const BYTE *)val, (DWORD)(strlen(val) + 1));
}

static int SetServiceStart(const char *path, DWORD start)
{
    HKEY key;
    LONG r = RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_SET_VALUE, &key);
    if (r != ERROR_SUCCESS) {
        return 0;       /* not present */
    }
    r = SetDword(key, "Start", start);
    RegFlushKey(key);
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

/* Set EnableWriteSupport=1 on an existing service key (ignored if absent). */
static void SetEnableWrite(const char *path)
{
    HKEY key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        SetDword(key, "EnableWriteSupport", 1);
        RegFlushKey(key);
        RegCloseKey(key);
    }
}

/*
 * Make the Ntfs service load our driver, with no manual regedit on any NT.
 * Runs inside the OS, so the live registry API applies even on NT 3.1 (whose
 * pre-3.5 hive format no offline editor can touch). We create/repair the whole
 * service definition: boot-start so an NTFS boot volume works, Type=file system,
 * the "Boot file system" group, ErrorControl=normal (a load failure logs and
 * boot continues, never a dead system). Idempotent - safe to re-run.
 */
static void EnableNtfsService(void)
{
    HKEY key;
    DWORD disp;
    LONG r = RegCreateKeyExA(HKEY_LOCAL_MACHINE, SVC_NTFS, 0, NULL,
                             REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL, &key, &disp);
    if (r != ERROR_SUCCESS) {
        /* fall back to a plain open+set if create is denied */
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, SVC_NTFS, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
            printf("  warning: could not open the Ntfs service key (error %ld).\n", r);
            return;
        }
    }
    SetDword(key, "Type", SERVICE_FILE_SYSTEM);
    SetDword(key, "Start", SERVICE_BOOT_START);     /* boot-start: owns boot volume too */
    SetDword(key, "ErrorControl", 1);               /* normal: a failure never blocks boot */
    SetSz(key, "Group", "Boot file system");
    RegFlushKey(key);
    RegCloseKey(key);
    printf("  Ntfs service enabled (boot-start) - no manual regedit needed.\n");
}

static void DriversPath(const char *leaf, char *out, DWORD cb)
{
    char sysdir[MAX_PATH];
    UINT n = GetWindowsDirectoryA(sysdir, sizeof(sysdir));
    if (n == 0 || n >= sizeof(sysdir)) {
        out[0] = 0;
        return;
    }
    sprintf(out, "%s\\System32\\drivers\\%s", sysdir, leaf);
    (void)cb;
}

/* Windows 2000 or later (NT 5.x): Windows File Protection guards Ntfs.sys */
static int IsNt5(void)
{
    DWORD v = GetVersion();
    return !(v & 0x80000000UL) && (v & 0xFF) >= 5;
}

/* Is the system volume NTFS? */
static int SystemVolumeIsNtfs(void)
{
    char dir[MAX_PATH], root[4], fs[32];
    DWORD serial, maxlen, flags;
    if (GetWindowsDirectoryA(dir, sizeof(dir)) == 0) return 0;
    root[0] = dir[0]; root[1] = ':'; root[2] = '\\'; root[3] = 0;
    if (!GetVolumeInformationA(root, NULL, 0, &serial, &maxlen, &flags, fs, sizeof(fs))) return 0;
    return lstrcmpiA(fs, "NTFS") == 0;
}

/* ------------------------------------------------------------------ */
/* Default install: NTFS3NT replaces Ntfs.sys (original kept as .sav) */
/* ------------------------------------------------------------------ */

/* ntfs.sys next to this program, else in the current folder (the
   installer is often run from another folder, a floppy or a CD) */
static void SourcePath(char *Out)
{
    char *Slash = NULL, *P;
    DWORD N = GetModuleFileNameA(NULL, Out, MAX_PATH);
    if (N != 0 && N < MAX_PATH) {
        for (P = Out; *P; P++) if (*P == '\\') Slash = P;
    }
    if (N == 0 || N >= MAX_PATH || Slash == NULL) {
        lstrcpyA(Out, "ntfs.sys");
        return;
    }
    lstrcpyA(Slash + 1, "ntfs.sys");
    if (GetFileAttributesA(Out) == 0xFFFFFFFF) lstrcpyA(Out, "ntfs.sys");
}

/*
 * Windows 2000 / XP: drivers\ntfs3nt.sys, loaded by the Ntfs service through
 * its ImagePath. Windows' own Ntfs.sys is not touched.
 */
static int Nt5Install(int force)
{
    char dst[MAX_PATH], src[MAX_PATH];
    HKEY key;
    LONG r;

    if (SystemVolumeIsNtfs() && !force) {
        printf("  The system volume is NTFS. With NTFS3NT installed it would be mounted\n");
        printf("  by NTFS3NT instead of Windows' own NTFS driver. Run ntfsinst /FORCE\n");
        printf("  to install anyway. Nothing was changed.\n");
        return 1;
    }

    SourcePath(src);
    DriversPath("ntfs3nt.sys", dst, sizeof(dst));
    if (dst[0] == 0) {
        printf("  cannot find the system directory.\n");
        return 1;
    }
    if (!CopyFileA(src, dst, FALSE)) {
        printf("  error: could not copy ntfs.sys to %s (error %lu).\n", dst, GetLastError());
        printf("  put ntfs.sys next to ntfsinst.exe and run it again.\n");
        return 1;
    }
    printf("  installed %s\n", dst);

    r = RegOpenKeyExA(HKEY_LOCAL_MACHINE, SVC_NTFS, 0, KEY_ALL_ACCESS, &key);
    if (r != ERROR_SUCCESS) {
        printf("  error: cannot open the Ntfs service key (error %ld).\n", r);
        return 1;
    }
    SetSz(key, "ImagePath", "System32\\DRIVERS\\ntfs3nt.sys");
    SetDword(key, "EnableWriteSupport", 1);
    SetDword(key, "Ntfs3ntInstalled", 1);
    RegFlushKey(key);
    RegCloseKey(key);

    printf("  the Ntfs service now loads NTFS3NT (Windows' Ntfs.sys is left as it is).\n");
    printf("Done. Restart to load NTFS3NT.\n");
    return 0;
}

static void Nt5Uninstall(void)
{
    HKEY key;
    DWORD v = 0, cb = sizeof(v), type;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, SVC_NTFS, 0, KEY_ALL_ACCESS, &key) != ERROR_SUCCESS) {
        return;
    }
    if (RegQueryValueExA(key, "Ntfs3ntInstalled", NULL, &type, (BYTE *)&v, &cb) == ERROR_SUCCESS && v) {
        RegDeleteValueA(key, "ImagePath");
        RegDeleteValueA(key, "Ntfs3ntInstalled");
        RegFlushKey(key);
        printf("  the Ntfs service loads Windows' own Ntfs.sys again.\n");
    }
    RegCloseKey(key);
}

static int ReplaceInstall(void)
{
    char ff[MAX_PATH], sav[MAX_PATH], src[MAX_PATH];

    LOG("replace install\r\n");
    SourcePath(src);

    DriversPath("ntfs.sys", ff, sizeof(ff));
    DriversPath("ntfs.sav", sav, sizeof(sav));
    if (ff[0] == 0) {
        printf("  cannot find the system directory.\n");
        return 1;
    }

    /* Keep the original Ntfs.sys once, as Ntfs.sav */
    if (GetFileAttributesA(sav) == 0xFFFFFFFF && GetFileAttributesA(ff) != 0xFFFFFFFF) {
        if (CopyFileA(ff, sav, TRUE)) {
            printf("  backed up Ntfs.sys to Ntfs.sav\n");
        } else {
            printf("  warning: could not back up Ntfs.sys (error %lu).\n", GetLastError());
        }
    }
    LOG("backed up\r\n");

    if (!CopyFileA(src, ff, FALSE)) {
        printf("  error: could not install NTFS3NT as %s (error %lu).\n", ff, GetLastError());
        printf("  put ntfs.sys next to ntfsinst.exe and run it again.\n");
        return 1;
    }
    LOG("copied ntfs over system ntfs\r\n");

    /* NTFS3NT loads under the Ntfs service name, so make sure that service is
     * present and enabled (turnkey on every NT, NT 3.1 included), and carry our
     * write-enable flag there. */
    EnableNtfsService();
    SetEnableWrite(SVC_NTFS);
    LOG("service enabled + enablewrite set\r\n");

    printf("  installed NTFS3NT as the NTFS file system (Ntfs.sys; original saved as Ntfs.sav).\n");
    printf("Done. Restart to load NTFS3NT.\n");
    LOG("done\r\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* /DISABLE: NTFS3NT as its own service, Ntfs service disabled        */
/* (NT 3.5+; overwrites no file)                                       */
/* ------------------------------------------------------------------ */

static int DisableInstall(DWORD start, int disableFastfat)
{
    HKEY key;
    DWORD disp;
    char dst[MAX_PATH], src[MAX_PATH];
    LONG r;
    int fastfatDone = 0;

    LOG("disable install\r\n");

    r = RegCreateKeyExA(HKEY_LOCAL_MACHINE, SVC_NTFS3NT, 0, NULL, REG_OPTION_NON_VOLATILE,
                        KEY_ALL_ACCESS, NULL, &key, &disp);
    if (r != ERROR_SUCCESS) {
        printf("  cannot create the ntfsnt service key (error %ld).\n", r);
        return 1;
    }
    SetDword(key, "Type", SERVICE_FILE_SYSTEM);
    SetDword(key, "Start", start);
    SetDword(key, "ErrorControl", 1);
    SetSz(key, "Group", "Boot file system");
    /* (2000/XP: under its own name, Windows File Protection guards Ntfs.sys) */
    SetSz(key, "ImagePath", IsNt5() ? "System32\\DRIVERS\\ntfs3nt.sys" : "System32\\DRIVERS\\ntfs.sys");
    SetDword(key, "EnableWriteSupport", 1);
    RegFlushKey(key);
    RegCloseKey(key);

    if (disableFastfat) {
        fastfatDone = SetServiceStart(SVC_NTFS, SERVICE_DISABLED);
    }

    DriversPath(IsNt5() ? "ntfs3nt.sys" : "ntfs.sys", dst, sizeof(dst));
    SourcePath(src);
    if (dst[0] && !CopyFileA(src, dst, FALSE)) {
        printf("  warning: could not copy ntfs.sys to %s (error %lu).\n", dst, GetLastError());
        printf("  copy it there yourself, then restart.\n");
    } else if (dst[0]) {
        printf("  NTFS3NT copied to %s.\n", dst);
    }

    printf("  ntfsnt service created (%s start).\n",
           start == SERVICE_BOOT_START ? "boot" : "system");
    if (!disableFastfat) {
        printf("  Ntfs left enabled.\n");
    } else if (fastfatDone) {
        printf("  Ntfs service disabled.\n");
    } else {
        printf("  note: Ntfs service not found (nothing to disable).\n");
    }
    printf("Done. Restart to load NTFS3NT.\n");
    LOG("done\r\n");
    return 0;
}

/* ------------------------------------------------------------------ */

static int Uninstall(void)
{
    char ff[MAX_PATH], sav[MAX_PATH];

    /* Undo a /DISABLE install */
    if (SetServiceStart(SVC_NTFS3NT, SERVICE_DISABLED)) {
        printf("  ntfsnt service disabled.\n");
    }
    SetServiceStart(SVC_NTFS, SERVICE_SYSTEM_START);
    Nt5Uninstall();

    /* Undo a replace install: restore the original Ntfs.sys */
    DriversPath("ntfs.sys", ff, sizeof(ff));
    DriversPath("ntfs.sav", sav, sizeof(sav));
    if (ff[0] && GetFileAttributesA(sav) != 0xFFFFFFFF) {
        if (CopyFileA(sav, ff, FALSE)) {
            printf("  restored the original Ntfs.sys from Ntfs.sav.\n");
        } else {
            printf("  warning: could not restore Ntfs.sys (error %lu).\n", GetLastError());
        }
    }
    printf("Done. Restart to apply.\n");
    return 0;
}

int main(int argc, char **argv)
{
    DWORD start = SERVICE_BOOT_START;
    int disableMode = 0;
    int disableFastfat = 1;
    int force = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (!_stricmp(argv[i], "/U") || !_stricmp(argv[i], "-U")) {
            return Uninstall();
        } else if (!_stricmp(argv[i], "/DISABLE")) {
            disableMode = 1;
        } else if (!_stricmp(argv[i], "/KEEPFAT")) {
            disableMode = 1;
            disableFastfat = 0;
        } else if (!_stricmp(argv[i], "/FORCE")) {
            force = 1;
        } else if (!_stricmp(argv[i], "/SYSTEM")) {
            disableMode = 1;
            start = SERVICE_SYSTEM_START;
            disableFastfat = 0;     /* a system-start driver never owns the boot volume */
        } else {
            printf("usage: ntfsinst [/DISABLE [/SYSTEM] [/KEEPFAT]] [/FORCE] [/U]\n");
            return 2;
        }
    }

    printf("NTFS3NT installer\n");

    if (disableMode) {
        return DisableInstall(start, disableFastfat);
    }
    if (IsNt5()) {
        return Nt5Install(force);
    }
    return ReplaceInstall();
}
