#include "StdAfx.h"

_NT_BEGIN

#include "../inc/initterm.h"
#include "io.h"

//////////////////////////////////////////////////////////////////////////
//

PKQUEUE g_Queue;

PLARGE_INTEGER g_ptimeout = &g_timeout;

EXTERN_C_START

void CALLBACK ThreadStartThunk(void* );

LARGE_INTEGER g_alttimeout{};

void NTAPI altOnTimeout()
{
}

EXTERN_C_END

#ifdef _WIN64
#pragma comment(linker, "/alternatename:g_timeout=g_alttimeout")
#pragma comment(linker, "/alternatename:OnTimeout=altOnTimeout")
#else
#pragma comment(linker, "/alternatename:_g_timeout=_g_alttimeout")
#pragma comment(linker, "/alternatename:_OnTimeout@0=_altOnTimeout@0")
#endif

struct MY_IRP_CONTEXT
{
	CDataPacket* _packet;
	PVOID _pv;
	ULONG _code;

	enum { tag = 'xtci'};

	void* operator new(size_t cb)
	{
		return ExAllocatePoolWithTag(PagedPool, cb, tag);
	}

	void operator delete(PVOID pv)
	{
		ExFreePoolWithTag(pv, tag);
	}

	MY_IRP_CONTEXT(CDataPacket* packet, PVOID pv, ULONG code) : _packet(packet), _pv(pv), _code(code)
	{
		DbgPrint("%s<%p>\n", __FUNCTION__, this);
		if (packet)
		{
			packet->AddRef();
		}
	}

	~MY_IRP_CONTEXT()
	{
		if (_packet)
		{
			_packet->Release();
		}
		DbgPrint("%s<%p>\n", __FUNCTION__, this);
	}
};

PIRP NTAPI IO_OBJECT::BuildSynchronousFsdRequest(
	ULONG  MajorFunction,
	PDEVICE_OBJECT  DeviceObject,
	PVOID  Buffer,
	ULONG  Length,
	PLARGE_INTEGER  StartingOffset,
	ULONG code, 
	CDataPacket* packet, 
	PVOID Pointer
	)
{
	if (MY_IRP_CONTEXT* Context = new MY_IRP_CONTEXT(packet, Pointer, code))
	{
		if (PIRP Irp = IoBuildSynchronousFsdRequest(MajorFunction, DeviceObject, Buffer, Length,
			StartingOffset, 0, 0))
		{
			Irp->Overlay.AsynchronousParameters.UserApcContext = Context;

			return Irp;
		}

		delete Context;
	}

	return NULL;
}

PIRP NTAPI IO_OBJECT::BuildDeviceIoControlRequest(
	ULONG  IoControlCode,
	PDEVICE_OBJECT  DeviceObject,
	PVOID  InputBuffer,
	ULONG  InputBufferLength,
	PVOID  OutputBuffer,
	ULONG  OutputBufferLength,
	BOOLEAN  InternalDeviceIoControl, 
	ULONG code, 
	CDataPacket* packet, 
	PVOID Pointer
	)
{
	if (MY_IRP_CONTEXT* Context = new MY_IRP_CONTEXT(packet, Pointer, code))
	{
		if (PIRP Irp = IoBuildDeviceIoControlRequest(IoControlCode, DeviceObject, InputBuffer, InputBufferLength,
			OutputBuffer, OutputBufferLength, InternalDeviceIoControl, 0, 0))
		{
			Irp->Overlay.AsynchronousParameters.UserApcContext = Context;

			return Irp;
		}

		delete Context;
	}

	return NULL;
}

void IO_OBJECT::OnComplete(PIRP Irp)
{
	MY_IRP_CONTEXT* Context = (MY_IRP_CONTEXT*)Irp->Overlay.AsynchronousParameters.UserApcContext;

	DbgPrint("%s<%p>(irp=%p, packet=%p, code=%.4s, pv=%p)[%x, %x]\n", __FUNCTION__, this, Irp, Context->_packet, &Context->_code, Context->_pv, Irp->IoStatus.Status, (ULONG)Irp->IoStatus.Information);

	IOCompletionRoutine(Context->_packet, Context->_code, 
		Irp->IoStatus.Status, Irp->IoStatus.Information, Context->_pv);

	IoFreeIrp(Irp);

	delete Context;

	Release();
}

void IO_OBJECT::PrepareIrp(PIRP Irp)
{
	Irp->UserIosb = &Irp->IoStatus;

	// will be dereferenced in IopCompleteRequest
	PFILE_OBJECT FileObject = m_FileObject;
	Irp->Tail.Overlay.OriginalFileObject = FileObject, ObfReferenceObject(FileObject);

	PIO_STACK_LOCATION IrpSp = IoGetNextIrpStackLocation(Irp);

	IrpSp->Control = 0;

	IrpSp->FileObject = FileObject;

	AddRef();
}

NTSTATUS IO_OBJECT::SendIrp(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	MY_IRP_CONTEXT* Context = (MY_IRP_CONTEXT*)Irp->Overlay.AsynchronousParameters.UserApcContext;
	
	DbgPrint("%s<%p>(irp=%p, packet=%p, code=%.4s, pv=%p)\n", __FUNCTION__, this, Irp, Context->_packet, &Context->_code, Context->_pv);
	
	PrepareIrp(Irp);

	NTSTATUS status = IofCallDriver(DeviceObject, Irp);
	
	DbgPrint("SendIrp(%p)=%x\n", Irp, status);

	if (NT_ERROR(status))
	{
		IOCompletionRoutine(Context->_packet, Context->_code, status, 0, Context->_pv);

		delete Context;

		Release();
	}

	return status;
}

#pragma push_macro("DbgPrint")
#undef DbgPrint

void IoThread()
{
	// only single iothread use g_ptimeout
	PLARGE_INTEGER ptimeout = (PLARGE_INTEGER)InterlockedExchangePointerNoFence((void**)&g_ptimeout, 0);

	if (ptimeout && !ptimeout->QuadPart)
	{
		ptimeout = 0;
	}

	DbgPrint("IoThread(%p) begin(%p)\n", KeGetCurrentThread(), ptimeout);//$$$

	for (;;)
	{
		union {
			PLIST_ENTRY entry;
			PIRP Irp;
			PBYTE pb;
		};

		switch((ULONG_PTR)(entry = KeRemoveQueue(g_Queue, UserMode, ptimeout)))
		{
		case STATUS_TIMEOUT:
			OnTimeout();
		case STATUS_USER_APC:
		case STATUS_ALERTED:
			continue;
		case STATUS_ABANDONED:
			DbgPrint("IoThread(%p) end\n", KeGetCurrentThread());//$$$
			ObfDereferenceObject(g_Queue);
			return;
		}

		// Tail.Overlay.ListEntry - used to queue the IRP to completion queue
		pb -= FIELD_OFFSET(IRP, Tail.Overlay.ListEntry);

		reinterpret_cast<IO_OBJECT*>(Irp->Tail.CompletionKey)->OnComplete(Irp);
	}
}

NTSTATUS InitIo(PDRIVER_OBJECT DriverObject)
{
	initterm();

	HANDLE hIOCP;
	NTSTATUS status = ZwCreateIoCompletion(&hIOCP, IO_COMPLETION_ALL_ACCESS, 0, 0);
	if (0 > status)
	{
		return status;
	}

	status = ObReferenceObjectByHandle(hIOCP, 0, 0, KernelMode, (void**)&g_Queue, 0);

	NtClose(hIOCP);

	if (0 > status)
	{
		return status;
	}

	KeInitializeQueue(g_Queue, 0);

	ULONG Count = g_Queue->MaximumCount, nIoThreads = 0;

	DbgPrint("g_Queue=%p, %x\n", g_Queue, Count);

	g_DriverObject = DriverObject;	

	do 
	{
		ObfReferenceObject(DriverObject);
		ObfReferenceObject(g_Queue);
		HANDLE hThread;
		if (0 > PsCreateSystemThread(&hThread, 0, 0, 0, 0, ThreadStartThunk, IoThread)) 
		{
			ObfDereferenceObject(g_Queue);
			ObfDereferenceObject(DriverObject);
		}
		else
		{
			nIoThreads++;
			NtClose(hThread);
		}
	} while (--Count);

	if (nIoThreads)
	{
		return STATUS_SUCCESS;
	}

	g_IoRundown->BeginRundown();

	return STATUS_UNSUCCESSFUL;
}

class DRIVER_RUNDOWN : public RUNDOWN_REF
{
	virtual void RundownCompleted()
	{
		DbgPrint("RundownCompleted(%x)\n", gnPackets);

		destroyterm();

		if (PLIST_ENTRY entry = KeRundownQueue(g_Queue))
		{
			DbgPrint("===== KeRundownQueue = %p\n", entry);
			__debugbreak();
		}

		ObfDereferenceObject(g_Queue);
	}
} grr;

RUNDOWN_REF * g_IoRundown = &grr;

#pragma pop_macro("DbgPrint")

//////////////////////////////////////////////////////////////////////////
// IO_OBJECT

void IO_OBJECT::operator delete(void* p)
{
	ExFreePool(p);
	g_IoRundown->Release();
}

void* IO_OBJECT::operator new(size_t cb)
{
	if (g_IoRundown->Acquire())
	{
		if (PVOID p = ExAllocatePool(PagedPool, cb)) return p;
		g_IoRundown->Release();
	}
	return 0;
}

IO_OBJECT::IO_OBJECT()
{
	m_FileObject = 0, m_hFile = 0, m_nRef = 1;
	DbgPrint("%s<%p>\n", __FUNCTION__, this);
}

IO_OBJECT::~IO_OBJECT()
{
	Close();
	DbgPrint("%s<%p>\n", __FUNCTION__, this);
}

void IO_OBJECT::CloseObjectHandle(HANDLE hFile, PFILE_OBJECT FileObject)
{
	if (FileObject)
	{
		ObfDereferenceObject(FileObject);
	}
	if (hFile) (ExGetPreviousMode() == KernelMode ? NtClose : ZwClose)(hFile);
}

NTSTATUS IO_OBJECT::Assign(HANDLE hFile)
{
	PFILE_OBJECT FileObject;
	NTSTATUS status = ObReferenceObjectByHandle(hFile, 0, *IoFileObjectType, KernelMode, (void**)&FileObject, 0);

	if (0 <= status)
	{
		if (0 > (status = Assign(hFile, FileObject)))
		{
			ObfDereferenceObject(FileObject);
		}
	}

	return status;
}

NTSTATUS IO_OBJECT::Assign(HANDLE hFile, PFILE_OBJECT FileObject)
{
	if (PIO_COMPLETION_CONTEXT CompletionContext = (PIO_COMPLETION_CONTEXT)
		ExAllocatePoolWithTag( PagedPool, sizeof( IO_COMPLETION_CONTEXT ), 'cCoI' ))
	{
		CompletionContext->Key = this;
		CompletionContext->Port = g_Queue;
		ObfReferenceObject(g_Queue);

		if (InterlockedCompareExchangePointer( (void**)&FileObject->CompletionContext, CompletionContext, NULL ))
		{
			ObfDereferenceObject(g_Queue);
			ExFreePool(CompletionContext);
			return STATUS_PORT_ALREADY_SET;
		}

		m_FileObject = FileObject;
		m_hFile = hFile;
		m_HandleLock.Init();

		return STATUS_SUCCESS;
	}

	return STATUS_INSUFFICIENT_RESOURCES;
}

//////////////////////////////////////////////////////////////////////////
//

void NTAPI IO_OBJECT_TIMEOUT::RtlTimer::OnDpc(PKDPC Dpc, PVOID , PVOID , PVOID )
{
	DbgPrint("RtlTimer::OnDpc(%p)\n", Dpc);
	ExQueueWorkItem(static_cast<RtlTimer*>(Dpc), DelayedWorkQueue);
}

void NTAPI IO_OBJECT_TIMEOUT::RtlTimer::OnWorkItem(PVOID This)
{
	CPP_FUNCTION;
	DbgPrint("RtlTimer::OnWorkItem(%p)\n", This);
	IO_OBJECT_TIMEOUT* pObj = reinterpret_cast<RtlTimer*>(This)->_pObj;
	pObj->SetNewTimer(0);
	pObj->OnTimeout();
	reinterpret_cast<RtlTimer*>(This)->Release();
}//UnlockDriver()

void IO_OBJECT_TIMEOUT::RtlTimer::Cancel()
{
	DbgPrint("%s(%p)\n", __FUNCTION__, this);
	if (KeCancelTimer(this))
	{
		DbgPrint("%s:2(%p)\n", __FUNCTION__, this);
		Release();
		UnlockDriver();
	}
}

void IO_OBJECT_TIMEOUT::SetNewTimer(RtlTimer* pTimer)
{
	DbgPrint("%s(%p)\n", __FUNCTION__, pTimer);

	if (pTimer = (RtlTimer*)InterlockedExchangePointer((void**)&_pTimer, pTimer))
	{
		DbgPrint("%s:2(%p)\n", __FUNCTION__, pTimer);
		pTimer->Cancel();
		pTimer->Release();
	}
}

BOOL IO_OBJECT_TIMEOUT::SetTimeout(DWORD dwMilliseconds)
{
	LARGE_INTEGER li;
	li.QuadPart = -(LONGLONG)dwMilliseconds * 10000;
	return SetTimeout(li);
}

BOOL IO_OBJECT_TIMEOUT::SetTimeout(LARGE_INTEGER dueTime)
{
	BOOL fOk = FALSE;

	if (RtlTimer* pTimer = new RtlTimer(this))
	{
		fOk = TRUE;

		pTimer->Set(dueTime);

		// _pTimer hold additional reference
		pTimer->AddRef();
		SetNewTimer(pTimer);

		if (KeReadStateTimer(pTimer))
		{
			DbgPrint("%s:2(%p)\n", __FUNCTION__, pTimer);
			// timer object is signaled
			SetNewTimer(0);
		}

		pTimer->Release();
	}

	return fOk;
}

_NT_END