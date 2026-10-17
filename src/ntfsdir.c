/*
 * NTFS3NT - directory control: query-directory enumeration over the $I30 index
 * (root node + INDX blocks), and change-notify requests held pending (never
 * completed on the spot) so the shell does not spin - the fix proven in FATNT.
 */

#define NTFS_KERNEL
#include "ntfs.h"

static const WCHAR Idx30[4] = { '$', 'I', '3', '0' };

/*
 * Directory snapshot. The first query (or a restart) walks the $I30 B-tree
 * in order - each entry's subnode before the entry itself - and copies every
 * key into a paged buffer, so the listing comes out sorted like Windows NTFS
 * and each later query is a sequential read of memory instead of re-reading
 * the whole index for every entry. Record layout:
 *   USHORT RecLen; USHORT Pad; ULONGLONG FileRef; FILE_NAME_ATTR + name
 */
#define SNAP_HDR 12

typedef struct _NTFS_SNAPB {
    PUCHAR Buf;
    ULONG  Len, Cap;
} NTFS_SNAPB;

static BOOLEAN
SnapAdd(NTFS_SNAPB *s, ULONGLONG ref, PFILE_NAME_ATTR fn)
{
    ULONG kl = sizeof(FILE_NAME_ATTR) + fn->NameLength * 2;
    ULONG rl = (SNAP_HDR + kl + 3) & ~3u;
    if (s->Len + rl > s->Cap) {
        ULONG nc = s->Cap ? s->Cap * 2 : 8192;
        PUCHAR nb;
        while (nc < s->Len + rl) nc *= 2;
        nb = (PUCHAR)ExAllocatePoolWithTag(PagedPool, nc, NTFS_TAG_BUF);
        if (nb == NULL) return FALSE;
        if (s->Buf) { RtlCopyMemory(nb, s->Buf, s->Len); ExFreePool(s->Buf); }
        s->Buf = nb; s->Cap = nc;
    }
    *(PUSHORT)(s->Buf + s->Len) = (USHORT)rl;
    *(PUSHORT)(s->Buf + s->Len + 2) = 0;
    RtlCopyMemory(s->Buf + s->Len + 4, &ref, 8);
    RtlCopyMemory(s->Buf + s->Len + SNAP_HDR, fn, kl);
    s->Len += rl;
    return TRUE;
}

static VOID
SnapWalk(PNTFS_VCB Vcb, PINDEX_HEADER h, PNTFS_RUN runs, ULONG rc, ULONG depth,
         BOOLEAN isRoot, NTFS_SNAPB *s)
{
    ULONG ibs = Vcb->Geo.IndexBlockSize, cs = Vcb->Geo.ClusterSize;
    ULONG vcnBytes = (ibs >= cs) ? cs : 512;
    PINDEX_ENTRY e;
    for (e = NtfsFirstIndexEntry(h); e; e = NtfsNextIndexEntry(e)) {
        if ((e->Flags & INDEX_ENTRY_HAS_SUBNODE) && runs && depth < 8) {
            PUCHAR b = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, ibs, NTFS_TAG_BUF);
            if (b) {
                if (NT_SUCCESS(NtfsReadAttributeData(Vcb, runs, rc,
                        NtfsIndexSubnodeVcn(e) * vcnBytes, ibs, b)) &&
                    ((PINDEX_BLOCK)b)->Rhdr.Type == NTFS_INDX_SIGNATURE &&
                    NtfsApplyFixup(b, ibs, Vcb->Geo.BytesPerSector))
                    SnapWalk(Vcb, &((PINDEX_BLOCK)b)->Header, runs, rc, depth + 1, isRoot, s);
                ExFreePool(b);
            }
        }
        if (e->Flags & INDEX_ENTRY_END) break;
        if (e->KeyLength) {
            PFILE_NAME_ATTR fn = (PFILE_NAME_ATTR)((PUCHAR)e + sizeof(INDEX_ENTRY));
            /* the root lists the NTFS system files ($MFT...): hide them */
            if (!(isRoot && NTFS_MREF_RECORD(e->FileReference) < 16))
                if (!SnapAdd(s, e->FileReference, fn)) return;
        }
        if (e->Length == 0) break;
    }
}

static VOID
BuildSnapshot(PNTFS_VCB Vcb, PNTFS_FCB Fcb, PNTFS_CCB Ccb)
{
    PUCHAR rec;
    PMFT_RECORD m;
    PATTRIBUTE_RECORD ir, ia;
    PNTFS_RUN runs = NULL;
    ULONG rc = 0;
    NTFS_SNAPB s = { NULL, 0, 0 };

    if (Ccb->Snap) { ExFreePool(Ccb->Snap); Ccb->Snap = NULL; }
    Ccb->SnapLen = 0; Ccb->SnapOff = 0; Ccb->SnapIdx = 0;
    rec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    if (rec == NULL) return;
    if (NT_SUCCESS(NtfsReadMftRecord(Vcb, Fcb->MftRecord, rec))) {
        m = (PMFT_RECORD)rec;
        ir = NtfsFindAttribute(m, ATTR_INDEX_ROOT, Idx30, 4);
        ia = NtfsFindAttribute(m, ATTR_INDEX_ALLOCATION, Idx30, 4);
        if (ia && ia->NonResident) {
            runs = (PNTFS_RUN)ExAllocatePoolWithTag(NonPagedPool,
                       NTFS_MAX_IDX_RUNS * sizeof(NTFS_RUN), NTFS_TAG_RUN);
            if (runs && !NtfsDecodeRuns(ia, runs, NTFS_MAX_IDX_RUNS, &rc)) rc = 0;
        }
        if (ir)
            SnapWalk(Vcb, &((PINDEX_ROOT)NtfsResidentValue(ir))->Header,
                     rc ? runs : NULL, rc, 0, (BOOLEAN)((Fcb->FcbState & FCB_STATE_ROOT) != 0), &s);
    }
    if (runs) ExFreePool(runs);
    ExFreePool(rec);
    Ccb->Snap = s.Buf;
    Ccb->SnapLen = s.Len;
}

/* DOS (8.3) alias of file `ref` in the snapshot, for FileBothDirectoryInformation */
static ULONG
SnapShortName(PNTFS_CCB Ccb, ULONGLONG ref, PWCHAR out)
{
    ULONG off = 0;
    while (off < Ccb->SnapLen) {
        USHORT rl = *(PUSHORT)(Ccb->Snap + off);
        PFILE_NAME_ATTR fn = (PFILE_NAME_ATTR)(Ccb->Snap + off + SNAP_HDR);
        ULONGLONG r; RtlCopyMemory(&r, Ccb->Snap + off + 4, 8);
        if (fn->NameType == NTFS_FNAME_DOS && r == ref && fn->NameLength <= 12) {
            RtlCopyMemory(out, (PUCHAR)fn + sizeof(FILE_NAME_ATTR), fn->NameLength * 2);
            return fn->NameLength;
        }
        if (rl == 0) break;
        off += rl;
    }
    return 0;
}

/* One directory entry in a class-neutral form. */
typedef struct _NTFS_DIRENT {
    PWCHAR     Name;
    ULONG      NameLen;
    ULONGLONG  Ref;
    BOOLEAN    IsDir;
    ULONGLONG  Size;
    ULONGLONG  Alloc;
    ULONG      Attributes;
    ULONGLONG  CreationTime;
    ULONGLONG  LastAccessTime;
    ULONGLONG  LastWriteTime;
    ULONGLONG  ChangeTime;
} NTFS_DIRENT;

static const WCHAR DotNames[2] = { L'.', L'.' };

/*
 * Entry number Ccb->IndexOffset: for a non-root directory 0 = ".", 1 = "..",
 * then the snapshot in index order (DOS-only aliases skipped). FALSE at end.
 */
static BOOLEAN
NextDirEntry(PNTFS_FCB Fcb, PNTFS_CCB Ccb, NTFS_DIRENT *Out)
{
    ULONG k = Ccb->IndexOffset;
    BOOLEAN isRoot = (BOOLEAN)((Fcb->FcbState & FCB_STATE_ROOT) != 0);
    if (!isRoot && k < 2) {
        RtlZeroMemory(Out, sizeof(*Out));
        Out->Name = (PWCHAR)DotNames; Out->NameLen = k + 1;
        Out->IsDir = TRUE;
        Out->Attributes = FILE_ATTRIBUTE_DIRECTORY;
        Out->CreationTime = Fcb->CreationTime; Out->LastAccessTime = Fcb->LastAccessTime;
        Out->LastWriteTime = Fcb->LastWriteTime; Out->ChangeTime = Fcb->ChangeTime;
        return TRUE;
    }
    if (!isRoot) k -= 2;
    /* resync the cursor if the caller moved backwards */
    if (k < Ccb->SnapIdx) { Ccb->SnapIdx = 0; Ccb->SnapOff = 0; }
    while (Ccb->SnapOff < Ccb->SnapLen) {
        USHORT rl = *(PUSHORT)(Ccb->Snap + Ccb->SnapOff);
        PFILE_NAME_ATTR fn = (PFILE_NAME_ATTR)(Ccb->Snap + Ccb->SnapOff + SNAP_HDR);
        if (rl == 0) break;
        if (fn->NameType == NTFS_FNAME_DOS) { Ccb->SnapOff += rl; continue; }
        if (Ccb->SnapIdx == k) {
            Out->Name = (PWCHAR)((PUCHAR)fn + sizeof(FILE_NAME_ATTR));
            Out->NameLen = fn->NameLength;
            RtlCopyMemory(&Out->Ref, Ccb->Snap + Ccb->SnapOff + 4, 8);
            Out->IsDir = (BOOLEAN)((fn->FileAttributes & NTFS_FILE_NAME_INDEX_PRESENT) != 0);
            Out->Size = fn->DataSize; Out->Alloc = fn->AllocatedSize;
            Out->Attributes = fn->FileAttributes & 0xFFFF;
            Out->CreationTime = fn->CreationTime; Out->LastAccessTime = fn->LastAccessTime;
            Out->LastWriteTime = fn->ChangeTime; Out->ChangeTime = fn->MftChangeTime;
            return TRUE;
        }
        Ccb->SnapIdx++;
        Ccb->SnapOff += rl;
    }
    return FALSE;
}

/* advance past the entry NextDirEntry just returned */
static VOID
SkipDirEntry(PNTFS_FCB Fcb, PNTFS_CCB Ccb)
{
    BOOLEAN isRoot = (BOOLEAN)((Fcb->FcbState & FCB_STATE_ROOT) != 0);
    if (isRoot || Ccb->IndexOffset >= 2) {
        if (Ccb->SnapOff < Ccb->SnapLen) {
            Ccb->SnapOff += *(PUSHORT)(Ccb->Snap + Ccb->SnapOff);
            Ccb->SnapIdx++;
        }
    }
    Ccb->IndexOffset++;
}

static BOOLEAN
PatternMatch(PNTFS_CCB Ccb, PWCHAR Name, ULONG NameLen)
{
    UNICODE_STRING n;
    if (Ccb->MatchAll) return TRUE;
    n.Buffer = Name; n.Length = n.MaximumLength = (USHORT)(NameLen * sizeof(WCHAR));
    if (Ccb->HasWild)
        return FsRtlIsNameInExpression(&Ccb->Pattern, &n, TRUE, NULL);
    return (BOOLEAN)(Ccb->Pattern.Length == n.Length &&
                     NtfsCollateName(Ccb->Pattern.Buffer, NameLen, Name, NameLen) == 0);
}

static NTSTATUS
NtfsQueryDirectory(PNTFS_IRP_CONTEXT Ctx)
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PNTFS_FCB Fcb = (PNTFS_FCB)FileObject->FsContext;
    PNTFS_CCB Ccb = (PNTFS_CCB)FileObject->FsContext2;
    PNTFS_VCB Vcb = Ctx->Vcb;
    FILE_INFORMATION_CLASS Class = IrpSp->Parameters.QueryDirectory.FileInformationClass;
    PUCHAR Buffer = (PUCHAR)(Ctx->Irp->MdlAddress ?
        MmGetSystemAddressForMdl(Ctx->Irp->MdlAddress) : Ctx->Irp->UserBuffer);
    ULONG Length = IrpSp->Parameters.QueryDirectory.Length;
    ULONG produced = 0;
    PULONG prevNext = NULL;
    NTFS_DIRENT de;
    BOOLEAN single = (BOOLEAN)((IrpSp->Flags & SL_RETURN_SINGLE_ENTRY) != 0);
    BOOLEAN first = FALSE;

    if (Fcb == NULL || !(Fcb->FcbState & FCB_STATE_DIRECTORY)) return STATUS_INVALID_PARAMETER;
    if (Buffer == NULL) return STATUS_INVALID_USER_BUFFER;

    if (Ccb == NULL) {
        Ccb = (PNTFS_CCB)ExAllocatePoolWithTag(NonPagedPool, sizeof(NTFS_CCB), NTFS_TAG_CCB);
        if (Ccb == NULL) return STATUS_INSUFFICIENT_RESOURCES;
        RtlZeroMemory(Ccb, sizeof(NTFS_CCB));
        Ccb->NodeTypeCode = NTFS_NTC_CCB;
        FileObject->FsContext2 = Ccb;
    }

    /* Capture the search expression on the first call (or a restart that carries
     * a new one). Missing/empty/"*" matches everything; otherwise it is matched
     * with FsRtlIsNameInExpression (DOS wildcards < > " included, as Win32's
     * FindFirstFile translates "*.txt" etc.) or as an exact name. */
    if (!Ccb->PatternSet ||
        ((IrpSp->Flags & SL_RESTART_SCAN) && IrpSp->Parameters.QueryDirectory.FileName)) {
        PUNICODE_STRING p = IrpSp->Parameters.QueryDirectory.FileName;
        first = TRUE;
        Ccb->PatternSet = TRUE;
        Ccb->MatchAll = TRUE;
        Ccb->HasWild = FALSE;
        Ccb->Pattern.Buffer = Ccb->PatternBuf;
        Ccb->Pattern.Length = 0;
        Ccb->Pattern.MaximumLength = sizeof(Ccb->PatternBuf);
        if (p && p->Length && p->Buffer && p->Length <= sizeof(Ccb->PatternBuf)) {
            ULONG n = p->Length / sizeof(WCHAR);
            if (!(n == 1 && p->Buffer[0] == L'*') &&
                !(n == 3 && p->Buffer[0] == L'*' && p->Buffer[1] == L'.' && p->Buffer[2] == L'*')) {
                if (NT_SUCCESS(RtlUpcaseUnicodeString(&Ccb->Pattern, p, FALSE))) {
                    Ccb->MatchAll = FALSE;
                    Ccb->HasWild = FsRtlDoesNameContainWildCards(&Ccb->Pattern);
                }
            }
        }
    }
    if (first || (IrpSp->Flags & SL_RESTART_SCAN) || Ccb->Snap == NULL) {
        if (first || (IrpSp->Flags & SL_RESTART_SCAN)) Ccb->IndexOffset = 0;
        BuildSnapshot(Vcb, Fcb, Ccb);
    }

    for (;;) {
        ULONG need, nameBytes, base;
        if (!NextDirEntry(Fcb, Ccb, &de)) break;
        if (!PatternMatch(Ccb, de.Name, de.NameLen)) { SkipDirEntry(Fcb, Ccb); continue; }
        nameBytes = de.NameLen * sizeof(WCHAR);

        switch (Class) {
        case FileDirectoryInformation: base = FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileName); break;
        case FileFullDirectoryInformation: base = FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, FileName); break;
        case FileNamesInformation: base = FIELD_OFFSET(FILE_NAMES_INFORMATION, FileName); break;
        case FileBothDirectoryInformation: base = FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileName); break;
        default: return STATUS_INVALID_INFO_CLASS;
        }
        need = (base + nameBytes + 7) & ~7u;

        if (produced + need > Length) {
            if (produced == 0) {
                /* not even one entry fits: return what does, as BUFFER_OVERFLOW */
                return STATUS_BUFFER_OVERFLOW;
            }
            break;
        }

        RtlZeroMemory(Buffer + produced, base);
        if (Class == FileNamesInformation) {
            PFILE_NAMES_INFORMATION e = (PFILE_NAMES_INFORMATION)(Buffer + produced);
            e->FileIndex = Ccb->IndexOffset;
            e->FileNameLength = nameBytes;
            RtlCopyMemory(e->FileName, de.Name, nameBytes);
            prevNext = &e->NextEntryOffset;
        } else {
            /* BOTH / DIRECTORY / FULL share the leading layout we fill */
            PFILE_BOTH_DIR_INFORMATION e = (PFILE_BOTH_DIR_INFORMATION)(Buffer + produced);
            ULONG attr = (de.Attributes & NTFS_DOS_ATTR_MASK) |
                         (de.IsDir ? FILE_ATTRIBUTE_DIRECTORY : 0);
            if (attr == 0) attr = FILE_ATTRIBUTE_NORMAL;
            e->FileIndex = Ccb->IndexOffset;
            e->CreationTime.QuadPart = (LONGLONG)de.CreationTime;
            e->LastAccessTime.QuadPart = (LONGLONG)de.LastAccessTime;
            e->LastWriteTime.QuadPart = (LONGLONG)de.LastWriteTime;
            e->ChangeTime.QuadPart = (LONGLONG)de.ChangeTime;
            e->EndOfFile.QuadPart = de.IsDir ? 0 : (LONGLONG)de.Size;
            e->AllocationSize.QuadPart = de.IsDir ? 0 : (LONGLONG)de.Alloc;
            e->FileAttributes = attr;
            e->FileNameLength = nameBytes;
            if (Class == FileBothDirectoryInformation && de.Ref)
                e->ShortNameLength = (CCHAR)(SnapShortName(Ccb, de.Ref, e->ShortName) * sizeof(WCHAR));
            RtlCopyMemory((PUCHAR)e + base, de.Name, nameBytes);
            prevNext = &e->NextEntryOffset;
        }
        *prevNext = need;
        produced += need;
        SkipDirEntry(Fcb, Ccb);

        if (single) break;
    }

    if (prevNext) *prevNext = 0;     /* last entry */
    if (produced == 0) {
        return first ? STATUS_NO_SUCH_FILE : STATUS_NO_MORE_FILES;
    }
    Ctx->Irp->IoStatus.Information = produced;
    return STATUS_SUCCESS;
}

/* change-notify held pending (no spin), completed at cleanup; see FATNT. */
static VOID NTAPI
NtfsCancelNotify(PDEVICE_OBJECT Dev, PIRP Irp)
{
    (void)Dev;
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
    IoReleaseCancelSpinLock(Irp->CancelIrql);
    Irp->IoStatus.Status = STATUS_CANCELLED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

NTSTATUS
NtfsCommonDirectoryControl(PNTFS_IRP_CONTEXT Ctx)
{
    if (Ctx->IrpSp->MinorFunction == IRP_MN_QUERY_DIRECTORY) {
        return NtfsQueryDirectory(Ctx);
    }
    if (Ctx->IrpSp->MinorFunction == IRP_MN_NOTIFY_CHANGE_DIRECTORY) {
        KIRQL irql;
        IoMarkIrpPending(Ctx->Irp);
        IoAcquireCancelSpinLock(&irql);
        if (Ctx->Irp->Cancel) {
            IoReleaseCancelSpinLock(irql);
            Ctx->Irp->IoStatus.Status = STATUS_CANCELLED;
            Ctx->Irp->IoStatus.Information = 0;
            IoCompleteRequest(Ctx->Irp, IO_NO_INCREMENT);
        } else {
            /* Hold it pending. Self-link the list entry so the cancel routine's
             * RemoveEntryList is always safe; the I/O manager cancels the held
             * notify when the directory handle closes. No live notification, but
             * no spin/freeze (the FATNT fix). */
            PLIST_ENTRY le = &Ctx->Irp->Tail.Overlay.ListEntry;
            le->Flink = le->Blink = le;
            Ctx->Irp->CancelRoutine = NtfsCancelNotify;
            IoReleaseCancelSpinLock(irql);
        }
        Ctx->Flags |= NTFS_CTX_NO_COMPLETE;
        return STATUS_PENDING;
    }
    return STATUS_INVALID_DEVICE_REQUEST;
}
