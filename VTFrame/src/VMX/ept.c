#include "ept.h"

#include "../Include/VMCS.h"
#include "../Hook/PageHook.h"
#include "../VMX/ExitHandle.h"
#include "../APC/APC.h"


/************************************************************************/
/*								����EPTҳ��		      					*/
/************************************************************************/
ULONG64 *BuildEPTTable()
{
	ULONG64 *ept_PML4 = 0;
	PHYSICAL_ADDRESS FirstPdptPA = { 0 }, FirstPdtPA = { 0 }, FirstPtePA = { 0 }, lowest = { 0 }, higest = { 0 };
	higest.QuadPart = ~0;

	ept_PML4 = MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE,lowest,higest,lowest, MmNonCached);
	if (!ept_PML4)
	{
		return NULL;
	}
	RtlZeroMemory(ept_PML4,PAGE_SIZE);

	ULONG64 *ept_PDPT = MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE, lowest, higest, lowest, MmNonCached);

	if (!ept_PDPT)
	{
		return NULL;
	}
	RtlZeroMemory(ept_PDPT, PAGE_SIZE);

	FirstPdptPA = MmGetPhysicalAddress(ept_PDPT);
	*ept_PML4 = (FirstPdptPA.QuadPart) + 7;

	for (ULONG64 a = 0; a < EPT_MEMORY_SIZE; a++)
	{
		ULONG64 *ept_PDT = MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE, lowest, higest, lowest, MmNonCached);
	
		if (!ept_PDT)
		{
			return NULL;
		}
		RtlZeroMemory(ept_PDT, PAGE_SIZE);
		
		FirstPdtPA = MmGetPhysicalAddress(ept_PDT);
		*ept_PDPT = (FirstPdtPA.QuadPart) + 7;
		ept_PDPT++;

		for (ULONG64 b =0; b < 512; b++)
		{
			ULONG64 *ept_PTE = MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE, lowest, higest, lowest, MmNonCached);
		
			if (!ept_PTE)
			{
				return NULL;
			}
			RtlZeroMemory(ept_PTE, PAGE_SIZE);

			FirstPtePA = MmGetPhysicalAddress(ept_PTE);
			*ept_PDT = (FirstPtePA.QuadPart) + 7;
			ept_PDT++;

			for (ULONG64 c = 0; c < 512; c++)
			{
				*ept_PTE = (a * (1<<30) + b * (1<<21) + c * (1<<12) + 0x37);
				ept_PTE++;
			}
		}

	}

	return ept_PML4;
	
}



//���VMCS�ṹ����EPT����
VOID EptEnable(IN ULONG64 *PML4)
{
	VMX_CPU_BASED_CONTROLS primary = { 0 };
	VMX_SECONDARY_CPU_BASED_CONTROLS secondary = { 0 };
	EPT_TABLE_POINTER EPTP = { 0 };

	__vmx_vmread(SECONDARY_VM_EXEC_CONTROL, (size_t*)&secondary.All);
	__vmx_vmread(CPU_BASED_VM_EXEC_CONTROL, (size_t*)&primary.All);

	// Set up the EPTP
	EPTP.Fields.PhysAddr = MmGetPhysicalAddress(PML4).QuadPart >> 12;
	EPTP.Fields.PageWalkLength = 3;
	EPTP.Fields.MemoryType = 6;

	__vmx_vmwrite(EPT_POINTER, EPTP.All);
	__vmx_vmwrite(VIRTUAL_PROCESSOR_ID, VM_VPID);

	//����secondary
	primary.Fields.ActivateSecondaryControl = TRUE;
	//���ÿ���EPT
	secondary.Fields.EnableEPT = TRUE;
	//���ÿ���VPID
	secondary.Fields.EnableVPID = TRUE;

	//д�����
	__vmx_vmwrite(SECONDARY_VM_EXEC_CONTROL, secondary.All);
	__vmx_vmwrite(CPU_BASED_VM_EXEC_CONTROL, primary.All);

	// Critical step
	EPT_CTX ctx = { 0 };

	//ʹEPT��TLBʧЧ
	__invept(INV_ALL_CONTEXTS, &ctx);
}

/************************************************************************/
/* �����Ժ�����������ȡ����ҳ֡��level�����ҳ���е�ƫ��                 */
/************************************************************************/
inline ULONG64 EptpTableOffset(IN ULONG64 pfn, IN CHAR level)
{
	ULONG64 mask = (1ULL << (level * EPT_TABLE_ORDER)) - 1;
	return (pfn & mask) >> ((level-1) * EPT_TABLE_ORDER);
}


/************************************************************************/
/* �����Ժ�����������������ҳ֡��ȡ��PTE�ṹ��Ҳ�������һ��ҳ��      */
/************************************************************************/
PEPT_PTE_ENTRY GetPteEntry(ULONG64 pfn)
{
	PEPT_MMPTE pML4 = NULL;
	PEPT_MMPTE pTable = NULL;
	PEPT_MMPTE pNewEPT = NULL;
	ULONG64 offset = 0;

	pML4 = (PEPT_MMPTE)g_data->cpu_data[CPU_IDX].ept_PML4T;
	pNewEPT = pML4;
	pTable = pML4;

	for (int i = 4; i > 1; i--)
	{
		//�õ���i��ҳ�����е�Entry
		pTable = pNewEPT;
		offset = EptpTableOffset(pfn, (char)i);
		PEPT_MMPTE pEPT = &pTable[offset];

		//�õ���һ��ҳ��������ַ
		if (pEPT->Fields.PhysAddr != 0)
		{
			PHYSICAL_ADDRESS phys = { 0 };
			phys.QuadPart = pEPT->Fields.PhysAddr << 12;
			pNewEPT = MmGetVirtualForPhysical(phys);
		}
	}

	offset = EptpTableOffset(pfn, (char)1);
	return (PEPT_PTE_ENTRY)&pNewEPT[offset];
}

//�޸�ָ��ҳ���PTEȨ�ޣ�ʹ֮����Exit�¼�
VOID PteModify(ULONG64 data,ULONG64 code) 
{
	PEPT_PTE_ENTRY pPte = GetPteEntry(data);
	pPte->Fields.Read = 0;
	pPte->Fields.Write = 0;
	pPte->Fields.Execute = 1;
	pPte->Fields.PhysAddr = code;
}

VOID UnPteModify(ULONG64 data) 
{
	PEPT_PTE_ENTRY pPte = GetPteEntry(data);
	pPte->Fields.Read = 1;
	pPte->Fields.Write = 1;
	pPte->Fields.Execute = 1;
	pPte->Fields.PhysAddr = data;
}

ULONG iii = 0;

//�Ѷ�EPT Exit������������
VOID VmExitEptViolation(IN PGUEST_STATE GuestState)
{
	ULONG64 pfn = PFN(GuestState->PhysicalAddress.QuadPart);
	PEPT_VIOLATION_DATA pViolationData = (PEPT_VIOLATION_DATA)&GuestState->ExitQualification;
	
	//�������ǵ�PageHook����
	for (PLIST_ENTRY pListEntry = g_PageList.Flink ; pListEntry != &g_PageList ; pListEntry = pListEntry->Flink)
	{
		PPAGE_HOOK_ENTRY pEntry = NULL;
		pEntry = CONTAINING_RECORD(pListEntry, PAGE_HOOK_ENTRY, Link);
		if (pEntry->DataPagePFN == pfn)
		{
			//��ȡԭʼҳ�棬д���ִ���޸ĺ��ҳ��
			if (pViolationData->Fields.Read)
			{
				PEPT_PTE_ENTRY pte = GetPteEntry(pfn);
				pte->Fields.Read = 1;
				pte->Fields.Write = 1;
				pte->Fields.Execute = 0;
				pte->Fields.PhysAddr = pEntry->DataPagePFN;
			}
			else if (pViolationData->Fields.Write)
			{
				ULONG64 phys = pEntry->CodePagePFN;
				PEPT_PTE_ENTRY pte = GetPteEntry(pfn);
				pte->Fields.Read = 1;
				pte->Fields.Write = 1;
				pte->Fields.Execute = 0;
				pte->Fields.PhysAddr = pEntry->CodePagePFN;
			}
			else if (pViolationData->Fields.Execute)
			{
				PEPT_PTE_ENTRY pte = GetPteEntry(pfn);
				pte->Fields.Read = 0;
				pte->Fields.Write = 0;
				pte->Fields.Execute = 1;
				pte->Fields.PhysAddr = pEntry->CodePagePFN;
			}
			
		}
	}
	EPT_CTX ctx = { 0 };
	__invept(INV_ALL_CONTEXTS, &ctx);
	ToggleMTF(TRUE);
}

VOID VmExitMTF(IN PGUEST_STATE GuestState)
{
	//������HOOK��ҳ��ȫ��ȥ��Ȩ��,Ϊ��������һ�ε�VM EXIT
	for (PLIST_ENTRY pListEntry = g_PageList.Flink; pListEntry != &g_PageList; pListEntry = pListEntry->Flink)
	{
		PPAGE_HOOK_ENTRY pEntry = NULL;
		pEntry = CONTAINING_RECORD(pListEntry, PAGE_HOOK_ENTRY, Link);
		PEPT_PTE_ENTRY pte = GetPteEntry(pEntry->DataPagePFN);
		pte->Fields.Read = 0;
		pte->Fields.Write = 0;
		pte->Fields.Execute = 1;
		pte->Fields.PhysAddr = pEntry->CodePagePFN;
	}

	ToggleMTF(FALSE);
}


VOID VmExitEptMisconfig(IN PGUEST_STATE GuestState)
{
	KeBugCheckEx(HYPERVISOR_ERROR, BUG_CHECK_EPT_MISCONFIG, GuestState->PhysicalAddress.QuadPart, GuestState->ExitQualification, 0);
}