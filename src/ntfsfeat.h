/*
 * NTFS3NT - feature FSCTLs (sparse, object ids, reparse set/delete, volume
 * bitmap and retrieval pointers, volume/file-record queries, USN journal and
 * quota entry points). Implemented in ntfsfeat.c; the dispatcher calls
 * NtfsFeatureFsctl from the IRP_MN_USER_FS_REQUEST path.
 */

#ifndef _NTFSFEAT_H_
#define _NTFSFEAT_H_

/* Handle an FSCTL. Returns TRUE if Code was ours (then *Status and
 * Ctx->Irp->IoStatus.Information are set); FALSE to let the caller try others. */
BOOLEAN NtfsFeatureFsctl(PNTFS_IRP_CONTEXT Ctx, ULONG Code, NTSTATUS *Status);

/* Append a USN change record for RecNo to $UsnJrnl:$J, if the journal is
 * active (Vcb->UsnNext != 0); a no-op otherwise. */
VOID    NtfsReportUsn(PNTFS_VCB Vcb, ULONGLONG RecNo, ULONG Reason);

/* USN change reasons (subset; Windows-compatible values). */
#ifndef USN_REASON_DATA_OVERWRITE
#define USN_REASON_DATA_OVERWRITE        0x00000001
#define USN_REASON_DATA_EXTEND           0x00000002
#define USN_REASON_DATA_TRUNCATION       0x00000004
#define USN_REASON_FILE_CREATE           0x00000100
#define USN_REASON_FILE_DELETE           0x00000200
#define USN_REASON_EA_CHANGE             0x00000400
#define USN_REASON_SECURITY_CHANGE       0x00000800
#define USN_REASON_RENAME_OLD_NAME       0x00001000
#define USN_REASON_RENAME_NEW_NAME       0x00002000
#define USN_REASON_BASIC_INFO_CHANGE     0x00008000
#define USN_REASON_HARD_LINK_CHANGE      0x00010000
#define USN_REASON_REPARSE_POINT_CHANGE  0x00100000
#define USN_REASON_OBJECT_ID_CHANGE      0x00080000
#define USN_REASON_CLOSE                 0x80000000
#endif

#endif /* _NTFSFEAT_H_ */
