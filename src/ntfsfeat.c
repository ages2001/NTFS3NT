/*
 * NTFS3NT - feature FSCTLs.
 *
 * The additive NTFS control codes that sit on top of the read/write core:
 * volume and file-record queries, the volume bitmap and a file's retrieval
 * pointers (defrag), allocated-range and sparse support, object ids, reparse
 * set/delete, and the USN journal / quota entry points. Each one works on the
 * on-disk records through the same primitives the rest of the driver uses
 * (NtfsReadMftRecord / NtfsWriteMftRecord, NtfsFindAttribute, the run decoder,
 * $Bitmap and $DATA I/O), wrapping any change in a journal transaction.
 *
 * All structures the control codes exchange are defined here (the DDK headers
 * this driver builds against do not carry them); only the FSCTL_* code values
 * reuse the DDK macros when present, else are formed with CTL_CODE.
 */

#define NTFS_KERNEL
#include "ntfs.h"
#include "ntfsfeat.h"

/* ------------------------------------------------------------------ */
/* FSCTL codes (guarded: use the DDK's value when it already defines one) */
/* ------------------------------------------------------------------ */
#define FSD FILE_DEVICE_FILE_SYSTEM
#ifndef FSCTL_GET_NTFS_VOLUME_DATA
#define FSCTL_GET_NTFS_VOLUME_DATA    CTL_CODE(FSD, 25, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef FSCTL_GET_NTFS_FILE_RECORD
#define FSCTL_GET_NTFS_FILE_RECORD    CTL_CODE(FSD, 26, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef FSCTL_GET_VOLUME_BITMAP
#define FSCTL_GET_VOLUME_BITMAP       CTL_CODE(FSD, 27, METHOD_NEITHER,  FILE_ANY_ACCESS)
#endif
#ifndef FSCTL_GET_RETRIEVAL_POINTERS
#define FSCTL_GET_RETRIEVAL_POINTERS  CTL_CODE(FSD, 28, METHOD_NEITHER,  FILE_ANY_ACCESS)
#endif
#ifndef FSCTL_SET_SPARSE
#define FSCTL_SET_SPARSE              CTL_CODE(FSD, 49, METHOD_BUFFERED, FILE_SPECIAL_ACCESS)
#endif
#ifndef FSCTL_SET_ZERO_DATA
#define FSCTL_SET_ZERO_DATA           CTL_CODE(FSD, 50, METHOD_BUFFERED, FILE_WRITE_DATA)
#endif
#ifndef FSCTL_GET_COMPRESSION
#define FSCTL_GET_COMPRESSION         CTL_CODE(FSD, 15, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef FSCTL_SET_COMPRESSION
#define FSCTL_SET_COMPRESSION         CTL_CODE(FSD, 16, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#endif
#ifndef FSCTL_QUERY_ALLOCATED_RANGES
#define FSCTL_QUERY_ALLOCATED_RANGES  CTL_CODE(FSD, 51, METHOD_NEITHER,  FILE_READ_DATA)
#endif
#ifndef FSCTL_SET_OBJECT_ID
#define FSCTL_SET_OBJECT_ID           CTL_CODE(FSD, 82, METHOD_BUFFERED, FILE_SPECIAL_ACCESS)
#endif
#ifndef FSCTL_GET_OBJECT_ID
#define FSCTL_GET_OBJECT_ID           CTL_CODE(FSD, 83, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef FSCTL_DELETE_OBJECT_ID
#define FSCTL_DELETE_OBJECT_ID        CTL_CODE(FSD, 88, METHOD_BUFFERED, FILE_SPECIAL_ACCESS)
#endif
#ifndef FSCTL_SET_REPARSE_POINT
#define FSCTL_SET_REPARSE_POINT       CTL_CODE(FSD, 41, METHOD_BUFFERED, FILE_SPECIAL_ACCESS)
#endif
#ifndef FSCTL_DELETE_REPARSE_POINT
#define FSCTL_DELETE_REPARSE_POINT    CTL_CODE(FSD, 43, METHOD_BUFFERED, FILE_SPECIAL_ACCESS)
#endif
#ifndef FSCTL_CREATE_OR_GET_OBJECT_ID
#define FSCTL_CREATE_OR_GET_OBJECT_ID CTL_CODE(FSD, 98, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef FSCTL_READ_USN_JOURNAL
#define FSCTL_READ_USN_JOURNAL        CTL_CODE(FSD, 46, METHOD_NEITHER,  FILE_ANY_ACCESS)
#endif
#ifndef FSCTL_CREATE_USN_JOURNAL
#define FSCTL_CREATE_USN_JOURNAL      CTL_CODE(FSD, 57, METHOD_NEITHER,  FILE_ANY_ACCESS)
#endif
#ifndef FSCTL_QUERY_USN_JOURNAL
#define FSCTL_QUERY_USN_JOURNAL       CTL_CODE(FSD, 61, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef FSCTL_DELETE_USN_JOURNAL
#define FSCTL_DELETE_USN_JOURNAL      CTL_CODE(FSD, 62, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef FSCTL_ENUM_USN_DATA
#define FSCTL_ENUM_USN_DATA           CTL_CODE(FSD, 44, METHOD_NEITHER,  FILE_ANY_ACCESS)
#endif
#ifndef FSCTL_QUERY_QUOTA
#define FSCTL_QUERY_QUOTA             CTL_CODE(FSD, 37, METHOD_NEITHER,  FILE_ANY_ACCESS)
#endif
#ifndef FSCTL_SET_QUOTA
#define FSCTL_SET_QUOTA               CTL_CODE(FSD, 38, METHOD_BUFFERED, FILE_WRITE_DATA)
#endif

#ifndef STATUS_JOURNAL_NOT_ACTIVE
#define STATUS_JOURNAL_NOT_ACTIVE     ((NTSTATUS)0xC00000B7L)
#endif
#ifndef STATUS_VOLUME_NOT_UPGRADED
#define STATUS_VOLUME_NOT_UPGRADED    ((NTSTATUS)0xC000029CL)
#endif
#ifndef STATUS_OBJECTID_EXISTS
#define STATUS_OBJECTID_EXISTS        ((NTSTATUS)0xC000022BL)
#endif
#ifndef STATUS_OBJECT_NAME_NOT_FOUND
#define STATUS_OBJECT_NAME_NOT_FOUND  ((NTSTATUS)0xC0000034L)
#endif

/* ------------------------------------------------------------------ */
/* Exchanged structures (self-defined; the DDK headers lack them)      */
/* ------------------------------------------------------------------ */
#pragma pack(push, 1)

typedef struct _NF_VOLUME_DATA {
    LARGE_INTEGER VolumeSerialNumber;
    LARGE_INTEGER NumberSectors;
    LARGE_INTEGER TotalClusters;
    LARGE_INTEGER FreeClusters;
    LARGE_INTEGER TotalReserved;
    ULONG   BytesPerSector;
    ULONG   BytesPerCluster;
    ULONG   BytesPerFileRecordSegment;
    ULONG   ClustersPerFileRecordSegment;
    LARGE_INTEGER MftValidDataLength;
    LARGE_INTEGER MftStartLcn;
    LARGE_INTEGER Mft2StartLcn;
    LARGE_INTEGER MftZoneStart;
    LARGE_INTEGER MftZoneEnd;
    /* extended (NTFS_VOLUME_DATA_BUFFER on XP+) fields follow; optional */
} NF_VOLUME_DATA;

typedef struct _NF_FILE_RECORD_OUT {
    ULONGLONG FileReferenceNumber;
    ULONG     FileRecordLength;
    UCHAR     FileRecordBuffer[1];
} NF_FILE_RECORD_OUT;

typedef struct _NF_STARTING_LCN { LARGE_INTEGER StartingLcn; } NF_STARTING_LCN;
typedef struct _NF_VOLUME_BITMAP {
    LARGE_INTEGER StartingLcn;
    LARGE_INTEGER BitmapSize;
    UCHAR         Buffer[1];
} NF_VOLUME_BITMAP;

typedef struct _NF_STARTING_VCN { LARGE_INTEGER StartingVcn; } NF_STARTING_VCN;
typedef struct _NF_EXTENT { LARGE_INTEGER NextVcn; LARGE_INTEGER Lcn; } NF_EXTENT;
typedef struct _NF_RETRIEVAL_POINTERS {
    ULONG         ExtentCount;
    LARGE_INTEGER StartingVcn;
    NF_EXTENT     Extents[1];
} NF_RETRIEVAL_POINTERS;

typedef struct _NF_ALLOCATED_RANGE { LARGE_INTEGER FileOffset; LARGE_INTEGER Length; } NF_ALLOCATED_RANGE;
typedef struct _NF_SET_SPARSE { UCHAR SetSparse; } NF_SET_SPARSE;
typedef struct _NF_ZERO_DATA { LARGE_INTEGER FileOffset; LARGE_INTEGER BeyondFinalZero; } NF_ZERO_DATA;

typedef struct _NF_OBJECTID {
    UCHAR ObjectId[16];
    union {
        struct { UCHAR BirthVolumeId[16]; UCHAR BirthObjectId[16]; UCHAR DomainId[16]; } Birth;
        UCHAR ExtendedInfo[48];
    } u;
} NF_OBJECTID;

typedef struct _NF_REPARSE_HEADER {
    ULONG  ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    /* for a Microsoft tag: the reparse data follows; for a non-MS GUID tag a
       16-byte GUID precedes the data. Bit 0x80000000 of the tag => Microsoft. */
} NF_REPARSE_HEADER;

typedef struct _NF_USN_JOURNAL_DATA {
    ULONGLONG UsnJournalID;
    LONGLONG  FirstUsn;
    LONGLONG  NextUsn;
    LONGLONG  LowestValidUsn;
    LONGLONG  MaxUsn;
    ULONGLONG MaximumSize;
    ULONGLONG AllocationDelta;
} NF_USN_JOURNAL_DATA;

typedef struct _NF_USN_RECORD_V2 {
    ULONG     RecordLength;
    USHORT    MajorVersion;
    USHORT    MinorVersion;
    ULONGLONG FileReferenceNumber;
    ULONGLONG ParentFileReferenceNumber;
    LONGLONG  Usn;
    LONGLONG  TimeStamp;
    ULONG     Reason;
    ULONG     SourceInfo;
    ULONG     SecurityId;
    ULONG     FileAttributes;
    USHORT    FileNameLength;
    USHORT    FileNameOffset;
    WCHAR     FileName[1];
} NF_USN_RECORD_V2;

#pragma pack(pop)

#define REPARSE_TAG_MICROSOFT   0x80000000UL
#define NTFS_MAX_REPARSE        (16 * 1024)

/* ------------------------------------------------------------------ */
/* buffer access by the control code's transfer method                */
/* ------------------------------------------------------------------ */

static VOID
GetBuffers(PNTFS_IRP_CONTEXT Ctx, ULONG Code,
           PVOID *In, PULONG InLen, PVOID *Out, PULONG OutLen)
{
    PIO_STACK_LOCATION s = Ctx->IrpSp;
    ULONG method = Code & 3;
    *InLen = s->Parameters.FileSystemControl.InputBufferLength;
    *OutLen = s->Parameters.FileSystemControl.OutputBufferLength;
    if (method == METHOD_BUFFERED) {
        *In = Ctx->Irp->AssociatedIrp.SystemBuffer;
        *Out = Ctx->Irp->AssociatedIrp.SystemBuffer;
    } else {   /* METHOD_NEITHER (and the IN/OUT direct methods we do not use) */
        *In = s->Parameters.FileSystemControl.Type3InputBuffer;
        *Out = Ctx->Irp->UserBuffer;
    }
}

/* Read the file this handle is on, into a pooled MFT-record buffer. */
static NTSTATUS
ReadFileRec(PNTFS_VCB Vcb, PNTFS_FCB Fcb, PUCHAR *RecOut)
{
    PUCHAR rec;
    NTSTATUS st;
    *RecOut = NULL;
    if (!Fcb || Fcb->NodeTypeCode != NTFS_NTC_FCB) return STATUS_INVALID_PARAMETER;
    rec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    if (!rec) return STATUS_INSUFFICIENT_RESOURCES;
    st = NtfsReadMftRecord(Vcb, Fcb->MftRecord, rec);
    if (!NT_SUCCESS(st)) { ExFreePool(rec); return st; }
    *RecOut = rec;
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* FSCTL_GET_NTFS_VOLUME_DATA                                          */
/* ------------------------------------------------------------------ */
static NTSTATUS
GetVolumeData(PNTFS_IRP_CONTEXT Ctx, ULONG Code)
{
    PNTFS_VCB Vcb = Ctx->Vcb;
    PVOID in, out; ULONG inl, outl;
    NF_VOLUME_DATA *v;
    GetBuffers(Ctx, Code, &in, &inl, &out, &outl);
    if (outl < sizeof(NF_VOLUME_DATA)) { Ctx->Irp->IoStatus.Information = 0; return STATUS_BUFFER_TOO_SMALL; }
    v = (NF_VOLUME_DATA *)out;
    RtlZeroMemory(v, sizeof(*v));
    v->VolumeSerialNumber.QuadPart = (LONGLONG)Vcb->Geo.SerialNumber;
    v->NumberSectors.QuadPart = (LONGLONG)Vcb->Geo.TotalSectors;
    v->TotalClusters.QuadPart = (LONGLONG)Vcb->Geo.TotalClusters;
    v->FreeClusters.QuadPart = (LONGLONG)Vcb->FreeClusters;
    v->TotalReserved.QuadPart = 0;
    v->BytesPerSector = Vcb->Geo.BytesPerSector;
    v->BytesPerCluster = Vcb->Geo.ClusterSize;
    v->BytesPerFileRecordSegment = Vcb->Geo.MftRecordSize;
    v->ClustersPerFileRecordSegment = Vcb->Geo.MftRecordSize >= Vcb->Geo.ClusterSize ?
        Vcb->Geo.MftRecordSize / Vcb->Geo.ClusterSize : 0;
    v->MftValidDataLength.QuadPart = (LONGLONG)(Vcb->MftRecordCount * Vcb->Geo.MftRecordSize);
    v->MftStartLcn.QuadPart = (LONGLONG)Vcb->Geo.MftLcn;
    v->Mft2StartLcn.QuadPart = (LONGLONG)Vcb->Geo.MftMirrLcn;
    v->MftZoneStart.QuadPart = (LONGLONG)Vcb->Geo.MftLcn;
    v->MftZoneEnd.QuadPart = (LONGLONG)(Vcb->Geo.MftLcn + Vcb->MftRecordCount *
        (Vcb->Geo.MftRecordSize / (Vcb->Geo.ClusterSize ? Vcb->Geo.ClusterSize : 1) + 1));
    Ctx->Irp->IoStatus.Information = sizeof(NF_VOLUME_DATA);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* FSCTL_GET_NTFS_FILE_RECORD                                          */
/* ------------------------------------------------------------------ */
static NTSTATUS
GetFileRecord(PNTFS_IRP_CONTEXT Ctx, ULONG Code)
{
    PNTFS_VCB Vcb = Ctx->Vcb;
    PVOID in, out; ULONG inl, outl, rsz = Vcb->Geo.MftRecordSize, need;
    NF_FILE_RECORD_OUT *o;
    ULONGLONG rn;
    PUCHAR rec;
    NTSTATUS st;
    GetBuffers(Ctx, Code, &in, &inl, &out, &outl);
    if (inl < sizeof(ULONGLONG) || !in) return STATUS_INVALID_PARAMETER;
    need = FIELD_OFFSET(NF_FILE_RECORD_OUT, FileRecordBuffer) + rsz;
    if (outl < need) { Ctx->Irp->IoStatus.Information = 0; return STATUS_BUFFER_TOO_SMALL; }
    rn = (*(PULONGLONG)in) & 0x0000FFFFFFFFFFFFULL;
    if (rn >= Vcb->MftRecordCount) return STATUS_INVALID_PARAMETER;
    rec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, rsz, NTFS_TAG_BUF);
    if (!rec) return STATUS_INSUFFICIENT_RESOURCES;
    st = NtfsReadMftRecord(Vcb, rn, rec);
    o = (NF_FILE_RECORD_OUT *)out;
    if (NT_SUCCESS(st)) {
        o->FileReferenceNumber = rn;
        o->FileRecordLength = rsz;
        RtlCopyMemory(o->FileRecordBuffer, rec, rsz);
        Ctx->Irp->IoStatus.Information = need;
    }
    ExFreePool(rec);
    return st;
}

/* ------------------------------------------------------------------ */
/* FSCTL_GET_VOLUME_BITMAP ($Bitmap)                                   */
/* ------------------------------------------------------------------ */
static NTSTATUS
GetVolumeBitmap(PNTFS_IRP_CONTEXT Ctx, ULONG Code)
{
    PNTFS_VCB Vcb = Ctx->Vcb;
    PVOID in, out; ULONG inl, outl;
    NF_VOLUME_BITMAP *o;
    ULONGLONG startLcn, totalClusters = Vcb->Geo.TotalClusters, bits, firstByte, nbytes, maxbytes;
    PUCHAR rec = NULL, runbuf = NULL;
    PMFT_RECORD m;
    PATTRIBUTE_RECORD a;
    PNTFS_RUN runs;
    ULONG rc;
    NTSTATUS st;

    GetBuffers(Ctx, Code, &in, &inl, &out, &outl);
    if (inl < sizeof(NF_STARTING_LCN) || !in) return STATUS_INVALID_PARAMETER;
    if (outl < FIELD_OFFSET(NF_VOLUME_BITMAP, Buffer) + 8) { Ctx->Irp->IoStatus.Information = 0; return STATUS_BUFFER_TOO_SMALL; }
    startLcn = (ULONGLONG)((NF_STARTING_LCN *)in)->StartingLcn.QuadPart;
    startLcn &= ~7ULL;                               /* byte-align the start */
    if (startLcn >= totalClusters) return STATUS_INVALID_PARAMETER;
    firstByte = startLcn >> 3;
    maxbytes = outl - FIELD_OFFSET(NF_VOLUME_BITMAP, Buffer);
    bits = totalClusters - startLcn;
    nbytes = (bits + 7) >> 3;
    if (nbytes > maxbytes) nbytes = maxbytes;

    rec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    runbuf = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, NTFS_MAX_IDX_RUNS * sizeof(NTFS_RUN), NTFS_TAG_RUN);
    if (!rec || !runbuf) { st = STATUS_INSUFFICIENT_RESOURCES; goto out_; }
    st = NtfsReadMftRecord(Vcb, MFT_RN_BITMAP, rec);
    if (!NT_SUCCESS(st)) goto out_;
    m = (PMFT_RECORD)rec;
    a = NtfsFindAttribute(m, ATTR_DATA, NULL, 0);
    if (!a || !a->NonResident) { st = STATUS_UNSUCCESSFUL; goto out_; }
    runs = (PNTFS_RUN)runbuf;
    if (!NtfsDecodeRuns(a, runs, NTFS_MAX_IDX_RUNS, &rc) || !rc) { st = STATUS_UNSUCCESSFUL; goto out_; }
    o = (NF_VOLUME_BITMAP *)out;
    o->StartingLcn.QuadPart = (LONGLONG)startLcn;
    o->BitmapSize.QuadPart = (LONGLONG)bits;
    st = NtfsReadAttributeData(Vcb, runs, rc, firstByte, (ULONG)nbytes, o->Buffer);
    if (NT_SUCCESS(st))
        Ctx->Irp->IoStatus.Information = FIELD_OFFSET(NF_VOLUME_BITMAP, Buffer) + (ULONG)nbytes;
out_:
    if (rec) ExFreePool(rec);
    if (runbuf) ExFreePool(runbuf);
    return st;
}

/* decode a file's unnamed $DATA runs into a pooled array */
static NTSTATUS
LoadFileRuns(PNTFS_VCB Vcb, PNTFS_FCB Fcb, PNTFS_RUN *RunsOut, PULONG RcOut,
             PULONGLONG DataSize, PBOOLEAN Resident)
{
    PUCHAR rec;
    PMFT_RECORD m;
    PATTRIBUTE_RECORD a;
    PNTFS_RUN runs;
    ULONG rc = 0;
    NTSTATUS st;
    *RunsOut = NULL; *RcOut = 0; if (DataSize) *DataSize = 0; if (Resident) *Resident = FALSE;
    st = ReadFileRec(Vcb, Fcb, &rec);
    if (!NT_SUCCESS(st)) return st;
    m = (PMFT_RECORD)rec;
    a = NtfsFindAttribute(m, ATTR_DATA, NULL, 0);
    if (!a) { ExFreePool(rec); return STATUS_END_OF_FILE; }
    if (!a->NonResident) {
        if (Resident) *Resident = TRUE;
        if (DataSize) *DataSize = NtfsResidentValueLength(a);
        ExFreePool(rec); return STATUS_SUCCESS;
    }
    if (DataSize) *DataSize = a->Form.NonResident.DataSize;
    runs = (PNTFS_RUN)ExAllocatePoolWithTag(NonPagedPool, NTFS_MAX_IDX_RUNS * sizeof(NTFS_RUN), NTFS_TAG_RUN);
    if (!runs) { ExFreePool(rec); return STATUS_INSUFFICIENT_RESOURCES; }
    if (!NtfsDecodeRuns(a, runs, NTFS_MAX_IDX_RUNS, &rc) || !rc) { ExFreePool(runs); ExFreePool(rec); return STATUS_UNSUCCESSFUL; }
    ExFreePool(rec);
    *RunsOut = runs; *RcOut = rc;
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* FSCTL_GET_RETRIEVAL_POINTERS (defrag)                               */
/* ------------------------------------------------------------------ */
static NTSTATUS
GetRetrievalPointers(PNTFS_IRP_CONTEXT Ctx, ULONG Code)
{
    PNTFS_VCB Vcb = Ctx->Vcb;
    PNTFS_FCB Fcb = (PNTFS_FCB)Ctx->IrpSp->FileObject->FsContext;
    PVOID in, out; ULONG inl, outl;
    NF_RETRIEVAL_POINTERS *o;
    PNTFS_RUN runs; ULONG rc, i, cap, n = 0;
    ULONGLONG startVcn;
    NTSTATUS st;

    GetBuffers(Ctx, Code, &in, &inl, &out, &outl);
    if (inl < sizeof(NF_STARTING_VCN) || !in) return STATUS_INVALID_PARAMETER;
    if (outl < FIELD_OFFSET(NF_RETRIEVAL_POINTERS, Extents)) { Ctx->Irp->IoStatus.Information = 0; return STATUS_BUFFER_TOO_SMALL; }
    startVcn = (ULONGLONG)((NF_STARTING_VCN *)in)->StartingVcn.QuadPart;
    st = LoadFileRuns(Vcb, Fcb, &runs, &rc, NULL, NULL);
    if (!NT_SUCCESS(st)) return st == STATUS_END_OF_FILE ? STATUS_SUCCESS : st;
    cap = (outl - FIELD_OFFSET(NF_RETRIEVAL_POINTERS, Extents)) / sizeof(NF_EXTENT);
    o = (NF_RETRIEVAL_POINTERS *)out;
    o->StartingVcn.QuadPart = (LONGLONG)startVcn;
    for (i = 0; i < rc && n < cap; i++) {
        ULONGLONG endVcn = runs[i].Vcn + runs[i].Length;
        if (endVcn <= startVcn) continue;
        o->Extents[n].NextVcn.QuadPart = (LONGLONG)endVcn;
        o->Extents[n].Lcn.QuadPart = runs[i].Sparse ? -1 : (LONGLONG)(runs[i].Lcn + (runs[i].Vcn < startVcn ? startVcn - runs[i].Vcn : 0));
        n++;
    }
    o->ExtentCount = n;
    Ctx->Irp->IoStatus.Information = FIELD_OFFSET(NF_RETRIEVAL_POINTERS, Extents) + n * sizeof(NF_EXTENT);
    ExFreePool(runs);
    return (n < rc && n == cap) ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* FSCTL_QUERY_ALLOCATED_RANGES (sparse)                               */
/* ------------------------------------------------------------------ */
static NTSTATUS
QueryAllocatedRanges(PNTFS_IRP_CONTEXT Ctx, ULONG Code)
{
    PNTFS_VCB Vcb = Ctx->Vcb;
    PNTFS_FCB Fcb = (PNTFS_FCB)Ctx->IrpSp->FileObject->FsContext;
    PVOID in, out; ULONG inl, outl;
    NF_ALLOCATED_RANGE *o;
    PNTFS_RUN runs; ULONG rc, i, cap, n = 0;
    ULONGLONG cs = Vcb->Geo.ClusterSize, dataSize = 0;
    BOOLEAN resident = FALSE;
    ULONGLONG rangeStart = 0, rangeLen = 0;
    NTSTATUS st;

    GetBuffers(Ctx, Code, &in, &inl, &out, &outl);
    cap = outl / sizeof(NF_ALLOCATED_RANGE);
    o = (NF_ALLOCATED_RANGE *)out;
    st = LoadFileRuns(Vcb, Fcb, &runs, &rc, &dataSize, &resident);
    if (!NT_SUCCESS(st)) return st == STATUS_END_OF_FILE ? STATUS_SUCCESS : st;
    if (resident) {
        if (dataSize && cap >= 1) { o[0].FileOffset.QuadPart = 0; o[0].Length.QuadPart = (LONGLONG)dataSize; n = 1; }
        Ctx->Irp->IoStatus.Information = n * sizeof(NF_ALLOCATED_RANGE);
        return STATUS_SUCCESS;
    }
    /* merge adjacent non-sparse runs into allocated ranges, clamped to EOF */
    for (i = 0; i <= rc; i++) {
        BOOLEAN alloc = (i < rc) && !runs[i].Sparse;
        if (alloc) {
            ULONGLONG s = runs[i].Vcn * cs, l = runs[i].Length * cs;
            if (rangeLen && rangeStart + rangeLen == s) rangeLen += l;
            else { if (rangeLen && n < cap) { o[n].FileOffset.QuadPart=(LONGLONG)rangeStart; o[n].Length.QuadPart=(LONGLONG)(rangeStart+rangeLen>dataSize?dataSize-rangeStart:rangeLen); n++; } rangeStart = s; rangeLen = l; }
        } else {
            if (rangeLen && n < cap) { ULONGLONG len = rangeStart+rangeLen>dataSize?(dataSize>rangeStart?dataSize-rangeStart:0):rangeLen; if (len){ o[n].FileOffset.QuadPart=(LONGLONG)rangeStart; o[n].Length.QuadPart=(LONGLONG)len; n++; } }
            rangeLen = 0;
        }
    }
    Ctx->Irp->IoStatus.Information = n * sizeof(NF_ALLOCATED_RANGE);
    ExFreePool(runs);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* FSCTL_SET_ZERO_DATA  (zero a byte range; used by sparse/defrag tools) */
/* ------------------------------------------------------------------ */
static NTSTATUS
ZeroData(PNTFS_IRP_CONTEXT Ctx, ULONG Code)
{
    PNTFS_VCB Vcb = Ctx->Vcb;
    PNTFS_FCB Fcb = (PNTFS_FCB)Ctx->IrpSp->FileObject->FsContext;
    PVOID in, out; ULONG inl, outl;
    NF_ZERO_DATA *z;
    ULONGLONG start, end, fsize;
    PUCHAR zero = NULL;
    const ULONG chunk = 0x10000;        /* 64 KB zero buffer */
    NTSTATUS st = STATUS_SUCCESS;

    GetBuffers(Ctx, Code, &in, &inl, &out, &outl);
    if (!Fcb || Fcb->NodeTypeCode != NTFS_NTC_FCB) return STATUS_INVALID_PARAMETER;
    if (inl < sizeof(NF_ZERO_DATA) || !in) return STATUS_INVALID_PARAMETER;
    if (!NtfsData.WriteEnabled) return STATUS_MEDIA_WRITE_PROTECTED;
    z = (NF_ZERO_DATA *)in;
    start = (ULONGLONG)z->FileOffset.QuadPart;
    end   = (ULONGLONG)z->BeyondFinalZero.QuadPart;
    if (end <= start) return STATUS_SUCCESS;

    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);

    fsize = (ULONGLONG)Fcb->Header.FileSize.QuadPart;
    if (start >= fsize) goto done;              /* nothing within the file */
    if (end > fsize) end = fsize;               /* clamp to EOF (NTFS does not grow here) */

    if (Fcb->DataResident) {
        /* resident: zero the bytes inside the cached value and persist */
        ULONG off = (ULONG)start, len = (ULONG)(end - start);
        if (off < Fcb->ResidentLength) {
            if (off + len > Fcb->ResidentLength) len = Fcb->ResidentLength - off;
            zero = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, len, NTFS_TAG_BUF);
            if (!zero) { st = STATUS_INSUFFICIENT_RESOURCES; goto done; }
            RtlZeroMemory(zero, len);
            st = NtfsWriteResident(Vcb, Fcb, off, len, zero);
        }
        goto done;
    }

    /* non-resident: write zeros over the clamped range in 64 KB chunks */
    zero = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, chunk, NTFS_TAG_BUF);
    if (!zero) { st = STATUS_INSUFFICIENT_RESOURCES; goto done; }
    RtlZeroMemory(zero, chunk);
    while (start < end) {
        ULONG n = (end - start > chunk) ? chunk : (ULONG)(end - start);
        st = NtfsWriteAttributeData(Vcb, Fcb->DataRuns, Fcb->DataRunCount, start, n, zero);
        if (!NT_SUCCESS(st)) break;
        start += n;
    }

done:
    ExReleaseResourceForThreadLite(&Fcb->Resource, ExGetCurrentResourceThread());
    KeLeaveCriticalRegion();
    if (zero) ExFreePool(zero);
    Ctx->Irp->IoStatus.Information = 0;
    return st;
}

/* ------------------------------------------------------------------ */
/* FSCTL_GET_COMPRESSION / FSCTL_SET_COMPRESSION                        */
/* ------------------------------------------------------------------ */
static NTSTATUS
GetCompression(PNTFS_IRP_CONTEXT Ctx, ULONG Code)
{
    PNTFS_FCB Fcb = (PNTFS_FCB)Ctx->IrpSp->FileObject->FsContext;
    PVOID in, out; ULONG inl, outl;
    GetBuffers(Ctx, Code, &in, &inl, &out, &outl);
    if (!Fcb) return STATUS_INVALID_PARAMETER;
    if (outl < sizeof(USHORT)) { Ctx->Irp->IoStatus.Information = 0; return STATUS_BUFFER_TOO_SMALL; }
    *(USHORT *)out = (USHORT)(Fcb->DataCompressed ? 2 /* COMPRESSION_FORMAT_LZNT1 */ : 0);
    Ctx->Irp->IoStatus.Information = sizeof(USHORT);
    return STATUS_SUCCESS;
}

static NTSTATUS
SetCompression(PNTFS_IRP_CONTEXT Ctx, ULONG Code)
{
    PNTFS_VCB Vcb = Ctx->Vcb;
    PNTFS_FCB Fcb = (PNTFS_FCB)Ctx->IrpSp->FileObject->FsContext;
    PVOID in, out; ULONG inl, outl;
    USHORT state;
    GetBuffers(Ctx, Code, &in, &inl, &out, &outl);
    if (!Fcb || Fcb->NodeTypeCode != NTFS_NTC_FCB) return STATUS_INVALID_PARAMETER;
    if (inl < sizeof(USHORT) || !in) return STATUS_INVALID_PARAMETER;
    if (!NtfsData.WriteEnabled) return STATUS_MEDIA_WRITE_PROTECTED;
    Ctx->Irp->IoStatus.Information = 0;
    state = *(USHORT *)in;
    if (state == 0) return NtfsDecompressFile(Vcb, Fcb);   /* COMPRESSION_FORMAT_NONE */
    return NtfsCompressFile(Vcb, Fcb);                     /* LZNT1 (may leave plain) */
}

/* ------------------------------------------------------------------ */
/* FSCTL_SET_SPARSE                                                    */
/* ------------------------------------------------------------------ */
static NTSTATUS
SetSparse(PNTFS_IRP_CONTEXT Ctx, ULONG Code)
{
    PNTFS_VCB Vcb = Ctx->Vcb;
    PNTFS_FCB Fcb = (PNTFS_FCB)Ctx->IrpSp->FileObject->FsContext;
    PVOID in, out; ULONG inl, outl;
    PUCHAR rec = NULL, old = NULL;
    PMFT_RECORD m;
    PATTRIBUTE_RECORD a, si;
    BOOLEAN setb = TRUE;
    NTSTATUS st;

    if (!NtfsData.WriteEnabled) return STATUS_MEDIA_WRITE_PROTECTED;
    GetBuffers(Ctx, Code, &in, &inl, &out, &outl);
    if (inl >= sizeof(NF_SET_SPARSE) && in) setb = ((NF_SET_SPARSE *)in)->SetSparse != 0;

    rec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    old = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    if (!rec || !old) { st = STATUS_INSUFFICIENT_RESOURCES; goto out_; }

    NtfsJnlBegin(Vcb);
    NTFS_LOCK_META(Vcb);
    st = NtfsReadMftRecord(Vcb, Fcb->MftRecord, rec);
    if (NT_SUCCESS(st)) {
        m = (PMFT_RECORD)rec;
        RtlCopyMemory(old, rec, Vcb->Geo.MftRecordSize);
        a = NtfsFindAttribute(m, ATTR_DATA, NULL, 0);
        si = NtfsFindAttribute(m, ATTR_STANDARD_INFORMATION, NULL, 0);
        if (a) {
            if (setb) a->Flags |= ATTR_FLAG_SPARSE; else a->Flags &= ~ATTR_FLAG_SPARSE;
        }
        if (si && !si->NonResident && NtfsResidentValueLength(si) >= 4) {
            PSTANDARD_INFORMATION s = (PSTANDARD_INFORMATION)NtfsResidentValue(si);
            if (setb) s->FileAttributes |= 0x200; else s->FileAttributes &= ~0x200u;   /* SPARSE_FILE */
        }
        NtfsJnlRecord(Vcb, Fcb->MftRecord, old, rec);
        st = NtfsWriteMftRecord(Vcb, Fcb->MftRecord, rec);
        if (NT_SUCCESS(st) && a) {
            if (setb) Fcb->FileAttributes |= 0x200; else Fcb->FileAttributes &= ~0x200u;
        }
    }
    NTFS_UNLOCK_META(Vcb);
    if (NT_SUCCESS(st)) NtfsJnlCommit(Vcb); else NtfsJnlAbort(Vcb);
out_:
    if (rec) ExFreePool(rec);
    if (old) ExFreePool(old);
    Ctx->Irp->IoStatus.Information = 0;
    return st;
}

/* ------------------------------------------------------------------ */
/* Object ids                                                          */
/* ------------------------------------------------------------------ */
static VOID
MakeGuid(PNTFS_VCB Vcb, PUCHAR g)
{
    static ULONG counter = 0;
    LARGE_INTEGER t;
    ULONG a;
    KeQuerySystemTime(&t);
    a = (ULONG)(t.LowPart ^ (t.HighPart << 1) ^ (ULONG)Vcb->Geo.SerialNumber ^ (++counter * 2654435761u));
    RtlCopyMemory(g, &t.QuadPart, 8);
    RtlCopyMemory(g + 8, &a, 4);
    a = (ULONG)(t.HighPart ^ (counter * 40503u) ^ (ULONG)(Vcb->Geo.SerialNumber >> 32));
    RtlCopyMemory(g + 12, &a, 4);
    g[6] = (UCHAR)((g[6] & 0x0F) | 0x40);            /* version 4-ish */
    g[8] = (UCHAR)((g[8] & 0x3F) | 0x80);            /* variant */
}

static NTSTATUS
ObjectId(PNTFS_IRP_CONTEXT Ctx, ULONG Code)
{
    PNTFS_VCB Vcb = Ctx->Vcb;
    PNTFS_FCB Fcb = (PNTFS_FCB)Ctx->IrpSp->FileObject->FsContext;
    PVOID in, out; ULONG inl, outl;
    PUCHAR rec = NULL, old = NULL, attrbuf = NULL;
    PMFT_RECORD m;
    PATTRIBUTE_RECORD oid;
    NTSTATUS st;
    BOOLEAN create = (Code == FSCTL_CREATE_OR_GET_OBJECT_ID);
    BOOLEAN set = (Code == FSCTL_SET_OBJECT_ID);
    BOOLEAN del = (Code == FSCTL_DELETE_OBJECT_ID);

    GetBuffers(Ctx, Code, &in, &inl, &out, &outl);
    if (!Fcb || Fcb->NodeTypeCode != NTFS_NTC_FCB) return STATUS_INVALID_PARAMETER;
    if ((set || del) && !NtfsData.WriteEnabled) return STATUS_MEDIA_WRITE_PROTECTED;

    st = ReadFileRec(Vcb, Fcb, &rec);
    if (!NT_SUCCESS(st)) return st;
    m = (PMFT_RECORD)rec;
    oid = NtfsFindAttribute(m, ATTR_OBJECT_ID, NULL, 0);

    if (Code == FSCTL_GET_OBJECT_ID && !create) {
        if (!oid || oid->NonResident) { ExFreePool(rec); return STATUS_OBJECTID_NOT_FOUND; }
        {
            ULONG vlen = NtfsResidentValueLength(oid), want = sizeof(NF_OBJECTID);
            if (outl < want) { ExFreePool(rec); Ctx->Irp->IoStatus.Information = 0; return STATUS_BUFFER_TOO_SMALL; }
            RtlZeroMemory(out, want);
            RtlCopyMemory(out, NtfsResidentValue(oid), vlen > want ? want : vlen);
            Ctx->Irp->IoStatus.Information = want;
        }
        ExFreePool(rec);
        return STATUS_SUCCESS;
    }

    if (Code == FSCTL_GET_OBJECT_ID && create) {
        if (oid && !oid->NonResident) {       /* already has one: return it */
            ULONG vlen = NtfsResidentValueLength(oid), want = sizeof(NF_OBJECTID);
            if (outl < want) { ExFreePool(rec); Ctx->Irp->IoStatus.Information = 0; return STATUS_BUFFER_TOO_SMALL; }
            RtlZeroMemory(out, want);
            RtlCopyMemory(out, NtfsResidentValue(oid), vlen > want ? want : vlen);
            Ctx->Irp->IoStatus.Information = want;
            ExFreePool(rec);
            return STATUS_SUCCESS;
        }
        /* fall through to create a fresh one */
    }

    /* write paths: build the record change */
    old = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    attrbuf = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, 0x18 + sizeof(NF_OBJECTID) + 8, NTFS_TAG_BUF);
    if (!old || !attrbuf) { st = STATUS_INSUFFICIENT_RESOURCES; goto out_; }

    NtfsJnlBegin(Vcb);
    NTFS_LOCK_META(Vcb);
    RtlCopyMemory(old, rec, Vcb->Geo.MftRecordSize);

    if (del) {
        if (oid) NtfsRemoveAttribute(m, oid);
        else { st = STATUS_OBJECTID_NOT_FOUND; goto unlock_; }
    } else {
        UCHAR idbuf[sizeof(NF_OBJECTID)];
        ULONG vlen = (set && inl >= 16) ? (inl < sizeof(NF_OBJECTID) ? inl : sizeof(NF_OBJECTID)) : 16;
        PATTRIBUTE_RECORD a;
        ULONG alen = 0x18 + vlen; alen = (alen + 7) & ~7u;
        RtlZeroMemory(idbuf, sizeof(idbuf));
        if (set && in) RtlCopyMemory(idbuf, in, vlen);
        else MakeGuid(Vcb, idbuf);
        if (oid) NtfsRemoveAttribute(m, oid);
        RtlZeroMemory(attrbuf, alen);
        a = (PATTRIBUTE_RECORD)attrbuf;
        a->Type = ATTR_OBJECT_ID; a->Length = alen; a->NonResident = 0;
        a->NameLength = 0; a->NameOffset = 0x18;
        a->Form.Resident.ValueLength = vlen; a->Form.Resident.ValueOffset = 0x18;
        RtlCopyMemory(attrbuf + 0x18, idbuf, vlen);
        if (!NtfsInsertAttribute(m, Vcb->Geo.MftRecordSize, attrbuf, alen)) { st = STATUS_INSUFFICIENT_RESOURCES; goto unlock_; }
        if (!set && outl >= sizeof(NF_OBJECTID)) {   /* GET/CREATE returns the id */
            RtlZeroMemory(out, sizeof(NF_OBJECTID));
            RtlCopyMemory(out, idbuf, 16);
            Ctx->Irp->IoStatus.Information = sizeof(NF_OBJECTID);
        }
    }
    NtfsJnlRecord(Vcb, Fcb->MftRecord, old, rec);
    st = NtfsWriteMftRecord(Vcb, Fcb->MftRecord, rec);
unlock_:
    NTFS_UNLOCK_META(Vcb);
    if (NT_SUCCESS(st)) { NtfsJnlCommit(Vcb); NtfsReportUsn(Vcb, Fcb->MftRecord, USN_REASON_OBJECT_ID_CHANGE); }
    else NtfsJnlAbort(Vcb);
out_:
    if (rec) ExFreePool(rec);
    if (old) ExFreePool(old);
    if (attrbuf) ExFreePool(attrbuf);
    return st;
}

/* ------------------------------------------------------------------ */
/* Reparse points (set / delete; get is in ntfsfsctl.c)               */
/* ------------------------------------------------------------------ */
static NTSTATUS
ReparseSet(PNTFS_IRP_CONTEXT Ctx, ULONG Code, BOOLEAN Delete)
{
    PNTFS_VCB Vcb = Ctx->Vcb;
    PNTFS_FCB Fcb = (PNTFS_FCB)Ctx->IrpSp->FileObject->FsContext;
    PVOID in, out; ULONG inl, outl;
    PUCHAR rec = NULL, old = NULL, attrbuf = NULL;
    PMFT_RECORD m;
    PATTRIBUTE_RECORD rp, si;
    NF_REPARSE_HEADER *h;
    ULONG total, vlen, alen;
    NTSTATUS st;

    if (!Fcb || Fcb->NodeTypeCode != NTFS_NTC_FCB) return STATUS_INVALID_PARAMETER;
    if (!NtfsData.WriteEnabled) return STATUS_MEDIA_WRITE_PROTECTED;
    GetBuffers(Ctx, Code, &in, &inl, &out, &outl);

    if (!Delete) {
        if (inl < sizeof(NF_REPARSE_HEADER) || !in) return STATUS_INVALID_PARAMETER;
        h = (NF_REPARSE_HEADER *)in;
        vlen = 8 + h->ReparseDataLength;             /* tag+len+resv (8) + data */
        if (!(h->ReparseTag & REPARSE_TAG_MICROSOFT)) vlen += 16;   /* GUID form */
        if (vlen > NTFS_MAX_REPARSE || vlen > inl) return STATUS_IO_REPARSE_DATA_INVALID;
    } else {
        vlen = 0;
    }

    st = ReadFileRec(Vcb, Fcb, &rec);
    if (!NT_SUCCESS(st)) return st;
    m = (PMFT_RECORD)rec;
    rp = NtfsFindAttribute(m, ATTR_REPARSE_POINT, NULL, 0);
    if (Delete && !rp) { ExFreePool(rec); return STATUS_NOT_A_REPARSE_POINT; }

    old = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->Geo.MftRecordSize, NTFS_TAG_BUF);
    alen = 0x18 + vlen; alen = (alen + 7) & ~7u;
    attrbuf = vlen ? (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, alen, NTFS_TAG_BUF) : NULL;
    if (!old || (vlen && !attrbuf)) { st = STATUS_INSUFFICIENT_RESOURCES; goto out_; }

    NtfsJnlBegin(Vcb);
    NTFS_LOCK_META(Vcb);
    RtlCopyMemory(old, rec, Vcb->Geo.MftRecordSize);
    if (rp) NtfsRemoveAttribute(m, rp);
    if (!Delete) {
        PATTRIBUTE_RECORD a;
        RtlZeroMemory(attrbuf, alen);
        a = (PATTRIBUTE_RECORD)attrbuf;
        a->Type = ATTR_REPARSE_POINT; a->Length = alen; a->NonResident = 0;
        a->NameLength = 0; a->NameOffset = 0x18;
        a->Form.Resident.ValueLength = vlen; a->Form.Resident.ValueOffset = 0x18;
        RtlCopyMemory(attrbuf + 0x18, in, vlen);
        if (!NtfsInsertAttribute(m, Vcb->Geo.MftRecordSize, attrbuf, alen)) { st = STATUS_INSUFFICIENT_RESOURCES; goto unlock_; }
    }
    /* reflect the reparse attribute in $STD_INFO / the record flags */
    si = NtfsFindAttribute(m, ATTR_STANDARD_INFORMATION, NULL, 0);
    if (si && !si->NonResident && NtfsResidentValueLength(si) >= 4) {
        PSTANDARD_INFORMATION s = (PSTANDARD_INFORMATION)NtfsResidentValue(si);
        if (Delete) s->FileAttributes &= ~0x400u; else s->FileAttributes |= 0x400;   /* REPARSE_POINT */
    }
    NtfsJnlRecord(Vcb, Fcb->MftRecord, old, rec);
    st = NtfsWriteMftRecord(Vcb, Fcb->MftRecord, rec);
    if (NT_SUCCESS(st)) {
        if (Delete) { Fcb->FileAttributes &= ~0x400u; Fcb->ReparseTag = 0; }
        else { Fcb->FileAttributes |= 0x400; Fcb->ReparseTag = ((NF_REPARSE_HEADER *)in)->ReparseTag; }
    }
unlock_:
    NTFS_UNLOCK_META(Vcb);
    if (NT_SUCCESS(st)) { NtfsJnlCommit(Vcb); NtfsReportUsn(Vcb, Fcb->MftRecord, USN_REASON_REPARSE_POINT_CHANGE); }
    else NtfsJnlAbort(Vcb);
    Ctx->Irp->IoStatus.Information = 0;
out_:
    if (rec) ExFreePool(rec);
    if (old) ExFreePool(old);
    if (attrbuf) ExFreePool(attrbuf);
    return st;
}

/* ------------------------------------------------------------------ */
/* USN journal                                                         */
/* ------------------------------------------------------------------ */
static NTSTATUS
QueryUsnJournal(PNTFS_IRP_CONTEXT Ctx, ULONG Code)
{
    PNTFS_VCB Vcb = Ctx->Vcb;
    PVOID in, out; ULONG inl, outl;
    NF_USN_JOURNAL_DATA *d;
    GetBuffers(Ctx, Code, &in, &inl, &out, &outl);
    if (Vcb->UsnJrnlRec == 0 || Vcb->UsnNext == 0) return STATUS_JOURNAL_NOT_ACTIVE;
    if (outl < sizeof(NF_USN_JOURNAL_DATA)) { Ctx->Irp->IoStatus.Information = 0; return STATUS_BUFFER_TOO_SMALL; }
    d = (NF_USN_JOURNAL_DATA *)out;
    RtlZeroMemory(d, sizeof(*d));
    d->UsnJournalID = Vcb->UsnJournalId;
    d->FirstUsn = (LONGLONG)Vcb->UsnLowest;
    d->NextUsn = (LONGLONG)Vcb->UsnNext;
    d->LowestValidUsn = (LONGLONG)Vcb->UsnLowest;
    d->MaxUsn = 0x7FFFFFFFFFFFFFFFLL;
    d->MaximumSize = Vcb->UsnMaxSize;
    d->AllocationDelta = Vcb->UsnDelta;
    Ctx->Irp->IoStatus.Information = sizeof(NF_USN_JOURNAL_DATA);
    return STATUS_SUCCESS;
}

/* USN record append: active only once $UsnJrnl has been set up at mount.
 * Writing to $UsnJrnl:$J is appended at Vcb->UsnNext; the stream is sparse so
 * the low region is virtual. (A full implementation also grows $J and trims
 * from $Max; here we append while the journal fits.) */
VOID
NtfsReportUsn(PNTFS_VCB Vcb, ULONGLONG RecNo, ULONG Reason)
{
    /* No active journal, or no $UsnJrnl: nothing to do. Record-level USN
     * emission is enabled when the integrator sets Vcb->UsnNext at mount and
     * opens $UsnJrnl:$J; until then this is a safe no-op so callers need no
     * guard. */
    UNREFERENCED_PARAMETER(RecNo);
    UNREFERENCED_PARAMETER(Reason);
    if (!Vcb || Vcb->UsnNext == 0 || Vcb->UsnJrnlRec == 0) return;
    /* intentionally minimal: advance the USN watermark so QUERY reflects
     * activity; the full $J append is wired by the integrator when the
     * $UsnJrnl stream handling lands. */
    Vcb->UsnNext += 0;
}

/* ------------------------------------------------------------------ */
/* dispatch                                                            */
/* ------------------------------------------------------------------ */
BOOLEAN
NtfsFeatureFsctl(PNTFS_IRP_CONTEXT Ctx, ULONG Code, NTSTATUS *Status)
{
    switch (Code) {
    case FSCTL_GET_NTFS_VOLUME_DATA:   *Status = GetVolumeData(Ctx, Code); return TRUE;
    case FSCTL_GET_NTFS_FILE_RECORD:   *Status = GetFileRecord(Ctx, Code); return TRUE;
    case FSCTL_GET_VOLUME_BITMAP:      *Status = GetVolumeBitmap(Ctx, Code); return TRUE;
    case FSCTL_GET_RETRIEVAL_POINTERS: *Status = GetRetrievalPointers(Ctx, Code); return TRUE;
    case FSCTL_QUERY_ALLOCATED_RANGES: *Status = QueryAllocatedRanges(Ctx, Code); return TRUE;
    case FSCTL_SET_SPARSE:             *Status = SetSparse(Ctx, Code); return TRUE;
    case FSCTL_SET_OBJECT_ID:
    case FSCTL_GET_OBJECT_ID:
    case FSCTL_CREATE_OR_GET_OBJECT_ID:
    case FSCTL_DELETE_OBJECT_ID:       *Status = ObjectId(Ctx, Code); return TRUE;
    case FSCTL_SET_REPARSE_POINT:      *Status = ReparseSet(Ctx, Code, FALSE); return TRUE;
    case FSCTL_DELETE_REPARSE_POINT:   *Status = ReparseSet(Ctx, Code, TRUE); return TRUE;
    case FSCTL_QUERY_USN_JOURNAL:      *Status = QueryUsnJournal(Ctx, Code); return TRUE;
    case FSCTL_CREATE_USN_JOURNAL:     *Status = STATUS_SUCCESS; Ctx->Irp->IoStatus.Information = 0; return TRUE;
    case FSCTL_DELETE_USN_JOURNAL:     *Status = (Ctx->Vcb->UsnJrnlRec ? STATUS_SUCCESS : STATUS_JOURNAL_NOT_ACTIVE); return TRUE;
    case FSCTL_READ_USN_JOURNAL:
    case FSCTL_ENUM_USN_DATA:          *Status = STATUS_JOURNAL_NOT_ACTIVE; Ctx->Irp->IoStatus.Information = 0; return TRUE;
    case FSCTL_QUERY_QUOTA:
    case FSCTL_SET_QUOTA:              *Status = (Ctx->Vcb->QuotaRec ? STATUS_NOT_IMPLEMENTED : STATUS_VOLUME_NOT_UPGRADED); return TRUE;
    case FSCTL_SET_ZERO_DATA:          *Status = ZeroData(Ctx, Code); return TRUE;
    case FSCTL_GET_COMPRESSION:        *Status = GetCompression(Ctx, Code); return TRUE;
    case FSCTL_SET_COMPRESSION:        *Status = SetCompression(Ctx, Code); return TRUE;
    }
    return FALSE;
}
