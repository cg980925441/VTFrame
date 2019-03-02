﻿#include <ntddk.h>
#include "VMX/VMX.h"
#include "Test/Test.h"
#include "Monitor/Monitor.h"
#include "Include/DriverDef.h"
#include "IDT/idt.h"

VOID Unload(PDRIVER_OBJECT DriverObject);
extern LIST_ENTRY g_PageList;


//让驱动可以创建回调
VOID BypassCheckSign(PDRIVER_OBJECT pDriverObj)
{
	//STRUCT FOR WIN64
	typedef struct _LDR_DATA                         			// 24 elements, 0xE0 bytes (sizeof)
	{
		struct _LIST_ENTRY InLoadOrderLinks;                     // 2 elements, 0x10 bytes (sizeof)
		struct _LIST_ENTRY InMemoryOrderLinks;                   // 2 elements, 0x10 bytes (sizeof)
		struct _LIST_ENTRY InInitializationOrderLinks;           // 2 elements, 0x10 bytes (sizeof)
		VOID*        DllBase;
		VOID*        EntryPoint;
		ULONG32      SizeOfImage;
		UINT8        _PADDING0_[0x4];
		struct _UNICODE_STRING FullDllName;                      // 3 elements, 0x10 bytes (sizeof)
		struct _UNICODE_STRING BaseDllName;                      // 3 elements, 0x10 bytes (sizeof)
		ULONG32      Flags;
	}LDR_DATA, *PLDR_DATA;
	PLDR_DATA ldr;
	ldr = (PLDR_DATA)(pDriverObj->DriverSection);
	ldr->Flags |= 0x20;
}


VOID MyCreateProcessNotifyEx
(
	__inout   PEPROCESS Process,
	__in      HANDLE ProcessId,
	__in_opt  PPS_CREATE_NOTIFY_INFO CreateInfo
)
{
	NTSTATUS st = 0;
	HANDLE hProcess = NULL;
	OBJECT_ATTRIBUTES oa = { 0 };
	CLIENT_ID ClientId = { 0 };
	char xxx[16] = { 0 };
	if (CreateInfo == NULL)	
	{
		if (GamePid == ProcessId)
		{
			DbgPrint("游戏进程退出,开始清理工作\n");
			GamePid = 0;
			for (int i = 0; i < KeNumberProcessors; i++)
			{
				KeSetSystemAffinityThread((KAFFINITY)(1 << i));
				__vmx_vmcall(VTFrame_Test2, RealCr3, FakeCr3, 0);
				KeRevertToUserAffinityThread();
			}
			//取消EPT Hook
			/*for (PLIST_ENTRY pListEntry = g_PageList.Flink; pListEntry != &g_PageList; pListEntry = pListEntry->Flink) {
				PPAGE_HOOK_ENTRY pEntry = NULL;
				pEntry = CONTAINING_RECORD(pListEntry, PAGE_HOOK_ENTRY, Link);

				for (int i = 0; i < KeNumberProcessors; i++)
				{
					KeSetSystemAffinityThread((KAFFINITY)(1 << i));
					__vmx_vmcall(VTFrame_Test2, pEntry->DataPagePFN,0, 0);
					KeRevertToUserAffinityThread();
				}
				
			}
			g_PageList.Flink = NULL;*/
		}
		
	}
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
	NTSTATUS status;

	// 查询硬件是否支持VT
	if (!IsVTSupport())
		return STATUS_UNSUCCESSFUL;

	// 申请全局变量的内存
	if (!AllocGlobalMemory())
		return STATUS_UNSUCCESSFUL;

	// 开启VT主要代码
	if (!StartVT())
		return STATUS_UNSUCCESSFUL;

	// 是否开启VT成功
	for (int i = 0; i <= (g_data->vcpus - 1); i++)
	{
		if (g_data->cpu_data[i].VmxState == VMX_STATE_ON)
			DbgPrint("VTFrame:CPU:%d开启VT成功\n", i);
	}

	
	TestSSDTHook();
	TestInlineHook();
	PrintIdt();

	//TestPageHook();
	//失效回调
	EnableObType(*PsProcessType, FALSE);
	EnableObType(*PsThreadType, FALSE);

	BypassCheckSign(DriverObject);
	//开个回调监控游戏进程退出,进行清理工作
	status = PsSetCreateProcessNotifyRoutineEx((PCREATE_PROCESS_NOTIFY_ROUTINE_EX)MyCreateProcessNotifyEx, FALSE);
	DbgPrint("PsSetCreateProcessNotifyRoutineEx return: %x", status);


	//符号链接
	status = CreateDeviceAndSymbol(DriverObject);
	if (!NT_SUCCESS(status))
		return status;

	//IRP
	DriverObject->MajorFunction[IRP_MJ_CREATE] = CREATE_DISPATCH;
	DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DEVICE_CONTROL_DISPATCH;
	DriverObject->DriverUnload = Unload;
	return status;
}

VOID Unload(PDRIVER_OBJECT DriverObject)
{
	for (int i = 0; i < KeNumberProcessors; i++)
	{
		KeSetSystemAffinityThread((KAFFINITY)(1 << i));

		SetupVT(NULL);

		KeRevertToUserAffinityThread();
	}
	FreeGlobalData(g_data);
	DeleteDeviceAndSymbol();
	DbgPrint("VTFrame:卸载VT成功\n");
	DbgPrint("VTFrame:Driver Unload\n");
}