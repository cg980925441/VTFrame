#include "PageHook.h"

#include "InlineHook.h"
#include "../Util/LDasm.h"
#include "../Include/common.h"
#include "../VMX/vtasm.h"

LIST_ENTRY g_PageList = { 0 };


PPAGE_HOOK_ENTRY PHGetHookEntry(IN PVOID ptr)
{
	if (g_PageList.Flink == NULL || IsListEmpty(&g_PageList))
		return NULL;

	for (PLIST_ENTRY pListEntry = g_PageList.Flink; pListEntry != &g_PageList; pListEntry = pListEntry->Flink)
	{
		PPAGE_HOOK_ENTRY pEntry = CONTAINING_RECORD(pListEntry, PAGE_HOOK_ENTRY, Link);
		if (pEntry->OriginalPtr == ptr)
			return pEntry;
	}

	return NULL;
}

PPAGE_HOOK_ENTRY PHGetHookEntryByPage(IN PVOID ptr, IN PAGE_TYPE Type)
{
	if (g_PageList.Flink == NULL || IsListEmpty(&g_PageList))
		return NULL;

	PVOID page = PAGE_ALIGN(ptr);
	for (PLIST_ENTRY pListEntry = g_PageList.Flink; pListEntry != &g_PageList; pListEntry = pListEntry->Flink)
	{
		//CONTAINING_RECORD�����Ǹ��ݽṹ���ͺ�����һ��ʵ�λ�ýṹ�Ŀ�ʼλ��
		PPAGE_HOOK_ENTRY pEntry = CONTAINING_RECORD(pListEntry, PAGE_HOOK_ENTRY, Link);
		if ((Type == DATA_PAGE && pEntry->DataPageVA == page) || (Type == CODE_PAGE && pEntry->CodePageVA == page))
			return pEntry;
	}

	return NULL;
}

//����ԭ����ǰN�ֽ�+���ص�ԭ����+N���棬pSize��ԭ����ǰN���ֽڴ�С,OriginalStore��ԭ�����׵�ַ
NTSTATUS PHpCopyCode(IN PVOID pFunc, OUT PUCHAR OriginalStore, OUT PULONG pSize)
{
	ULONG len = 0;
	JUMP_THUNK jmpRet = { 0 };
	ldasm_data data = { 0 };
	KIRQL irql = 0;

	do
	{
		len += ldasm(pFunc, &data, TRUE);
	} while (len < sizeof(JUMP_THUNK));

	//����ԭʼָ�ǰN���ֽڵ�����Hook�ṹ�䵱ԭʼ�������
	RtlCopyMemory(OriginalStore, pFunc, len);

	//���ص�ԭʼ����ǰN���ֽں�
	InitJumpThunk(&jmpRet, (ULONG64)pFunc + len);

	RtlCopyMemory((PVOID)((ULONG64)OriginalStore + len), &jmpRet, sizeof(JUMP_THUNK));

	*pSize = len;

	return STATUS_SUCCESS;
}

NTSTATUS PHHook(IN PVOID pFunc, IN PVOID pHook)
{
	PUCHAR CodePage = NULL;
	BOOLEAN Newpage = FALSE;
	PHYSICAL_ADDRESS phys = { 0 };
	phys.QuadPart = MAXULONG64;

	
	//�Ƿ��Ѿ�HOOK��
	PPAGE_HOOK_ENTRY pEntry = PHGetHookEntryByPage(pFunc, DATA_PAGE);
	if (pEntry != NULL)
	{
		CodePage = pEntry->CodePageVA;
	}
	else
	{
		//����һҳ�ڴ�
		CodePage = MmAllocateContiguousMemory(PAGE_SIZE, phys);
		
		Newpage = TRUE;
	}

	if (CodePage == NULL)
		return STATUS_INSUFFICIENT_RESOURCES;

	//����һ��PageHookEntry�ṹ���뵽PageHook����
	PPAGE_HOOK_ENTRY pHookEntry = ExAllocatePoolWithTag(NonPagedPool, sizeof(PAGE_HOOK_ENTRY), 'VTF');
	if (pHookEntry == NULL)
		return STATUS_INSUFFICIENT_RESOURCES;

	RtlZeroMemory(pHookEntry, sizeof(PAGE_HOOK_ENTRY));
	RtlCopyMemory(CodePage, PAGE_ALIGN(pFunc), PAGE_SIZE);

	//����ԭʼ�����ݵ�PageHookEntry��emmmm���������Ϊ�˵���ԭ���ĺ���
	NTSTATUS status = PHpCopyCode(pFunc, pHookEntry->OriginalData, &pHookEntry->OriginalSize);
	if (!NT_SUCCESS(status))
	{
		ExFreePoolWithTag(pHookEntry, 'VTF');
		return status;
	}

	//ҳ��ƫ��
	//PAGE_ALIGN��������ʵ����ȡpFunc�ĸ�52λ��ֵ������12λ���㣩,�����͵õ��������ַ��ҳ��ƫ��
	ULONG_PTR page_offset = (ULONG_PTR)pFunc - (ULONG_PTR)PAGE_ALIGN(pFunc);
	
	//����һ����Hook��������ת
	//��ʱ���������ҳ�ڴ�ԭ��������ֵ����һ��������Hook����������תָ��
	JUMP_THUNK thunk = { 0 };
	InitJumpThunk(&thunk, (ULONG64)pHook);
	RtlZeroMemory(CodePage + page_offset,pHookEntry->OriginalSize);
	memcpy(CodePage + page_offset, &thunk, sizeof(thunk));

	//ԭʼ��Ŀ��
	pHookEntry->OriginalPtr = 0;
	pHookEntry->DataPageVA = PAGE_ALIGN(pFunc);
	pHookEntry->DataPagePFN = PFN(MmGetPhysicalAddress(pFunc).QuadPart);
	pHookEntry->CodePageVA = CodePage;
	pHookEntry->CodePagePFN = PFN(MmGetPhysicalAddress(CodePage).QuadPart);

	// ����PageHook����
	if (g_PageList.Flink == NULL)
		InitializeListHead(&g_PageList);
	InsertTailList(&g_PageList, &pHookEntry->Link);

	// ����VMM����HOOK
	if (Newpage)
	{
		for (int i = 0; i < KeNumberProcessors; i++)
		{
			KeSetSystemAffinityThread((KAFFINITY)(1 << i));

			//ִ��ָ������
			__vmx_vmcall(VTFrame_HOOK_PAGE, pHookEntry->DataPagePFN, pHookEntry->CodePagePFN,0);

			KeRevertToUserAffinityThread();
		}
	}

	return STATUS_SUCCESS;
}

/**
 * �޸������ַ����EPTҳ�棬��ȡ���ȡԭ����ҳ�棬�����޸ĺ��ҳ�治�ᱻCRC
 */
NTSTATUS ModifyAddressValue(PVOID address)
{

	PUCHAR CodePage = NULL;
	BOOLEAN Newpage = FALSE;
	PHYSICAL_ADDRESS phys = { 0 };
	phys.QuadPart = MAXULONG64;


	//�Ƿ��Ѿ�HOOK��
	PPAGE_HOOK_ENTRY pEntry = PHGetHookEntryByPage(address, DATA_PAGE);
	if (pEntry != NULL)
	{
		CodePage = pEntry->CodePageVA;
	}
	else
	{
		//����һҳ�ڴ�
		CodePage = MmAllocateContiguousMemory(PAGE_SIZE, phys);
		Newpage = TRUE;
	}

	if (CodePage == NULL)
		return STATUS_INSUFFICIENT_RESOURCES;

	//����һ��PageHookEntry�ṹ���뵽PageHook����
	PPAGE_HOOK_ENTRY pHookEntry = ExAllocatePoolWithTag(NonPagedPool, sizeof(PAGE_HOOK_ENTRY), 'VTF');
	if (pHookEntry == NULL)
		return STATUS_INSUFFICIENT_RESOURCES;

	RtlZeroMemory(pHookEntry, sizeof(PAGE_HOOK_ENTRY));

	//����ԭʼҳ���ݵ��������ҳ��
	RtlCopyMemory(CodePage, PAGE_ALIGN(address), PAGE_SIZE);

	//ԭʼ��Ŀ��
	pHookEntry->OriginalPtr = address;
	pHookEntry->DataPageVA = PAGE_ALIGN(address);
	pHookEntry->DataPagePFN = PFN(MmGetPhysicalAddress(address).QuadPart);
	pHookEntry->DataPhys = MmGetPhysicalAddress(address).QuadPart;
	pHookEntry->CodePageVA = CodePage;
	pHookEntry->CodePagePFN = PFN(MmGetPhysicalAddress(CodePage).QuadPart);

	// ����PageHook����
	if (g_PageList.Flink == NULL)
		InitializeListHead(&g_PageList);
	InsertTailList(&g_PageList, &pHookEntry->Link);
	

	// ����VMM����HOOK
	if (Newpage)
	{
		for (int i = 0; i < KeNumberProcessors; i++)
		{
			KeSetSystemAffinityThread((KAFFINITY)(1 << i));

			//ִ��ָ������
			__vmx_vmcall(VTFrame_HOOK_PAGE, pHookEntry->DataPagePFN, pHookEntry->CodePagePFN, 0);

			KeRevertToUserAffinityThread();
		}
	}

	return STATUS_SUCCESS;
}


NTSTATUS ModifyAddressValue2(PVOID address, PVOID pByte, ULONG length, PVOID address1, PVOID pByte1, ULONG length1)
{

	PUCHAR CodePage = NULL;
	BOOLEAN Newpage = FALSE;
	PHYSICAL_ADDRESS phys = { 0 };
	phys.QuadPart = MAXULONG64;


	//�Ƿ��Ѿ�HOOK��
	PPAGE_HOOK_ENTRY pEntry = PHGetHookEntryByPage(address, DATA_PAGE);
	if (pEntry != NULL)
	{
		CodePage = pEntry->CodePageVA;
	}
	else
	{
		//����һҳ�ڴ�
		CodePage = MmAllocateContiguousMemory(PAGE_SIZE, phys);
		Newpage = TRUE;
	}

	if (CodePage == NULL)
		return STATUS_INSUFFICIENT_RESOURCES;

	//����һ��PageHookEntry�ṹ���뵽PageHook����
	PPAGE_HOOK_ENTRY pHookEntry = ExAllocatePoolWithTag(NonPagedPool, sizeof(PAGE_HOOK_ENTRY), 'VTF');
	if (pHookEntry == NULL)
		return STATUS_INSUFFICIENT_RESOURCES;

	RtlZeroMemory(pHookEntry, sizeof(PAGE_HOOK_ENTRY));
	//����ԭʼҳ���ݵ��������ҳ��

	RtlCopyMemory(CodePage, PAGE_ALIGN(address), PAGE_SIZE);

	//ҳ��ƫ��
	//PAGE_ALIGN��������ʵ����ȡpFunc�ĸ�52λ��ֵ������12λ���㣩,�����͵õ��������ַ��ҳ��ƫ��
	ULONG_PTR page_offset = (ULONG_PTR)address - (ULONG_PTR)PAGE_ALIGN(address);
	ULONG_PTR page_offset1 = (ULONG_PTR)address1 - (ULONG_PTR)PAGE_ALIGN(address1);

	//����ԭҳ�洦���ڴ�
	RtlCopyMemory(CodePage + page_offset, pByte, length);
	RtlCopyMemory(CodePage + page_offset1, pByte1, length1);

	//ԭʼ��Ŀ��
	pHookEntry->OriginalPtr = address;
	pHookEntry->DataPageVA = PAGE_ALIGN(address);
	pHookEntry->DataPagePFN = PFN(MmGetPhysicalAddress(address).QuadPart);
	pHookEntry->CodePageVA = CodePage;
	pHookEntry->CodePagePFN = PFN(MmGetPhysicalAddress(CodePage).QuadPart);

	// ����PageHook����
	if (g_PageList.Flink == NULL)
		InitializeListHead(&g_PageList);
	InsertTailList(&g_PageList, &pHookEntry->Link);


	// ����VMM����HOOK
	if (Newpage)
	{
		for (int i = 0; i < KeNumberProcessors; i++)
		{
			KeSetSystemAffinityThread((KAFFINITY)(1 << i));

			//ִ��ָ������
			__vmx_vmcall(VTFrame_HOOK_PAGE, pHookEntry->DataPagePFN, pHookEntry->CodePagePFN, 0);

			KeRevertToUserAffinityThread();
		}
	}

	return STATUS_SUCCESS;
}


NTSTATUS UnPageHook() 
{
	for (PLIST_ENTRY pListEntry = g_PageList.Flink; pListEntry != &g_PageList; pListEntry = pListEntry->Flink)
	{
		PPAGE_HOOK_ENTRY pEntry = NULL;
		pEntry = CONTAINING_RECORD(pListEntry, PAGE_HOOK_ENTRY, Link);

		for (int i = 0; i < KeNumberProcessors; i++)
		{
			KeSetSystemAffinityThread((KAFFINITY)(1 << i));

			//ִ��ָ������
			__vmx_vmcall(VTFrame_UNHOOK_PAGE, pEntry->DataPagePFN, 0, 0);

			KeRevertToUserAffinityThread();
		}
		RemoveEntryList(pListEntry);
		
	}

	return STATUS_SUCCESS;
	
}
