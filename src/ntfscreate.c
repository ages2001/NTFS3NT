/*
 * NTFS3NT - create/open. Resolves a path from the root by looking each component
 * up in its parent directory's $I30 index, and opens the target as an FCB.
 * Creates files and directories (with 8.3 aliases) when a create disposition
 * asks for it; also handles relative, volume (DASD) and target-directory opens.
 */

#define NTFS_KERNEL
#include "ntfs.h"

static const WCHAR Idx30[4] = { '$', 'I', '3', '0' };

/* name lookups use the index B-tree (NtfsLookupInDir, ntfsidx.c) */


/* Fill in an FCB's sizes/flags from its MFT record. */
static void
LoadFcbInfo(PNTFS_VCB Vcb, PNTFS_FCB Fcb)
{
    PUCHAR rec;
    PMFT_RECORD m;
    PATTRIBUTE_RECORD a;

    rec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    if (rec == NULL) return;
    if (!NT_SUCCESS(NtfsReadMftRecord(Vcb, Fcb->MftRecord, rec))) { ExFreePool(rec); return; }
    m = (PMFT_RECORD)rec;
    Fcb->Sequence = m->SequenceNumber;

    /* DOS attributes (readonly/hidden/system/archive/sparse/compressed/reparse)
     * come from $STANDARD_INFORMATION; surface them for info queries. */
    {
        PATTRIBUTE_RECORD si = NtfsFindAttribute(m, ATTR_STANDARD_INFORMATION, NULL, 0);
        if (si && !si->NonResident) {
            PSTANDARD_INFORMATION s = (PSTANDARD_INFORMATION)NtfsResidentValue(si);
            Fcb->FileAttributes = s->FileAttributes & NTFS_DOS_ATTR_MASK;
            Fcb->CreationTime = s->CreationTime;
            Fcb->LastWriteTime = s->ChangeTime;      /* data modification */
            Fcb->ChangeTime = s->MftChangeTime;      /* MFT change */
            Fcb->LastAccessTime = s->LastAccessTime;
        }
    }
    /* reparse point: record the tag and flag it (symlink/junction/etc.) */
    {
        PATTRIBUTE_RECORD rp = NtfsFindAttribute(m, ATTR_REPARSE_POINT, NULL, 0);
        if (rp) {
            Fcb->FileAttributes |= FILE_ATTRIBUTE_REPARSE_POINT;
            if (!rp->NonResident && NtfsResidentValueLength(rp) >= 4) {
                Fcb->ReparseTag = *(PULONG)NtfsResidentValue(rp);
            }
        }
    }

    if (m->Flags & MFT_RECORD_IS_DIRECTORY) {
        Fcb->FcbState |= FCB_STATE_DIRECTORY;
        Fcb->FileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
        Fcb->Header.FileSize.QuadPart = 0;
        Fcb->Header.AllocationSize.QuadPart = 0;
        Fcb->Header.ValidDataLength.QuadPart = 0;
    } else {
        UCHAR first[0x48];
        PATTRIBUTE_RECORD f = (PATTRIBUTE_RECORD)first;
        PNTFS_RUN runs = NULL; ULONG rc = 0;
        BOOLEAN hasList = FALSE;
        NTSTATUS ls;
        if (Fcb->FileAttributes == 0) Fcb->FileAttributes = FILE_ATTRIBUTE_NORMAL;
        /* the unnamed $DATA: in the base record, or spread over extension
         * records through an $ATTRIBUTE_LIST (very fragmented files) */
        ls = NtfsLoadDataExtents(Vcb, Fcb->MftRecord, m, &runs, &rc, f, &hasList);
        a = (NT_SUCCESS(ls) && !f->NonResident) ? NtfsFindAttribute(m, ATTR_DATA, NULL, 0) : NULL;
        if (!NT_SUCCESS(ls) || (!f->NonResident && a == NULL)) {
            Fcb->Header.FileSize.QuadPart = 0;
            Fcb->DataResident = TRUE;
            Fcb->Header.AllocationSize.QuadPart = 0;
        } else if (!f->NonResident) {
            ULONG vlen = NtfsResidentValueLength(a);
            Fcb->DataResident = TRUE;
            Fcb->Header.FileSize.QuadPart = vlen;
            /* cache the resident bytes now (MFT record is in hand) so the read
             * path needs no disk I/O - avoids a nested synchronous read. */
            if (vlen) {
                Fcb->ResidentData = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, vlen, NTFS_TAG_BUF);
                if (Fcb->ResidentData) {
                    RtlCopyMemory(Fcb->ResidentData, NtfsResidentValue(a), vlen);
                    Fcb->ResidentLength = vlen;
                }
            }
            Fcb->Header.AllocationSize.QuadPart =
                ((LONGLONG)vlen + Vcb->Geo.ClusterSize - 1) & ~((LONGLONG)Vcb->Geo.ClusterSize - 1);
        } else {
            Fcb->DataResident = FALSE;
            Fcb->Header.FileSize.QuadPart = (LONGLONG)f->Form.NonResident.DataSize;
            /* the real allocation (it can run past the rounded EOF) */
            Fcb->Header.AllocationSize.QuadPart = (LONGLONG)f->Form.NonResident.AllocatedSize;
            /* NTFS transparent compression (LZNT1): CompressionUnit != 0 and the
             * COMPRESSED attribute flag set. Record the unit size for the read
             * path (which decompresses per unit). */
            if ((f->Flags & ATTR_FLAG_COMPRESSED) && f->Form.NonResident.CompressionUnit) {
                Fcb->DataCompressed = TRUE;
                Fcb->CuClusters = 1u << f->Form.NonResident.CompressionUnit;
                Fcb->FileAttributes |= FILE_ATTRIBUTE_COMPRESSED;
            }
            Fcb->DataRuns = runs; Fcb->DataRunCount = rc; runs = NULL;
        }
        if (runs) ExFreePool(runs);
        Fcb->Header.ValidDataLength = Fcb->Header.FileSize;
    }
    ExFreePool(rec);
}

/*
 * Open a named $DATA stream (ADS) on an already-resolved file, populating the
 * CCB's stream descriptor so the read path can serve it. Returns
 * STATUS_OBJECT_NAME_NOT_FOUND if no $DATA with that name exists.
 */
static NTSTATUS
OpenNamedStream(PNTFS_VCB Vcb, PNTFS_FCB Fcb, PNTFS_CCB Ccb,
                PWCHAR Name, ULONG NameLen)
{
    PUCHAR rec;
    PMFT_RECORD m;
    PATTRIBUTE_RECORD a;
    NTSTATUS Status = STATUS_OBJECT_NAME_NOT_FOUND;

    rec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    if (rec == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    if (!NT_SUCCESS(NtfsReadMftRecord(Vcb, Fcb->MftRecord, rec))) {
        ExFreePool(rec); return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    m = (PMFT_RECORD)rec;
    a = NtfsFindAttribute(m, ATTR_DATA, Name, NameLen);
    if (a == NULL) { ExFreePool(rec); return Status; }

    Ccb->NamedStream = TRUE;
    { ULONG n = NameLen; if (n > 63) n = 63;
      RtlCopyMemory(Ccb->StreamName, Name, n * sizeof(WCHAR)); Ccb->StreamNameLen = n; }
    if (!a->NonResident) {
        ULONG vlen = NtfsResidentValueLength(a);
        Ccb->StreamResident = TRUE;
        Ccb->StreamSize = vlen;
        if (vlen) {
            Ccb->StreamResidentData = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, vlen, NTFS_TAG_BUF);
            if (Ccb->StreamResidentData) {
                RtlCopyMemory(Ccb->StreamResidentData, NtfsResidentValue(a), vlen);
                Ccb->StreamResidentLength = vlen;
            }
        }
        Status = STATUS_SUCCESS;
    } else {
        PNTFS_RUN runs; ULONG rc;
        Ccb->StreamResident = FALSE;
        Ccb->StreamSize = a->Form.NonResident.DataSize;
        if ((a->Flags & ATTR_FLAG_COMPRESSED) && a->Form.NonResident.CompressionUnit) {
            Ccb->StreamCompressed = TRUE;
            Ccb->StreamCuClusters = 1u << a->Form.NonResident.CompressionUnit;
        }
        runs = (PNTFS_RUN)ExAllocatePoolWithTag(NonPagedPool,
                   NTFS_MAX_IDX_RUNS * sizeof(NTFS_RUN), NTFS_TAG_RUN);
        if (runs && NtfsDecodeRuns(a, runs, NTFS_MAX_IDX_RUNS, &rc) && rc) {
            Ccb->StreamRuns = (PNTFS_RUN)ExAllocatePoolWithTag(NonPagedPool,
                                 rc * sizeof(NTFS_RUN), NTFS_TAG_RUN);
            if (Ccb->StreamRuns) {
                RtlCopyMemory(Ccb->StreamRuns, runs, rc * sizeof(NTFS_RUN));
                Ccb->StreamRunCount = rc;
            }
        }
        if (runs) ExFreePool(runs);
        Status = STATUS_SUCCESS;
    }
    ExFreePool(rec);
    return Status;
}

/* Tag bit 0x20000000 marks a "name surrogate" (symlink, mount point): the
 * object manager follows it. Other tags belong to a filter driver. */
#ifndef IO_REPARSE_TAG_RESERVED_ZERO
#define IO_REPARSE_TAG_RESERVED_ZERO 0
#endif
#define NTFS_REPARSE_NAME_SURROGATE(t)  (((t) & 0x20000000UL) != 0)

/*
 * A reparse point is being opened for traversal. Read its $REPARSE_POINT data;
 * for a name-surrogate tag hand it to the I/O manager (STATUS_REPARSE with the
 * data in the Irp's auxiliary buffer) so the link is resolved. For other tags
 * open the file normally (STATUS_SUCCESS) so it stays accessible.
 */
static NTSTATUS
NtfsReparseOpen(PNTFS_IRP_CONTEXT Ctx, PNTFS_FCB Fcb)
{
    PNTFS_VCB Vcb = Ctx->Vcb;
    PUCHAR rec;
    PMFT_RECORD m;
    PATTRIBUTE_RECORD rp;
    ULONG tag, vlen;
    PUCHAR aux;

    if (!NTFS_REPARSE_NAME_SURROGATE(Fcb->ReparseTag) && Fcb->ReparseTag != 0)
        return STATUS_SUCCESS;        /* a filter's tag: open it normally */

    rec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    if (!rec) return STATUS_INSUFFICIENT_RESOURCES;
    if (!NT_SUCCESS(NtfsReadMftRecord(Vcb, Fcb->MftRecord, rec))) { ExFreePool(rec); return STATUS_SUCCESS; }
    m = (PMFT_RECORD)rec;
    rp = NtfsFindAttribute(m, ATTR_REPARSE_POINT, NULL, 0);
    if (!rp || rp->NonResident) { ExFreePool(rec); return STATUS_SUCCESS; }
    vlen = NtfsResidentValueLength(rp);
    if (vlen < 8) { ExFreePool(rec); return STATUS_SUCCESS; }
    tag = *(PULONG)NtfsResidentValue(rp);
    if (!NTFS_REPARSE_NAME_SURROGATE(tag)) { ExFreePool(rec); return STATUS_SUCCESS; }

    aux = (PUCHAR)ExAllocatePoolWithTag(PagedPool, vlen, NTFS_TAG_BUF);
    if (!aux) { ExFreePool(rec); return STATUS_INSUFFICIENT_RESOURCES; }
    RtlCopyMemory(aux, NtfsResidentValue(rp), vlen);
    ExFreePool(rec);

    /* the I/O manager frees the auxiliary buffer and reparses the path */
    Ctx->Irp->Tail.Overlay.AuxiliaryBuffer = (PCHAR)aux;
    Ctx->Irp->IoStatus.Information = tag;
    return STATUS_REPARSE;
}

NTSTATUS
NtfsCommonCreate(PNTFS_IRP_CONTEXT Ctx)
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PNTFS_VCB Vcb = Ctx->Vcb;
    PNTFS_FCB Fcb;
    ULONGLONG rec;
    PWCHAR p, comp = NULL;
    ULONG remain, clen = 0;
    ULONG opts = IrpSp->Parameters.Create.Options;
    ULONGLONG startRec = MFT_RN_ROOT;
    ULONG disp = (IrpSp->Parameters.Create.Options >> 24) & 0xFF;
    PWCHAR StreamName = NULL;          /* named $DATA stream (ADS), if any */
    ULONG  StreamLen = 0;
    ULONG  NameChars;
    ULONG  CreateInfo = FILE_OPENED;    /* IoStatus.Information to report */

    /* opening the FS device itself (not a volume) */
    if (Vcb == NULL) {
        FileObject->FsContext = NULL;
        Ctx->Irp->IoStatus.Information = FILE_OPENED;
        return STATUS_SUCCESS;
    }

    /* no creation/overwrite while read-only */
    if (disp == FILE_CREATE || disp == FILE_SUPERSEDE || disp == FILE_OVERWRITE ||
        disp == FILE_OVERWRITE_IF) {
        if (!NtfsData.WriteEnabled) {
            return STATUS_MEDIA_WRITE_PROTECTED;
        }
    }

    /* Split off a named stream: "path\file:stream[:$DATA]". A ':' only appears
     * in the final component. Everything after the first ':' is the stream name
     * (up to an optional ":type" suffix). An empty stream name ("::$DATA") means
     * the unnamed (main) stream, so it is ignored. */
    NameChars = FileObject->FileName.Length / sizeof(WCHAR);
    {
        ULONG i;
        PWCHAR nb = FileObject->FileName.Buffer;
        for (i = 0; i < NameChars; i++) {
            if (nb[i] == L':') {
                ULONG s = i + 1, e = s;
                while (e < NameChars && nb[e] != L':') e++;
                if (e > s) { StreamName = nb + s; StreamLen = e - s; }
                NameChars = i;            /* path is everything before the ':' */
                break;
            }
        }
    }

    if (Vcb->VcbState & VCB_STATE_DISMOUNTED) return STATUS_VOLUME_DISMOUNTED;

    /* volume (DASD) open: "\\.\X:" - empty name, no related file object */
    if (NameChars == 0 && StreamName == NULL && FileObject->RelatedFileObject == NULL) {
        if (disp != FILE_OPEN && disp != FILE_OPEN_IF) return STATUS_ACCESS_DENIED;
        if (opts & FILE_DIRECTORY_FILE) return STATUS_NOT_A_DIRECTORY;
        FileObject->FsContext = Vcb;
        FileObject->FsContext2 = NULL;
        FileObject->Vpb = Vcb->Vpb;
        Vcb->OpenCount++;
        Ctx->Irp->IoStatus.Information = FILE_OPENED;
        return STATUS_SUCCESS;
    }

    /* relative open: the walk starts at the related (directory) file object */
    if (FileObject->RelatedFileObject != NULL) {
        PNTFS_FCB rf = (PNTFS_FCB)FileObject->RelatedFileObject->FsContext;
        if (rf != NULL && rf->NodeTypeCode == NTFS_NTC_FCB) {
            if (!(rf->FcbState & FCB_STATE_DIRECTORY) && NameChars) return STATUS_OBJECT_PATH_NOT_FOUND;
            startRec = rf->MftRecord;
        }
        if (NameChars && FileObject->FileName.Buffer[0] == L'\\') return STATUS_OBJECT_NAME_INVALID;
    }

    /* SL_OPEN_TARGET_DIRECTORY (rename/move target): open the PARENT of the
     * final component, report whether that component exists, and leave just
     * the final component in FileObject->FileName for the set-rename call. */
    if (IrpSp->Flags & SL_OPEN_TARGET_DIRECTORY) {
        PWCHAR nb = FileObject->FileName.Buffer;
        ULONG n = NameChars, ls, i;
        ULONGLONG dir = startRec, r;
        while (n > 0 && nb[n - 1] == L'\\') n--;              /* trailing '\' */
        if (n == 0) return STATUS_OBJECT_NAME_INVALID;
        ls = n; while (ls > 0 && nb[ls - 1] != L'\\') ls--;   /* final component */
        /* walk the parent part */
        p = nb; remain = ls;
        while (remain > 0) {
            while (remain > 0 && *p == L'\\') { p++; remain--; }
            if (remain == 0) break;
            comp = p; clen = 0;
            while (remain > 0 && *p != L'\\') { p++; remain--; clen++; }
            dir = NtfsLookupInDir(Vcb, dir, comp, clen);
            if (dir == (ULONGLONG)-1) return STATUS_OBJECT_PATH_NOT_FOUND;
        }
        r = NtfsLookupInDir(Vcb, dir, nb + ls, n - ls);
        CreateInfo = (r == (ULONGLONG)-1) ? FILE_DOES_NOT_EXIST : FILE_EXISTS;
        for (i = 0; i < n - ls; i++) nb[i] = nb[ls + i];
        FileObject->FileName.Length = (USHORT)((n - ls) * sizeof(WCHAR));
        if (dir == MFT_RN_ROOT) { Fcb = Vcb->RootFcb; Fcb->RefCount++; goto opened; }
        Fcb = NtfsCreateFcb(Vcb, dir);
        if (Fcb == NULL) return STATUS_INSUFFICIENT_RESOURCES;
        if (!(Fcb->FcbState & FCB_STATE_INFO_LOADED)) {
            LoadFcbInfo(Vcb, Fcb);
            Fcb->FcbState |= FCB_STATE_INFO_LOADED;
        }
        if (!(Fcb->FcbState & FCB_STATE_DIRECTORY)) { Fcb->RefCount--; return STATUS_OBJECT_PATH_NOT_FOUND; }
        comp = NULL; clen = 0;
        goto opened;
    }

    /* volume / root open (or reopen of the related directory itself) */
    if (NameChars == 0 ||
        (NameChars == 1 && FileObject->FileName.Buffer[0] == L'\\')) {
        if (startRec != MFT_RN_ROOT && NameChars == 0) { rec = startRec; goto have_rec; }
        if (opts & FILE_NON_DIRECTORY_FILE) return STATUS_FILE_IS_A_DIRECTORY;
        if (disp == FILE_CREATE) return STATUS_OBJECT_NAME_COLLISION;
        Fcb = Vcb->RootFcb;
        Fcb->RefCount++;
        goto opened;
    }

    /* walk the path from the root, tracking the parent of the current component
     * so a not-found leaf with a create disposition can be created in place */
    rec = startRec;
    p = FileObject->FileName.Buffer;
    remain = NameChars;
    {
        ULONGLONG parentRec = startRec;
        while (remain > 0) {
            ULONGLONG next;
            while (remain > 0 && *p == L'\\') { p++; remain--; }
            if (remain == 0) break;
            comp = p; clen = 0;
            while (remain > 0 && *p != L'\\') { p++; remain--; clen++; }
            /* is this the last component? (only trailing backslashes remain) */
            { PWCHAR q = p; ULONG r = remain; while (r && *q == L'\\') { q++; r--; } (void)q;
              parentRec = rec;
              /* wildcards are not legal in a name: say so (not "not found"),
                 as NT's own file systems do - NT 3.1's COPY tells a pattern
                 from a missing file by this status */
              { ULONG w; for (w = 0; w < clen; w++)
                  if (comp[w] == L'*' || comp[w] == L'?' || comp[w] == L'<' || comp[w] == L'>' || comp[w] == L'"')
                      return STATUS_OBJECT_NAME_INVALID; }
              next = NtfsLookupInDir(Vcb, rec, comp, clen);
              if (next == (ULONGLONG)-1) {
                  BOOLEAN isLast = (r == 0);
                  BOOLEAN creating = (disp == FILE_CREATE || disp == FILE_OPEN_IF ||
                                      disp == FILE_OVERWRITE_IF || disp == FILE_SUPERSEDE);
                  if (isLast && creating && NtfsData.WriteEnabled) {
                      /* create a new empty file in parentRec */
                      PUCHAR pr = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool,
                                      Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
                      USHORT pseq = 5;
                      NTSTATUS cst;
                      ULONGLONG newRec = 0;
                      if (pr && NT_SUCCESS(NtfsReadMftRecord(Vcb, parentRec, pr)))
                          pseq = ((PMFT_RECORD)pr)->SequenceNumber;
                      if (pr) ExFreePool(pr);
                      if ((opts & FILE_DIRECTORY_FILE) && (opts & FILE_NON_DIRECTORY_FILE))
                          return STATUS_INVALID_PARAMETER;
                      cst = NtfsCreateInDir(Vcb, parentRec, pseq, comp, clen, "", 0,
                                            (opts & FILE_DIRECTORY_FILE) != 0, &newRec);
                      if (!NT_SUCCESS(cst)) return cst;
                      rec = newRec;
                      CreateInfo = FILE_CREATED;
                      goto have_rec;
                  }
                  return isLast ? STATUS_OBJECT_NAME_NOT_FOUND : STATUS_OBJECT_PATH_NOT_FOUND;
              }
              /* exists: FILE_CREATE must fail with a collision */
              if (r == 0 && disp == FILE_CREATE) return STATUS_OBJECT_NAME_COLLISION;
            }
            rec = next;
        }
    }

have_rec:
    Fcb = NtfsCreateFcb(Vcb, rec);
    if (Fcb == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    if (!(Fcb->FcbState & FCB_STATE_INFO_LOADED)) {
        LoadFcbInfo(Vcb, Fcb);
        Fcb->FcbState |= FCB_STATE_INFO_LOADED;
    }

    if (IrpSp->Flags & SL_OPEN_PAGING_FILE) Fcb->FcbState |= FCB_STATE_PAGING_FILE;

    /* reparse point: unless the caller asked to open the link itself
     * (FILE_OPEN_REPARSE_POINT) or to delete it, hand the reparse data back to
     * the I/O manager, which resolves the symlink / mount point and re-parses
     * the path. Non-Microsoft tags go to whatever filter owns them. */
    if ((Fcb->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        !(opts & FILE_OPEN_REPARSE_POINT) && CreateInfo != FILE_CREATED &&
        !(StreamName && StreamLen)) {
        NTSTATUS rst = NtfsReparseOpen(Ctx, Fcb);
        if (rst == STATUS_REPARSE || !NT_SUCCESS(rst)) {
            Fcb->RefCount--;
            return rst;
        }
        /* rst == STATUS_SUCCESS: a tag we do not surrogate (open it normally) */
    }

    /* directory / non-directory requirements of the open */
    if ((opts & FILE_DIRECTORY_FILE) && !(Fcb->FcbState & FCB_STATE_DIRECTORY)) {
        Fcb->RefCount--; return STATUS_NOT_A_DIRECTORY;
    }
    if ((opts & FILE_NON_DIRECTORY_FILE) && (Fcb->FcbState & FCB_STATE_DIRECTORY) &&
        !(StreamName && StreamLen)) {
        Fcb->RefCount--; return STATUS_FILE_IS_A_DIRECTORY;
    }
    if ((Fcb->FcbState & FCB_STATE_DIRECTORY) && CreateInfo != FILE_CREATED &&
        (disp == FILE_OVERWRITE || disp == FILE_OVERWRITE_IF || disp == FILE_SUPERSEDE)) {
        Fcb->RefCount--; return STATUS_FILE_IS_A_DIRECTORY;
    }

    /* existing file opened with an overwrite disposition: truncate to zero */
    if (CreateInfo != FILE_CREATED && NtfsData.WriteEnabled && !(StreamName && StreamLen) &&
        (disp == FILE_OVERWRITE || disp == FILE_OVERWRITE_IF || disp == FILE_SUPERSEDE)) {
        NTSTATUS tst;
        if (Fcb->FcbState & FCB_STATE_DIRECTORY) { Fcb->RefCount--; return STATUS_FILE_IS_A_DIRECTORY; }
        tst = NtfsTruncateFile(Vcb, Fcb);
        if (!NT_SUCCESS(tst)) { Fcb->RefCount--; return tst; }
        CreateInfo = (disp == FILE_SUPERSEDE) ? FILE_SUPERSEDED : FILE_OVERWRITTEN;
    }

    /* remember the leaf name for FileNameInformation queries */
    if (clen > 0) {
        ULONG nb = clen; if (nb > 255) nb = 255;
        RtlCopyMemory(Fcb->FileName, comp, nb * sizeof(WCHAR));
        Fcb->FileNameLength = (USHORT)(nb * sizeof(WCHAR));
    }

opened:
    FileObject->FsContext = Fcb;
    FileObject->FsContext2 = NULL;

    /* named stream (ADS): build a CCB holding the stream descriptor */
    if (StreamName && StreamLen > 0) {
        PNTFS_CCB Ccb = (PNTFS_CCB)ExAllocatePoolWithTag(NonPagedPool,
                            sizeof(NTFS_CCB), NTFS_TAG_CCB);
        NTSTATUS sst;
        if (Ccb == NULL) { Fcb->RefCount--; return STATUS_INSUFFICIENT_RESOURCES; }
        RtlZeroMemory(Ccb, sizeof(NTFS_CCB));
        Ccb->NodeTypeCode = NTFS_NTC_CCB;
        sst = OpenNamedStream(Vcb, Fcb, Ccb, StreamName, StreamLen);
        if (!NT_SUCCESS(sst)) {
            ExFreePool(Ccb);
            Fcb->RefCount--;         /* undo NtfsCreateFcb / root ref */
            return sst;              /* no such stream */
        }
        FileObject->FsContext2 = Ccb;
    }

    FileObject->SectionObjectPointer = &Fcb->SectionObjectPointers;
    FileObject->Vpb = Vcb->Vpb;
    Fcb->UncleanCount++;
    Vcb->OpenCount++;
    Ctx->Irp->IoStatus.Information = CreateInfo;
    return STATUS_SUCCESS;
}
