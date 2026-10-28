/*
 * NTFS3NT - $LogFile journaling.
 *
 * Model: an undo (rollback) journal. Every metadata write in this driver is
 * written through to the disk at once, so a committed operation is already
 * durable; the only thing a crash can leave behind is a half-finished
 * operation. So each top-level operation runs between NtfsJnlBegin and
 * NtfsJnlCommit/Abort, and before any metadata record or block is overwritten
 * the write primitive hands us the OLD bytes (NtfsJnlRecord / NtfsJnlRange);
 * we log them (write-ahead: the undo image reaches the disk before the
 * metadata it protects) and, at commit, write a commit marker. A crash before
 * the commit marker leaves an uncommitted operation whose undo images we
 * replay in reverse at the next mount, restoring the volume to the state it
 * had before that operation began. The $Volume dirty bit is set while an
 * operation is in flight, so if Windows mounts first after our crash its
 * autochk/chkdsk reconciles the volume too.
 *
 * The $LogFile keeps a pair of valid, CLEAN LFS restart pages at all times
 * (so Windows and ntfsfix see a well-formed, clean log and never try to
 * replay our private records); our undo records live in the record-page area
 * in our own framing and are blanked at a clean unmount.
 *
 * Everything is behind Vcb->Journal: NULL means journaling is off (read-only
 * mount or no usable $LogFile) and every hook is a no-op, so the driver writes
 * through exactly as before.
 */

#define NTFS_KERNEL
#include "ntfs.h"
#include "ntelog.h"

/* NtfsJnlRange Kind values (NTFS_JNL_*) now live in ntfs.h */

/* our record-page framing */
#define NJ_HDR_MAGIC    0x474C4A4EUL   /* 'NJLG' */
#define NJ_REC_MAGIC    0x3152414EUL   /* 'NJR1' */
#define NJ_END_MAGIC    0x3145AE4EUL   /* 'N\xAE E1' - any distinctive tail */
#define NJ_STATE_DIRTY  1

#define NJ_PAGE         0x4000UL       /* our record page: 16 KB (holds a 4 KB INDX block + overhead) */

#pragma pack(push, 1)
typedef struct _NJ_HDR {
    ULONG       Magic;                  /* NJ_HDR_MAGIC */
    ULONG       Version;
    ULONG       State;                  /* NJ_STATE_DIRTY while operations may be in flight */
    ULONG       PageSize;               /* NJ_PAGE */
    ULONGLONG   Lsn;
    ULONG       NextTx;
    ULONG       Sum;
} NJ_HDR, *PNJ_HDR;

typedef struct _NJ_REC {
    ULONG       Magic;                  /* NJ_REC_MAGIC */
    ULONG       Type;                   /* 0 = undo image, 1 = commit, 2 = undo-create (dealloc) */
    ULONG       TxId;
    ULONG       Kind;                   /* NTFS_JNL_* */
    ULONGLONG   Target;
    ULONGLONG   Vbo;
    ULONG       Length;                 /* bytes of undo image that follow */
    ULONG       RecLen;                 /* total bytes incl header, 8-aligned */
    ULONGLONG   Lsn;
    /* UCHAR Data[Length]; ULONG End; */
} NJ_REC, *PNJ_REC;
#pragma pack(pop)

#define NJ_REC_TYPE_UNDO    0
#define NJ_REC_TYPE_COMMIT  1
#define NJ_REC_TYPE_CREATE  2

typedef struct _NTFS_JOURNAL {
    PNTFS_VCB   Vcb;
    PNTFS_RUN   Runs;                   /* $LogFile $DATA runs */
    ULONG       RunCount;
    ULONGLONG   LogBytes;               /* usable $LogFile size */
    ULONG       SysPage;                /* restart-page size (4096) */
    ULONGLONG   RecBase;                /* first byte of our record area */
    ULONGLONG   RecEnd;                 /* end of the usable record area */
    /* append cursor */
    ULONGLONG   PgBase;                 /* absolute offset of the current record page */
    ULONG       PgUsed;                 /* bytes used in PgBuf */
    PUCHAR      PgBuf;                   /* NJ_PAGE bytes */
    ULONGLONG   Lsn;
    ULONG       NextTx;
    /* the current top-level transaction */
    ULONG       TxDepth;
    ULONG       CurTx;
    BOOLEAN     CurTxWrote;
    BOOLEAN     HdrWritten;             /* the NJLG header is on disk (we are dirty) */
    BOOLEAN     VolumeDirtySet;
    PUCHAR      Scratch;                /* MftRecordSize scratch for undo */
    PNTFS_RUN   TmpRuns;                /* run scratch for undo target streams */
} NTFS_JOURNAL, *PNTFS_JOURNAL;

/* ------------------------------------------------------------------ */
/* raw $LogFile I/O                                                    */
/* ------------------------------------------------------------------ */

static NTSTATUS
LogRead(PNTFS_JOURNAL J, ULONGLONG Off, ULONG Len, PVOID Buf)
{
    return NtfsReadAttributeData(J->Vcb, J->Runs, J->RunCount, Off, Len, Buf);
}
static NTSTATUS
LogWrite(PNTFS_JOURNAL J, ULONGLONG Off, ULONG Len, PVOID Buf)
{
    return NtfsWriteAttributeData(J->Vcb, J->Runs, J->RunCount, Off, Len, Buf);
}

/* ------------------------------------------------------------------ */
/* clean LFS restart pages (so tools see a valid, clean $LogFile)       */
/* ------------------------------------------------------------------ */

static ULONG
SeqNumberBits(ULONGLONG FileSize)
{
    ULONG b = 67;
    while (FileSize) { FileSize >>= 1; b--; }
    return b;
}

/* Build one clean RSTR restart page into Buf (SysPage bytes) and stamp its
 * update sequence array. WinXP+ clean convention: log open (in_use=0,
 * free=NO_CLIENT) with the VOLUME_IS_CLEAN flag set. */
static VOID
BuildRestartPage(PNTFS_JOURNAL J, PUCHAR Buf)
{
    PLFS_RESTART_PAGE_HEADER h = (PLFS_RESTART_PAGE_HEADER)Buf;
    ULONG usaCount = (J->SysPage >> 9) + 1;
    ULONG raOff = (0x1E + 2 * usaCount + 7) & ~7u;     /* fixups at 0x1E */
    PLFS_RESTART_AREA ra;
    PLFS_CLIENT_RECORD cr;
    ULONG clientOff = 0x30;

    RtlZeroMemory(Buf, J->SysPage);
    h->Signature = LFS_RSTR_SIGNATURE;
    h->UsaOffset = 0x1E;
    h->UsaCount = (USHORT)usaCount;
    h->ChkDskLsn = 0;
    h->SystemPageSize = J->SysPage;
    h->LogPageSize = J->SysPage;
    h->RestartAreaOffset = (USHORT)raOff;
    h->MinorVersion = 1;
    h->MajorVersion = 1;

    ra = (PLFS_RESTART_AREA)(Buf + raOff);
    ra->CurrentLsn = J->Lsn;
    ra->LogClients = 1;
    ra->ClientFreeList = LFS_CLIENT_NONE;
    ra->ClientInUseList = 0;
    ra->Flags = LFS_RESTART_VOLUME_IS_CLEAN;
    ra->SeqNumberBits = SeqNumberBits(J->LogBytes);
    ra->RestartAreaLength = (USHORT)(clientOff + 0xA0);
    ra->ClientArrayOffset = (USHORT)clientOff;
    ra->FileSize = J->LogBytes;
    ra->LastLsnDataLength = 0;
    ra->LogRecordHeaderLength = 0x30;
    ra->LogPageDataOffset = 0x40;
    ra->RestartLogOpenCount = (ULONG)J->Lsn + 1;

    cr = (PLFS_CLIENT_RECORD)((PUCHAR)ra + clientOff);
    cr->OldestLsn = 0;
    cr->ClientRestartLsn = J->Lsn;
    cr->PrevClient = LFS_CLIENT_NONE;
    cr->NextClient = LFS_CLIENT_NONE;
    cr->SeqNumber = 0;
    cr->ClientNameLength = 8;
    cr->ClientName[0] = 'N'; cr->ClientName[1] = 'T';
    cr->ClientName[2] = 'F'; cr->ClientName[3] = 'S';

    NtfsSetFixup(Buf, J->SysPage, 512);
}

static NTSTATUS
WriteCleanRestart(PNTFS_JOURNAL J)
{
    PUCHAR buf = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, J->SysPage, NTFS_TAG_BUF);
    NTSTATUS st;
    if (!buf) return STATUS_INSUFFICIENT_RESOURCES;
    BuildRestartPage(J, buf);
    st = LogWrite(J, 0, J->SysPage, buf);
    if (NT_SUCCESS(st)) st = LogWrite(J, J->SysPage, J->SysPage, buf);
    ExFreePool(buf);
    return st;
}

/* ------------------------------------------------------------------ */
/* $Volume dirty bit                                                   */
/* ------------------------------------------------------------------ */

static VOID
SetVolumeDirty(PNTFS_VCB Vcb, BOOLEAN Dirty)
{
    PUCHAR rec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    PMFT_RECORD m;
    PATTRIBUTE_RECORD vi;
    PVOLUME_INFORMATION v;
    if (!rec) return;
    if (NT_SUCCESS(NtfsReadMftRecord(Vcb, MFT_RN_VOLUME, rec))) {
        m = (PMFT_RECORD)rec;
        vi = NtfsFindAttribute(m, ATTR_VOLUME_INFORMATION, NULL, 0);
        if (vi && !vi->NonResident && NtfsResidentValueLength(vi) >= sizeof(VOLUME_INFORMATION)) {
            v = (PVOLUME_INFORMATION)NtfsResidentValue(vi);
            if (Dirty) v->Flags |= VOLUME_FLAG_DIRTY;
            else       v->Flags &= ~VOLUME_FLAG_DIRTY;
            NtfsWriteMftRecord(Vcb, MFT_RN_VOLUME, rec);
        }
    }
    ExFreePool(rec);
}

/* ------------------------------------------------------------------ */
/* record-page appender (write-ahead)                                  */
/* ------------------------------------------------------------------ */

static VOID
WriteHeader(PNTFS_JOURNAL J, ULONG State)
{
    PNJ_HDR h = (PNJ_HDR)J->PgBuf;        /* reuse PgBuf transiently is unsafe; use a tiny stack-free buffer */
    PUCHAR buf = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, J->SysPage, NTFS_TAG_BUF);
    if (!buf) return;
    RtlFillMemory(buf, J->SysPage, 0xFF);
    h = (PNJ_HDR)buf;
    h->Magic = (State == NJ_STATE_DIRTY) ? NJ_HDR_MAGIC : 0xFFFFFFFFUL;
    h->Version = 1;
    h->State = State;
    h->PageSize = NJ_PAGE;
    h->Lsn = J->Lsn;
    h->NextTx = J->NextTx;
    h->Sum = h->Magic ^ h->State ^ h->PageSize ^ (ULONG)h->Lsn ^ h->NextTx;
    LogWrite(J, J->RecBase - J->SysPage, J->SysPage, buf);   /* header page sits just before the records */
    ExFreePool(buf);
}

/* Ensure the NJLG dirty header is on disk before we start appending records. */
static VOID
EnsureDirty(PNTFS_JOURNAL J)
{
    /* set the flags BEFORE the writes: SetVolumeDirty writes the $Volume record
     * through NtfsWriteMftRecord, which (inside a transaction) calls back into
     * NtfsJnlRecord -> EnsureDirty; the flag must already be set or that
     * recurses without end (kernel stack overflow). */
    if (!J->HdrWritten) {
        J->HdrWritten = TRUE;
        WriteHeader(J, NJ_STATE_DIRTY);
    }
    if (!J->VolumeDirtySet) {
        J->VolumeDirtySet = TRUE;
        SetVolumeDirty(J->Vcb, TRUE);
    }
}

static NTSTATUS
FlushPage(PNTFS_JOURNAL J)
{
    return LogWrite(J, J->PgBase, NJ_PAGE, J->PgBuf);
}

/* Append one framed record (Hdr + Data + end magic), flush it to disk. */
static NTSTATUS
Append(PNTFS_JOURNAL J, PNJ_REC Hdr, PVOID Data, ULONG DataLen)
{
    ULONG total = sizeof(NJ_REC) + DataLen + sizeof(ULONG);
    ULONG aligned = (total + 7) & ~7u;
    PUCHAR p;
    if (aligned > NJ_PAGE) return STATUS_INSUFFICIENT_RESOURCES;   /* record too big for a page */
    if (J->PgUsed + aligned > NJ_PAGE) {
        /* pad the rest of the page and move to the next one */
        if (J->PgBase + NJ_PAGE + NJ_PAGE > J->RecEnd) {
            /* out of log room: everything committed is already durable and no
               uncommitted work spans here, so recycle the log from the start */
            J->PgBase = J->RecBase;
        } else {
            FlushPage(J);
            J->PgBase += NJ_PAGE;
        }
        RtlFillMemory(J->PgBuf, NJ_PAGE, 0xFF);
        J->PgUsed = 0;
    }
    Hdr->Magic = NJ_REC_MAGIC;
    Hdr->RecLen = aligned;
    Hdr->Lsn = ++J->Lsn;
    p = J->PgBuf + J->PgUsed;
    RtlCopyMemory(p, Hdr, sizeof(NJ_REC));
    if (DataLen) RtlCopyMemory(p + sizeof(NJ_REC), Data, DataLen);
    *(PULONG)(p + sizeof(NJ_REC) + DataLen) = NJ_END_MAGIC;
    J->PgUsed += aligned;
    return FlushPage(J);     /* write-ahead: durable before the caller writes metadata */
}

/* ------------------------------------------------------------------ */
/* public: active test                                                 */
/* ------------------------------------------------------------------ */

BOOLEAN
NtfsJnlActive(PNTFS_VCB Vcb)
{
    return (BOOLEAN)(Vcb->Journal != NULL);
}

/* TRUE while a transaction is open: the write primitives read the old image
 * to log only then, and skip the cost otherwise. */
BOOLEAN
NtfsJnlInTx(PNTFS_VCB Vcb)
{
    PNTFS_JOURNAL J = (PNTFS_JOURNAL)Vcb->Journal;
    return (BOOLEAN)(J != NULL && J->TxDepth != 0);
}

/* ------------------------------------------------------------------ */
/* begin / record / range / commit / abort                             */
/* ------------------------------------------------------------------ */

VOID
NtfsJnlBegin(PNTFS_VCB Vcb)
{
    PNTFS_JOURNAL J = (PNTFS_JOURNAL)Vcb->Journal;
    if (!J) return;
    if (J->TxDepth++ == 0) {
        J->CurTx = ++J->NextTx;
        J->CurTxWrote = FALSE;
    }
}

VOID
NtfsJnlRecord(PNTFS_VCB Vcb, ULONGLONG RecNo, PVOID Old, PVOID New)
{
    PNTFS_JOURNAL J = (PNTFS_JOURNAL)Vcb->Journal;
    NJ_REC h;
    if (!J || J->TxDepth == 0) return;
    EnsureDirty(J);
    RtlZeroMemory(&h, sizeof(h));
    h.TxId = J->CurTx;
    h.Target = RecNo;
    h.Kind = NTFS_JNL_MFT;
    if (Old) {
        h.Type = NJ_REC_TYPE_UNDO;
        h.Length = Vcb->Geo.MftRecordSize;
        Append(J, &h, Old, Vcb->Geo.MftRecordSize);
    } else {
        /* a record being brought into use: undo = mark it free again */
        h.Type = NJ_REC_TYPE_CREATE;
        h.Length = 0;
        Append(J, &h, NULL, 0);
    }
    /* stamp a rising $LogFile LSN into the record about to be written */
    if (New) ((PNTFS_RECORD_HEADER)New)->Lsn = J->Lsn;
    J->CurTxWrote = TRUE;
}

VOID
NtfsJnlRange(PNTFS_VCB Vcb, ULONG Kind, ULONGLONG Target, ULONGLONG Vbo,
             PVOID Old, PVOID New, ULONG Length)
{
    PNTFS_JOURNAL J = (PNTFS_JOURNAL)Vcb->Journal;
    NJ_REC h;
    UNREFERENCED_PARAMETER(New);
    if (!J || J->TxDepth == 0 || Length == 0) return;
    if (sizeof(NJ_REC) + Length + sizeof(ULONG) > NJ_PAGE) return;  /* too big to log; skip (rare) */
    EnsureDirty(J);
    RtlZeroMemory(&h, sizeof(h));
    h.Type = NJ_REC_TYPE_UNDO;
    h.TxId = J->CurTx;
    h.Kind = Kind;
    h.Target = Target;
    h.Vbo = Vbo;
    h.Length = Length;
    if (Old) Append(J, &h, Old, Length);
    /* for an INDX block, stamp the LSN into the block header being written */
    if (Kind == NTFS_JNL_INDEX && New &&
        ((PNTFS_RECORD_HEADER)New)->Type == NTFS_INDX_SIGNATURE)
        ((PNTFS_RECORD_HEADER)New)->Lsn = J->Lsn;
    J->CurTxWrote = TRUE;
}

NTSTATUS
NtfsJnlCommit(PNTFS_VCB Vcb)
{
    PNTFS_JOURNAL J = (PNTFS_JOURNAL)Vcb->Journal;
    NJ_REC h;
    if (!J) return STATUS_SUCCESS;
    if (J->TxDepth == 0) return STATUS_SUCCESS;
    if (--J->TxDepth) return STATUS_SUCCESS;
    if (J->CurTxWrote) {
        RtlZeroMemory(&h, sizeof(h));
        h.Type = NJ_REC_TYPE_COMMIT;
        h.TxId = J->CurTx;
        Append(J, &h, NULL, 0);
    }
    return STATUS_SUCCESS;
}

/* Roll back the records of the current (outermost) transaction now, because
 * the operation failed partway. We re-read our own log from this tx's first
 * record to the cursor and apply the undo images in reverse. Simpler and rare:
 * since each metadata write is write-through, the safe thing is to leave the
 * undo records in the log without a commit marker - our mount-time replay will
 * undo them. But the volume is live now, so apply them immediately. */
static VOID ReplayUndo(PNTFS_JOURNAL J, ULONG OnlyTx, BOOLEAN HaveCommitList);

VOID
NtfsJnlAbort(PNTFS_VCB Vcb)
{
    PNTFS_JOURNAL J = (PNTFS_JOURNAL)Vcb->Journal;
    if (!J) return;
    if (J->TxDepth == 0) return;
    if (--J->TxDepth) return;
    if (J->CurTxWrote) {
        /* undo this transaction's writes right away (it has no commit marker) */
        ReplayUndo(J, J->CurTx, FALSE);
    }
}

/* ------------------------------------------------------------------ */
/* applying an undo image to its target                                */
/* ------------------------------------------------------------------ */

/* decode a stream's runs for an undo write; returns run count or 0 */
static ULONG
TargetRuns(PNTFS_JOURNAL J, ULONG Kind, ULONGLONG Target, PNTFS_RUN Runs, ULONG MaxRuns, PULONG Ibs)
{
    PNTFS_VCB Vcb = J->Vcb;
    PUCHAR rec = J->Scratch;
    PMFT_RECORD m;
    PATTRIBUTE_RECORD a = NULL;
    ULONG rc = 0;
    static const WCHAR I30[4] = { '$','I','3','0' };
    ULONGLONG recno;
    if (Ibs) *Ibs = 0;
    switch (Kind) {
    case NTFS_JNL_INDEX:  recno = Target;        break;
    case NTFS_JNL_BITMAP: recno = MFT_RN_BITMAP; break;
    case NTFS_JNL_MFTBMP: recno = MFT_RN_MFT;    break;
    case NTFS_JNL_DATA:   recno = Target;        break;
    default: return 0;
    }
    if (!NT_SUCCESS(NtfsReadMftRecord(Vcb, recno, rec))) return 0;
    m = (PMFT_RECORD)rec;
    switch (Kind) {
    case NTFS_JNL_INDEX:  a = NtfsFindAttribute(m, ATTR_INDEX_ALLOCATION, I30, 4);
                          if (Ibs) *Ibs = Vcb->Geo.IndexBlockSize; break;
    case NTFS_JNL_BITMAP: a = NtfsFindAttribute(m, ATTR_DATA, NULL, 0);       break;
    case NTFS_JNL_MFTBMP: a = NtfsFindAttribute(m, ATTR_BITMAP, NULL, 0);     break;
    case NTFS_JNL_DATA:   a = NtfsFindAttribute(m, ATTR_DATA, NULL, 0);       break;
    }
    if (!a || !a->NonResident) return 0;
    if (!NtfsDecodeRuns(a, Runs, MaxRuns, &rc)) return 0;
    return rc;
}

static VOID
ApplyUndo(PNTFS_JOURNAL J, PNJ_REC h, PUCHAR data)
{
    PNTFS_VCB Vcb = J->Vcb;
    if (h->Type == NJ_REC_TYPE_CREATE) {
        /* bring a record back out of use */
        PUCHAR rec = J->Scratch;
        if (NT_SUCCESS(NtfsReadMftRecord(Vcb, h->Target, rec))) {
            PMFT_RECORD m = (PMFT_RECORD)rec;
            m->Flags &= ~MFT_RECORD_IN_USE;
            m->SequenceNumber++;
            NtfsWriteMftRecord(Vcb, h->Target, rec);
        }
        return;
    }
    if (h->Kind == NTFS_JNL_MFT) {
        NtfsWriteMftRecord(Vcb, h->Target, data);
        return;
    }
    {
        ULONG ibs = 0;
        ULONG rc = TargetRuns(J, h->Kind, h->Target, J->TmpRuns, NTFS_MAX_IDX_RUNS, &ibs);
        if (rc == 0) return;
        if (h->Kind == NTFS_JNL_INDEX && ibs)
            NtfsSetFixup(data, ibs, Vcb->Geo.BytesPerSector);     /* INDX block carries a USA */
        NtfsWriteAttributeData(Vcb, J->TmpRuns, rc, h->Vbo, h->Length, data);
    }
}

/* ------------------------------------------------------------------ */
/* replay at mount (undo uncommitted transactions)                     */
/* ------------------------------------------------------------------ */

/* A scanned undo record kept in memory during replay. */
typedef struct _NJ_SCAN {
    NJ_REC  Hdr;
    PUCHAR  Data;           /* Length bytes (NULL for create/commit) */
} NJ_SCAN, *PNJ_SCAN;

/* Walk the record area, collecting undo records; apply those of transactions
 * that have no commit marker, in reverse. If OnlyTx != 0 only that transaction
 * is undone (used by NtfsJnlAbort on a live volume). */
static VOID
ReplayUndo(PNTFS_JOURNAL J, ULONG OnlyTx, BOOLEAN HaveCommitList)
{
    PNTFS_VCB Vcb = J->Vcb;
    PUCHAR page = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, NJ_PAGE, NTFS_TAG_BUF);
    PNJ_SCAN scan = NULL;
    PULONG committed = NULL;
    ULONG scanCap = 4096, scanN = 0, comCap = 256, comN = 0;
    ULONGLONG off;
    ULONG i;
    UNREFERENCED_PARAMETER(HaveCommitList);
    if (!page) return;
    scan = (PNJ_SCAN)ExAllocatePoolWithTag(NonPagedPool, scanCap * sizeof(NJ_SCAN), NTFS_TAG_BUF);
    committed = (PULONG)ExAllocatePoolWithTag(NonPagedPool, comCap * sizeof(ULONG), NTFS_TAG_BUF);
    if (!scan || !committed) goto out;

    for (off = J->RecBase; off + NJ_PAGE <= J->RecEnd; off += NJ_PAGE) {
        ULONG p = 0;
        if (!NT_SUCCESS(LogRead(J, off, NJ_PAGE, page))) break;
        if (((PNJ_REC)page)->Magic != NJ_REC_MAGIC) break;      /* end of the live log */
        while (p + sizeof(NJ_REC) <= NJ_PAGE) {
            PNJ_REC r = (PNJ_REC)(page + p);
            ULONG end;
            if (r->Magic != NJ_REC_MAGIC) break;
            if (r->RecLen < sizeof(NJ_REC) || r->RecLen > NJ_PAGE - p) break;
            end = p + sizeof(NJ_REC) + r->Length;
            if (end + sizeof(ULONG) > NJ_PAGE) break;
            if (*(PULONG)(page + end) != NJ_END_MAGIC) break;   /* torn/incomplete */
            if (OnlyTx && r->TxId != OnlyTx) { p += r->RecLen; continue; }
            if (r->Type == NJ_REC_TYPE_COMMIT) {
                if (comN == comCap) { } else committed[comN++] = r->TxId;
            } else {
                if (scanN == scanCap) {
                    ULONG nc = scanCap * 2;
                    PNJ_SCAN ns = (PNJ_SCAN)ExAllocatePoolWithTag(NonPagedPool, nc * sizeof(NJ_SCAN), NTFS_TAG_BUF);
                    if (!ns) goto apply;
                    RtlCopyMemory(ns, scan, scanN * sizeof(NJ_SCAN));
                    ExFreePool(scan); scan = ns; scanCap = nc;
                }
                scan[scanN].Hdr = *r;
                if (r->Length) {
                    scan[scanN].Data = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, r->Length, NTFS_TAG_BUF);
                    if (scan[scanN].Data) RtlCopyMemory(scan[scanN].Data, page + p + sizeof(NJ_REC), r->Length);
                } else scan[scanN].Data = NULL;
                scanN++;
            }
            p += r->RecLen;
        }
    }

apply:
    /* undo, in reverse, every record whose transaction never committed */
    {
        ULONG undone = 0;
        for (i = scanN; i-- > 0; ) {
            ULONG tx = scan[i].Hdr.TxId, k; BOOLEAN done = FALSE;
            if (OnlyTx) { if (tx != OnlyTx) continue; }
            else { for (k = 0; k < comN; k++) if (committed[k] == tx) { done = TRUE; break; } }
            if (done) continue;
            ApplyUndo(J, &scan[i].Hdr, scan[i].Data);
            undone++;
        }
        DBGP("[NTFS3NT: scanned "); DBGX(scanN); DBGP(" recs, undid "); DBGX(undone); DBGP("]\n");
    }
    (void)Vcb;

out:
    if (scan) { for (i = 0; i < scanN; i++) if (scan[i].Data) ExFreePool(scan[i].Data); ExFreePool(scan); }
    if (committed) ExFreePool(committed);
    if (page) ExFreePool(page);
}

/* ------------------------------------------------------------------ */
/* mount / unmount                                                     */
/* ------------------------------------------------------------------ */

static VOID
FreeJournal(PNTFS_JOURNAL J)
{
    if (!J) return;
    if (J->Runs) ExFreePool(J->Runs);
    if (J->PgBuf) ExFreePool(J->PgBuf);
    if (J->Scratch) ExFreePool(J->Scratch);
    if (J->TmpRuns) ExFreePool(J->TmpRuns);
    ExFreePool(J);
}

VOID
NtfsJnlMount(PNTFS_VCB Vcb, BOOLEAN ReadOnly)
{
    PNTFS_JOURNAL J;
    PUCHAR rec = NULL;
    PMFT_RECORD m;
    PATTRIBUTE_RECORD a;
    PNTFS_RUN runs = NULL;
    ULONG rc = 0;
    PNJ_HDR hdr = NULL;

    Vcb->Journal = NULL;
    if (ReadOnly || !NtfsData.WriteEnabled) return;

    J = (PNTFS_JOURNAL)ExAllocatePoolWithTag(NonPagedPool, sizeof(NTFS_JOURNAL), NTFS_TAG_VCB);
    rec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    if (!J || !rec) goto fail;
    RtlZeroMemory(J, sizeof(*J));
    J->Vcb = Vcb;
    J->SysPage = 4096;

    /* $LogFile ($DATA of record 2) runs */
    if (!NT_SUCCESS(NtfsReadMftRecord(Vcb, MFT_RN_LOGFILE, rec))) goto fail;
    m = (PMFT_RECORD)rec;
    a = NtfsFindAttribute(m, ATTR_DATA, NULL, 0);
    if (!a || !a->NonResident) goto fail;        /* resident/absent $LogFile: no journaling */
    J->LogBytes = a->Form.NonResident.DataSize;
    if (J->LogBytes < (ULONGLONG)(J->SysPage * 8)) goto fail;
    runs = (PNTFS_RUN)ExAllocatePoolWithTag(NonPagedPool, NTFS_MAX_MFT_RUNS * sizeof(NTFS_RUN), NTFS_TAG_RUN);
    if (!runs || !NtfsDecodeRuns(a, runs, NTFS_MAX_MFT_RUNS, &rc) || rc == 0) goto fail;
    J->Runs = (PNTFS_RUN)ExAllocatePoolWithTag(NonPagedPool, rc * sizeof(NTFS_RUN), NTFS_TAG_RUN);
    if (!J->Runs) goto fail;
    RtlCopyMemory(J->Runs, runs, rc * sizeof(NTFS_RUN));
    J->RunCount = rc;
    ExFreePool(runs); runs = NULL;

    J->PgBuf = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, NJ_PAGE, NTFS_TAG_BUF);
    J->Scratch = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    J->TmpRuns = (PNTFS_RUN)ExAllocatePoolWithTag(NonPagedPool, NTFS_MAX_IDX_RUNS * sizeof(NTFS_RUN), NTFS_TAG_RUN);
    hdr = (PNJ_HDR)ExAllocatePoolWithTag(NonPagedPool, J->SysPage, NTFS_TAG_BUF);
    if (!J->PgBuf || !J->Scratch || !J->TmpRuns || !hdr) goto fail;

    /* our header page sits at 4*SysPage; records follow */
    J->RecBase = 5 * (ULONGLONG)J->SysPage;
    J->RecEnd = J->LogBytes & ~((ULONGLONG)NJ_PAGE - 1);
    if (J->RecEnd <= J->RecBase + NJ_PAGE) goto fail;

    /* was the log left dirty (we crashed)? */
    {
        NTSTATUS rst = LogRead(J, J->RecBase - J->SysPage, J->SysPage, (PUCHAR)hdr);
        DBGP("[NTFS3NT: hdr rd="); DBGX(rst); DBGP(" magic="); DBGX(hdr->Magic);
        DBGP(" state="); DBGX(hdr->State); DBGP(" logLcn="); DBGX((ULONG)J->Runs[0].Lcn); DBGP("]\n");
        if (NT_SUCCESS(rst) && hdr->Magic == NJ_HDR_MAGIC && hdr->State == NJ_STATE_DIRTY) {
            J->Lsn = hdr->Lsn;
            J->NextTx = hdr->NextTx;
            DBGP("[NTFS3NT: log DIRTY - replaying]\n");
            ReplayUndo(J, 0, TRUE);                  /* undo uncommitted transactions */
            DBGP("[NTFS3NT: replay done]\n");
        } else {
            DBGP("[NTFS3NT: log clean]\n");
        }
    }

    /* fresh, clean log state */
    J->Lsn++;
    J->NextTx = 0;
    J->PgBase = J->RecBase;
    J->PgUsed = 0;
    RtlFillMemory(J->PgBuf, NJ_PAGE, 0xFF);
    J->HdrWritten = FALSE;
    J->VolumeDirtySet = FALSE;
    WriteHeader(J, 0xFFFFFFFFUL);                /* clear our dirty header */
    WriteCleanRestart(J);                        /* valid, clean LFS restart pages */
    SetVolumeDirty(Vcb, FALSE);

    ExFreePool(hdr);
    ExFreePool(rec);
    Vcb->Journal = J;
    DBGP("[NTFS3NT: journal active]\n");
    return;

fail:
    if (runs) ExFreePool(runs);
    if (hdr) ExFreePool(hdr);
    if (rec) ExFreePool(rec);
    FreeJournal(J);
    Vcb->Journal = NULL;
}

VOID
NtfsJnlUnmount(PNTFS_VCB Vcb, BOOLEAN Clean)
{
    PNTFS_JOURNAL J = (PNTFS_JOURNAL)Vcb->Journal;
    if (!J) return;
    if (Clean) {
        /* blank our records, clear the dirty header and the $Volume dirty bit,
           and leave a clean LFS restart page pair behind */
        WriteHeader(J, 0xFFFFFFFFUL);
        WriteCleanRestart(J);
        if (J->VolumeDirtySet) SetVolumeDirty(Vcb, FALSE);
    }
    Vcb->Journal = NULL;
    FreeJournal(J);
}
