#pragma once

#define IOCTL_LookupProcessByProcessId	CTL_CODE(FILE_DEVICE_UNKNOWN, 0, METHOD_NEITHER, FILE_ANY_ACCESS)
#define IOCTL_LookupThreadByThreadId	CTL_CODE(FILE_DEVICE_UNKNOWN, 1, METHOD_NEITHER, FILE_ANY_ACCESS)
#define IOCTL_ReadMemory				CTL_CODE(FILE_DEVICE_UNKNOWN, 2, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)
#define IOCTL_QueryMemory				CTL_CODE(FILE_DEVICE_UNKNOWN, 3, METHOD_NEITHER, FILE_ANY_ACCESS)
#define IOCTL_QueryHandles				CTL_CODE(FILE_DEVICE_UNKNOWN, 4, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)
#define IOCTL_OpenProcess				CTL_CODE(FILE_DEVICE_UNKNOWN, 5, METHOD_NEITHER, FILE_ANY_ACCESS)
#define IOCTL_SetProtectedProcess		CTL_CODE(FILE_DEVICE_UNKNOWN, 6, METHOD_NEITHER, FILE_ANY_ACCESS)
#define IOCTL_DelProtectedProcess		CTL_CODE(FILE_DEVICE_UNKNOWN, 7, METHOD_NEITHER, FILE_ANY_ACCESS)
#define IOCTL_QueryProtectedProcess		CTL_CODE(FILE_DEVICE_UNKNOWN, 8, METHOD_NEITHER, FILE_ANY_ACCESS)
#define IOCTL_OpenThread				CTL_CODE(FILE_DEVICE_UNKNOWN, 9, METHOD_NEITHER, FILE_ANY_ACCESS)

struct FQH 
{
	ULONG_PTR ProcessId;
	BYTE ThreadIndex, ProcessIndex, FileIndex;
};