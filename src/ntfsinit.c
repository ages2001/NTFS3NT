/*
 * NTFS3NT - DriverEntry and driver registration.
 */

#define NTFS_KERNEL
#include "ntfs.h"

NTFS_DATA NtfsData;

#define NTFS_FS_DEVICE_NAME  L"\\Ntfs"

NTSTATUS NTAPI
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING Name;
    PDEVICE_OBJECT DeviceObject;
    NTSTATUS Status;
    ULONG i;

    UNREFERENCED_PARAMETER(RegistryPath);

#ifdef NTFS_NT31
    if (!NtfsResolveResourceApi()) {
        DBGP("[NTFS3NT: resource API not found]\n");
        return STATUS_PROCEDURE_NOT_FOUND;
    }
#endif

    RtlZeroMemory(&NtfsData, sizeof(NtfsData));
    NtfsData.NodeTypeCode = NTFS_NTC_DATA;
    NtfsData.NodeByteSize = sizeof(NTFS_DATA);
    NtfsData.DriverObject = DriverObject;
    InitializeListHead(&NtfsData.VcbList);
    ExInitializeResourceLite(&NtfsData.Resource);
    NtfsData.WriteEnabled = TRUE;    /* write path: in-place + extend existing files */

    RtlInitUnicodeString(&Name, NTFS_FS_DEVICE_NAME);
    Status = IoCreateDevice(DriverObject, 0, &Name, FILE_DEVICE_DISK_FILE_SYSTEM,
                            0, FALSE, &DeviceObject);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    NtfsData.FileSystemDeviceObject = DeviceObject;

    /* NT 3.1's DRIVER_OBJECT.MajorFunction[] is smaller than later headers'
     * IRP_MJ_MAXIMUM_FUNCTION, so fill only through IRP_MJ_CLEANUP. */
#ifdef NTFS_NT31
    for (i = 0; i <= IRP_MJ_CLEANUP; i++) {
        DriverObject->MajorFunction[i] = NtfsFsdDispatch;
    }
#else
    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        DriverObject->MajorFunction[i] = NtfsFsdDispatch;
    }
#endif

#ifdef DO_DEVICE_INITIALIZING
    DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
#endif

    /* Cache Manager participation (fast I/O + callbacks) - required by NT 5
     * (Windows 2000/XP) for cached file reads; harmless on NT 3.1 - 4.0. */
    NtfsInitCaching(DriverObject);

    IoRegisterFileSystem(DeviceObject);
    DBGP("[NTFS3NT: loaded]\n");
    return STATUS_SUCCESS;
}
