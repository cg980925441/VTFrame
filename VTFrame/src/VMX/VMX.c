#include "VMX.h"

#include "../Include/CPU.h"
#include "../Include/Native.h"
#include "../Include/VMCS.h"
#include "vtasm.h"
#include "ept.h"
#include "VmxEvent.h"

//�ؼ������ݽṹָ�룬��ÿ��CPU��VCPU�ṹ��CPU����
PGLOBAL_DATA g_data = NULL;


NTSTATUS UtilProtectNonpagedMemory(IN PVOID ptr, IN ULONG64 size, IN ULONG protection)
{
	NTSTATUS status = STATUS_SUCCESS;
	PMDL pMdl = IoAllocateMdl(ptr, (ULONG)size, FALSE, FALSE, NULL);
	if (pMdl)
	{
		MmBuildMdlForNonPagedPool(pMdl);
		pMdl->MdlFlags |= MDL_MAPPED_TO_SYSTEM_VA;
		status = MmProtectMdlSystemAddress(pMdl, protection);
		IoFreeMdl(pMdl);
		return status;
	}

	return STATUS_UNSUCCESSFUL;
}


VOID VmxpConvertGdtEntry(IN PVOID GdtBase, IN USHORT Selector, OUT PVMX_GDTENTRY64 VmxGdtEntry)
{
	PKGDTENTRY64 gdtEntry = NULL;

	// Read the GDT entry at the given selector, masking out the RPL bits. x64
	// Windows does not use an LDT for these selectors in kernel, so the TI bit
	// should never be set.
	NT_ASSERT((Selector & SELECTOR_TABLE_INDEX) == 0);
	gdtEntry = (PKGDTENTRY64)((ULONG_PTR)GdtBase + (Selector & ~RPL_MASK));

	// Write the selector directly 
	VmxGdtEntry->Selector = Selector;

	// Use the LSL intrinsic to read the segment limit
	VmxGdtEntry->Limit = __segmentlimit(Selector);

	// Build the full 64-bit effective address, keeping in mind that only when
	// the System bit is unset, should this be done.
	//
	// NOTE: The Windows definition of KGDTENTRY64 is WRONG. The "System" field
	// is incorrectly defined at the position of where the AVL bit should be.
	// The actual location of the SYSTEM bit is encoded as the highest bit in
	// the "Type" field.
	VmxGdtEntry->Base = ((gdtEntry->Bytes.BaseHigh << 24) | (gdtEntry->Bytes.BaseMiddle << 16) | (gdtEntry->BaseLow)) & MAXULONG;
	VmxGdtEntry->Base |= ((gdtEntry->Bits.Type & 0x10) == 0) ? ((ULONG_PTR)gdtEntry->BaseUpper << 32) : 0;

	// Load the access rights
	VmxGdtEntry->AccessRights = 0;
	VmxGdtEntry->Bytes.Flags1 = gdtEntry->Bytes.Flags1;
	VmxGdtEntry->Bytes.Flags2 = gdtEntry->Bytes.Flags2;

	// Finally, handle the VMX-specific bits
	VmxGdtEntry->Bits.Reserved = 0;
	VmxGdtEntry->Bits.Unusable = !gdtEntry->Bits.Present;
}


ULONG VmxpAdjustMsr(IN LARGE_INTEGER ControlValue, ULONG DesiredValue)
{
	// VMX feature/capability MSRs encode the "must be 0" bits in the high word
	// of their value, and the "must be 1" bits in the low word of their value.
	// Adjust any requested capability/feature based on these requirements.
	DesiredValue &= ControlValue.HighPart;
	DesiredValue |= ControlValue.LowPart;
	return DesiredValue;
}


/************************************************************************/
/* �жϴ�����Ӳ���Ƿ�֧��VT��1.�������Ƿ���Intel��������2.�������Ƿ�֧��VT  3.�Ƿ�BIOS�Ϲر���VT���� 
                             4.�������Ƿ�֧��TRUEϵ��MSR�Ĵ���*/
/************************************************************************/
BOOLEAN IsVTSupport()
{
	
	CPUID data = { 0 };
	char vendor[0x20] = { 0 };
	__cpuid((int*)&data, 0);
	*(int*)(vendor) = data.ebx;
	*(int*)(vendor + 4) = data.edx;
	*(int*)(vendor + 8) = data.ecx;



	//���CPU����Intel�ģ�ֱ�ӷ���ʧ��
	if (!memcmp(vendor, "GenuineIntel", 12) == 0) {
		DbgPrint("����Intel CPU\n");
		return FALSE;
	}
		

	RtlZeroMemory(&data,sizeof(CPUID));

	//���CPU��֧��VT������ʧ��
	__cpuid((int*)&data, 1);
	if ((data.ecx & (1 << 5)) == 0) {
		DbgPrint("CPU��֧��VT\n");
		DbgPrint("cpuid 1:%x", data.ecx);
		return FALSE;
	}
	//DbgPrint("cpuid 1:%x", data.ecx);

	IA32_FEATURE_CONTROL_MSR Control = { 0 };
	Control.All = __readmsr(MSR_IA32_FEATURE_CONTROL);
	//DbgPrint("MSR_IA32_FEATURE_CONTROL:%lx", Control.All);
	//�����BIOS�Ͻ���VT������ʧ��
	if (Control.Fields.Lock == 0)
	{
		Control.Fields.Lock = TRUE;
		Control.Fields.EnableVmxon = TRUE;
		__writemsr(MSR_IA32_FEATURE_CONTROL, Control.All);
	}
	else if (Control.Fields.EnableVmxon == FALSE)
	{
		DbgPrint("VTFrame:��BIOSδ����VMX\n");
		return FALSE;
	}

	//���CPU��֧��TRUEϵ��MSR�Ĵ������������������壬��Ϊ���Ǻ���VMCS����ĳЩ�ṹ�Ǵ�TRUEϵ��MSR�ϻ�ȡ��
	IA32_VMX_BASIC_MSR base;
	base.All = __readmsr(MSR_IA32_VMX_BASIC);
	if (base.Fields.VmxCapabilityHint != 1)
	{
		DbgPrint("VTFrame:��CPU��֧��Trueϵ�мĴ���\n");
		return FALSE;
	}

	DbgPrint("VTFrame:CPU֧��VT\n");

	return TRUE;
}


/*
��Ҫ�Ķ�VMM�е�������Ŀ��ƾ�������,�˴���������,����Ĵ�������ExitHandle��
�˴������˶�Ӧ�Ĺ���,�ͱ�����ExitHandle�н��д���,��Ȼ��ᵼ����������.
*/
VOID VmxSetupVMCS(IN PVCPU Vcpu)
{
	PKPROCESSOR_STATE state = &Vcpu->HostState;
	VMX_GDTENTRY64 vmxGdtEntry = { 0 };
	VMX_VM_ENTER_CONTROLS vmEnterCtlRequested = { 0 };
	VMX_VM_EXIT_CONTROLS vmExitCtlRequested = { 0 };
	VMX_PIN_BASED_CONTROLS vmPinCtlRequested = { 0 };
	VMX_CPU_BASED_CONTROLS vmCpuCtlRequested = { 0 };
	VMX_SECONDARY_CPU_BASED_CONTROLS vmCpuCtl2Requested = { 0 };
	LARGE_INTEGER msrVmxPin = { 0 }, msrVmxCpu = { 0 }, msrVmxEntry = { 0 }, msrVmxExit = { 0 }, msrVmxSec = {0};

	//��ȡVMX���MSR�Ĵ��������CPU֧�ֵĹ��ܣ�����������õĹ���CPU��֧�ֵģ��ͱ��밴��CPU��Ҫ����

	//����������VM���п������PIN��PROCESS�ֶ�
	msrVmxPin.QuadPart = __readmsr(MSR_IA32_VMX_TRUE_PINBASED_CTLS);
	msrVmxCpu.QuadPart = __readmsr(MSR_IA32_VMX_TRUE_PROCBASED_CTLS);
	//cpu secondary
	msrVmxSec.QuadPart = __readmsr(MSR_IA32_VMX_PROCBASED_CTLS2);
	//VM Exit
	msrVmxExit.QuadPart = __readmsr(MSR_IA32_VMX_TRUE_EXIT_CTLS);
	//VM Entry
	msrVmxEntry.QuadPart = __readmsr(MSR_IA32_VMX_TRUE_ENTRY_CTLS);
	
	
	/////////////////////////////////////////////////////////////////////////////////////////////
	////////////////��ȻVMCS�󲿷ֵ�����,���Ƕ����Դ�MSR�ж�ȡ,����������Ҫʵ��ĳЩ����,������Ҫ�Լ��޸Ŀ���
	////////////////���´������ǿ�������,��ЩΪ����,��Щ�������Ƕ��ƵĹ���,�޸����,�ٽ���д�뵽VMCS������

	//��Ҫ��CPU����
	vmCpuCtlRequested.Fields.CR3LoadExiting = TRUE;//��д��CR3ʱ����VM-EXIT
	vmCpuCtlRequested.Fields.CR3StoreExiting = TRUE;//�ڶ�ȡCR3ʱ����VM-EXIT
	vmCpuCtlRequested.Fields.ActivateSecondaryControl = TRUE;//������Ҫ��CPU����
    vmCpuCtlRequested.Fields.UseMSRBitmaps = TRUE;//����MSR BitMap����
	vmCpuCtlRequested.Fields.MovDRExiting = TRUE;//��DR�Ĵ����Ĳ�������VM EXIT
	

	//��Ҫ��CPU����
	vmCpuCtl2Requested.Fields.EnableRDTSCP = TRUE;	// for Win10
	vmCpuCtl2Requested.Fields.EnableXSAVESXSTORS = TRUE;	// for Win10
	vmCpuCtl2Requested.Fields.EnableINVPCID = TRUE;	// for Win10
	//vmCpuCtl2Requested.Fields.EnableVPID = TRUE;	

	//�������������
	vmEnterCtlRequested.Fields.IA32eModeGuest = TRUE;//ֻ��ΪTRUEʱ��VM ENTRY���ܽ���IA32Eģʽ
	vmEnterCtlRequested.Fields.LoadDebugControls = TRUE;	//���DR

	//�˳����������
	vmExitCtlRequested.Fields.HostAddressSpaceSize = TRUE;//���ص�IA32Eģʽ��HOST�У�64λ������ΪTRUE
	vmExitCtlRequested.Fields.AcknowledgeInterruptOnExit = TRUE;
	
	//////////////////////////////////////////////////////////////////////////////////////////////
	

	//������MSR�Ĵ�����д����,����MSR Bitmap�ͻ�������MSR�Ĵ����Ķ�д
	// Load the MSR bitmap. Unlike other bitmaps, not having an MSR bitmap will
	// trap all MSRs, so have to allocate an empty one.
	PUCHAR bitMapReadLow = g_data->MSRBitmap;       // 0x00000000 - 0x00001FFF
	PUCHAR bitMapReadHigh = bitMapReadLow + 1024;   // 0xC0000000 - 0xC0001FFF
	PUCHAR bitMapWriteLow = bitMapReadHigh + 1024;
	PUCHAR bitMapWriteHigh = bitMapWriteLow + 1024;

	RTL_BITMAP bitMapReadLowHeader = { 0 };
	RTL_BITMAP bitMapReadHighHeader = { 0 };
	RTL_BITMAP bitMapWriteLowHeader = { 0 };
	RTL_BITMAP bitMapWriteHighHeader = { 0 };


	RtlInitializeBitMap(&bitMapReadLowHeader, (PULONG)bitMapReadLow, 1024 * 8);
	RtlInitializeBitMap(&bitMapReadHighHeader, (PULONG)bitMapReadHigh, 1024 * 8);
	RtlInitializeBitMap(&bitMapWriteLowHeader, (PULONG)bitMapWriteLow, 1024 * 8);
	RtlInitializeBitMap(&bitMapWriteHighHeader, (PULONG)bitMapWriteHigh, 1024 * 8);

	RtlSetBit(&bitMapReadLowHeader, MSR_IA32_FEATURE_CONTROL);    // MSR_IA32_FEATURE_CONTROL
	RtlSetBit(&bitMapReadLowHeader, MSR_IA32_DEBUGCTL);          // MSR_DEBUGCTL
	RtlSetBit(&bitMapReadHighHeader, MSR_LSTAR - 0xC0000000);     // MSR_LSTAR

	RtlSetBit(&bitMapWriteLowHeader, MSR_IA32_FEATURE_CONTROL);    // MSR_IA32_FEATURE_CONTROL
	RtlSetBit(&bitMapWriteLowHeader, MSR_IA32_DEBUGCTL);          // MSR_DEBUGCTL
	RtlSetBit(&bitMapWriteHighHeader, MSR_LSTAR - 0xC0000000);     // MSR_LSTAR

																  // VMX MSRs
	for (ULONG i = MSR_IA32_VMX_BASIC; i <= MSR_IA32_VMX_VMFUNC; i++)
	{
		RtlSetBit(&bitMapReadLowHeader, i);
		RtlSetBit(&bitMapWriteLowHeader, i);
	}

	__vmx_vmwrite(MSR_BITMAP, MmGetPhysicalAddress(g_data->MSRBitmap).QuadPart);

	//Page faults (exceptions with vector 14) are specially treated. When a page fault occurs, a processor consults 
	//(1) bit 14 of the exception bitmap; 
	//(2) the error code produced with the page fault[PFEC]; 
	//(3) the page - fault error - code mask field[PFEC_MASK]; 
	//and (4) the page - fault error - code match field[PFEC_MATCH].It checks if
	//PFEC & PFEC_MASK = PFEC_MATCH.If there is equality, the specification of bit 14 in the exception bitmap is
	//followed(for example, a VM exit occurs if that bit is set).If there is inequality, the meaning of that bit is
	//reversed(for example, a VM exit occurs if that bit is clear)

	/*
		//Ҫ����쳣����Ҫ���������ֶ�����Ϊ0�ģ�������������VMCS�ڴ�ʱ�Ѿ��������ڴ棬����û�����õ��ֶΣ�Ĭ��ֵ����0������Ҳ���Բ�����
		PAGE_FAULT_ERROR_CODE_MASK = 0x00004006,
		PAGE_FAULT_ERROR_CODE_MATCH = 0x00004008,
	*/

	//ת��1���쳣
	ULONG ExceptionBitmap = 0;
	ExceptionBitmap |= 1 << 1;
	__vmx_vmwrite(EXCEPTION_BITMAP, ExceptionBitmap);
	

	// If the ��VMCS shadowing�� VM-execution control is 1, the VMREAD and VMWRITE 
	//instructions access the VMCS referenced by this pointer.Otherwise, software should set
	//this field to FFFFFFFF_FFFFFFFFH to avoid VM - entry failures
	__vmx_vmwrite(VMCS_LINK_POINTER, MAXULONG64);

	//���п�����
	//Secondary
	__vmx_vmwrite(SECONDARY_VM_EXEC_CONTROL,
		VmxpAdjustMsr(msrVmxSec, vmCpuCtl2Requested.All)
	);

	//PIN
	__vmx_vmwrite(
		PIN_BASED_VM_EXEC_CONTROL,
		VmxpAdjustMsr(msrVmxPin, vmPinCtlRequested.All)
	);
	//CPU
	__vmx_vmwrite(
		CPU_BASED_VM_EXEC_CONTROL,
		VmxpAdjustMsr(msrVmxCpu, vmCpuCtlRequested.All)
	);
	//VM Exit
	__vmx_vmwrite(
		VM_EXIT_CONTROLS,
		VmxpAdjustMsr(msrVmxExit, vmExitCtlRequested.All)
	);
	//VM Entry
	__vmx_vmwrite(
		VM_ENTRY_CONTROLS,
		VmxpAdjustMsr(msrVmxEntry, vmEnterCtlRequested.All)
	);

	//�����Ƕ�Guest��Host��һЩ�Ĵ�����д

	// CS (Ring 0 Code)
	VmxpConvertGdtEntry(state->SpecialRegisters.Gdtr.Base, state->ContextFrame.SegCs, &vmxGdtEntry);
	__vmx_vmwrite(GUEST_CS_SELECTOR, vmxGdtEntry.Selector);
	__vmx_vmwrite(GUEST_CS_LIMIT, vmxGdtEntry.Limit);
	__vmx_vmwrite(GUEST_CS_AR_BYTES, vmxGdtEntry.AccessRights);
	__vmx_vmwrite(GUEST_CS_BASE, vmxGdtEntry.Base);
	__vmx_vmwrite(HOST_CS_SELECTOR, state->ContextFrame.SegCs & ~RPL_MASK);

	// SS (Ring 0 Data)
	VmxpConvertGdtEntry(state->SpecialRegisters.Gdtr.Base, state->ContextFrame.SegSs, &vmxGdtEntry);
	__vmx_vmwrite(GUEST_SS_SELECTOR, vmxGdtEntry.Selector);
	__vmx_vmwrite(GUEST_SS_LIMIT, vmxGdtEntry.Limit);
	__vmx_vmwrite(GUEST_SS_AR_BYTES, vmxGdtEntry.AccessRights);
	__vmx_vmwrite(GUEST_SS_BASE, vmxGdtEntry.Base);
	__vmx_vmwrite(HOST_SS_SELECTOR, state->ContextFrame.SegSs & ~RPL_MASK);

	// DS (Ring 3 Data)
	VmxpConvertGdtEntry(state->SpecialRegisters.Gdtr.Base, state->ContextFrame.SegDs, &vmxGdtEntry);
	__vmx_vmwrite(GUEST_DS_SELECTOR, vmxGdtEntry.Selector);
	__vmx_vmwrite(GUEST_DS_LIMIT, vmxGdtEntry.Limit);
	__vmx_vmwrite(GUEST_DS_AR_BYTES, vmxGdtEntry.AccessRights);
	__vmx_vmwrite(GUEST_DS_BASE, vmxGdtEntry.Base);
	__vmx_vmwrite(HOST_DS_SELECTOR, state->ContextFrame.SegDs & ~RPL_MASK);

	// ES (Ring 3 Data)
	VmxpConvertGdtEntry(state->SpecialRegisters.Gdtr.Base, state->ContextFrame.SegEs, &vmxGdtEntry);
	__vmx_vmwrite(GUEST_ES_SELECTOR, vmxGdtEntry.Selector);
	__vmx_vmwrite(GUEST_ES_LIMIT, vmxGdtEntry.Limit);
	__vmx_vmwrite(GUEST_ES_AR_BYTES, vmxGdtEntry.AccessRights);
	__vmx_vmwrite(GUEST_ES_BASE, vmxGdtEntry.Base);
	__vmx_vmwrite(HOST_ES_SELECTOR, state->ContextFrame.SegEs & ~RPL_MASK);

	// FS (Ring 3 Compatibility-Mode TEB)
	VmxpConvertGdtEntry(state->SpecialRegisters.Gdtr.Base, state->ContextFrame.SegFs, &vmxGdtEntry);
	__vmx_vmwrite(GUEST_FS_SELECTOR, vmxGdtEntry.Selector);
	__vmx_vmwrite(GUEST_FS_LIMIT, vmxGdtEntry.Limit);
	__vmx_vmwrite(GUEST_FS_AR_BYTES, vmxGdtEntry.AccessRights);
	__vmx_vmwrite(GUEST_FS_BASE, vmxGdtEntry.Base);
	__vmx_vmwrite(HOST_FS_BASE, vmxGdtEntry.Base);
	__vmx_vmwrite(HOST_FS_SELECTOR, state->ContextFrame.SegFs & ~RPL_MASK);

	// GS (Ring 3 Data if in Compatibility-Mode, MSR-based in Long Mode)
	VmxpConvertGdtEntry(state->SpecialRegisters.Gdtr.Base, state->ContextFrame.SegGs, &vmxGdtEntry);
	__vmx_vmwrite(GUEST_GS_SELECTOR, vmxGdtEntry.Selector);
	__vmx_vmwrite(GUEST_GS_LIMIT, vmxGdtEntry.Limit);
	__vmx_vmwrite(GUEST_GS_AR_BYTES, vmxGdtEntry.AccessRights);
	__vmx_vmwrite(GUEST_GS_BASE, state->SpecialRegisters.MsrGsBase);
	__vmx_vmwrite(HOST_GS_BASE, state->SpecialRegisters.MsrGsBase);
	__vmx_vmwrite(HOST_GS_SELECTOR, state->ContextFrame.SegGs & ~RPL_MASK);

	// Task Register (Ring 0 TSS)
	VmxpConvertGdtEntry(state->SpecialRegisters.Gdtr.Base, state->SpecialRegisters.Tr, &vmxGdtEntry);
	__vmx_vmwrite(GUEST_TR_SELECTOR, vmxGdtEntry.Selector);
	__vmx_vmwrite(GUEST_TR_LIMIT, vmxGdtEntry.Limit);
	__vmx_vmwrite(GUEST_TR_AR_BYTES, vmxGdtEntry.AccessRights);
	__vmx_vmwrite(GUEST_TR_BASE, vmxGdtEntry.Base);
	__vmx_vmwrite(HOST_TR_BASE, vmxGdtEntry.Base);
	__vmx_vmwrite(HOST_TR_SELECTOR, state->SpecialRegisters.Tr & ~RPL_MASK);

	// LDT
	VmxpConvertGdtEntry(state->SpecialRegisters.Gdtr.Base, state->SpecialRegisters.Ldtr, &vmxGdtEntry);
	__vmx_vmwrite(GUEST_LDTR_SELECTOR, vmxGdtEntry.Selector);
	__vmx_vmwrite(GUEST_LDTR_LIMIT, vmxGdtEntry.Limit);
	__vmx_vmwrite(GUEST_LDTR_AR_BYTES, vmxGdtEntry.AccessRights);
	__vmx_vmwrite(GUEST_LDTR_BASE, vmxGdtEntry.Base);

	// GDT
	__vmx_vmwrite(GUEST_GDTR_BASE, (ULONG_PTR)state->SpecialRegisters.Gdtr.Base);
	__vmx_vmwrite(GUEST_GDTR_LIMIT, state->SpecialRegisters.Gdtr.Limit);
	__vmx_vmwrite(HOST_GDTR_BASE, (ULONG_PTR)state->SpecialRegisters.Gdtr.Base);

	// IDT
	__vmx_vmwrite(GUEST_IDTR_BASE, (ULONG_PTR)state->SpecialRegisters.Idtr.Base);
	__vmx_vmwrite(GUEST_IDTR_LIMIT, state->SpecialRegisters.Idtr.Limit);
	__vmx_vmwrite(HOST_IDTR_BASE, (ULONG_PTR)state->SpecialRegisters.Idtr.Base);

	// CR0
	__vmx_vmwrite(CR0_READ_SHADOW, state->SpecialRegisters.Cr0);
	__vmx_vmwrite(HOST_CR0, state->SpecialRegisters.Cr0);
	__vmx_vmwrite(GUEST_CR0, state->SpecialRegisters.Cr0);

	//CR3�˴���ע��������д�������ǿ���DPC����Ĳ�����Ҳ�������Ǵ����������CR3,����Ҳ����ֱ��ͨ��__readmsrָ���ȡ,���ݼ��ݼ���
	__vmx_vmwrite(HOST_CR3,  __readcr3());
	__vmx_vmwrite(GUEST_CR3, __readcr3());

	// CR4
	__vmx_vmwrite(HOST_CR4, state->SpecialRegisters.Cr4);
	__vmx_vmwrite(GUEST_CR4, state->SpecialRegisters.Cr4);
	__vmx_vmwrite(CR4_GUEST_HOST_MASK, 0x2000);
	__vmx_vmwrite(CR4_READ_SHADOW, state->SpecialRegisters.Cr4 & ~0x2000);

	// Debug MSR and DR7
	__vmx_vmwrite(GUEST_IA32_DEBUGCTL, state->SpecialRegisters.DebugControl);
	__vmx_vmwrite(GUEST_DR7, state->SpecialRegisters.KernelDr7);


	//�������бȽϹؼ�,һ����VM�����,һ������VMM�����. VM��ʵ����������ʵ��CPU,��VMM����CPUִ��ĳЩָ�������뵽�Ĵ�������
	//VM���ǲ�����Ҫ��ô����,ֻ��Ҫ������Ӧ����Ϣ����;��VMM��������Ҫ��д��Ӧ����,�������,������׵����������������

	// ������Guest�����ݣ�������Native�����������������Ϣ
	__vmx_vmwrite(GUEST_RSP, state->ContextFrame.Rsp);
	__vmx_vmwrite(GUEST_RIP, state->ContextFrame.Rip);
	__vmx_vmwrite(GUEST_RFLAGS, state->ContextFrame.EFlags);

	
	//VMM����ں����Ķ�ջ
	__vmx_vmwrite(HOST_RSP, (ULONG_PTR)Vcpu->VMMStack + KERNEL_STACK_SIZE - sizeof(MYCONTEXT) - 0x80);
	__vmx_vmwrite(HOST_RSP, (ULONG_PTR)Vcpu->VMMStack + KERNEL_STACK_SIZE - sizeof(VOID*)*2);
	__vmx_vmwrite(HOST_RIP, (ULONG_PTR)AsmVmmEntryPoint);
}



/*
���������֮ǰ,����ʹ��CPU����VMX Rootģʽ,����ִ�к����VMXָ��
*/
BOOLEAN VmxEnterRoot(IN PVCPU Vcpu)
{
	IA32_VMX_BASIC_MSR pBasic;
	LARGE_INTEGER cr0Fix0 = { 0 }, cr0Fix1 = { 0 }, cr4Fix0 = { 0 }, cr4Fix1 = { 0 };

	//��ȡ��MSR�Ĵ����е�ֵ
	pBasic.All = __readmsr(MSR_IA32_VMX_BASIC);
	cr0Fix0.QuadPart = __readmsr(MSR_IA32_VMX_CR0_FIXED0);
	cr0Fix1.QuadPart = __readmsr(MSR_IA32_VMX_CR0_FIXED1);
	cr4Fix0.QuadPart = __readmsr(MSR_IA32_VMX_CR4_FIXED0);
	cr4Fix1.QuadPart = __readmsr(MSR_IA32_VMX_CR4_FIXED1);

	//��ȡVT�İ汾��ʶ��ֵ��VMCS��VMXON����,��������һ��,��VMCS��VMXON�ڴ�Ҫ��
	Vcpu->VMXON->RevisionId = pBasic.Fields.RevisionIdentifier;
	Vcpu->VMCS->RevisionId = pBasic.Fields.RevisionIdentifier;

	//����Intel�ֲ�ĸ�¼��CR0��CR4�Ĵ����е�һЩλ����Ϊ0�ͱ���Ϊ1��Ҫ��
	Vcpu->HostState.SpecialRegisters.Cr0 &= cr0Fix1.LowPart;
	Vcpu->HostState.SpecialRegisters.Cr0 |= cr0Fix0.LowPart;

	Vcpu->HostState.SpecialRegisters.Cr4 &= cr4Fix1.LowPart;
	Vcpu->HostState.SpecialRegisters.Cr4 |= cr4Fix0.LowPart;


	//����CR0��CR4�Ĵ���
	__writecr0(Vcpu->HostState.SpecialRegisters.Cr0);
	__writecr4(Vcpu->HostState.SpecialRegisters.Cr4);

	//����VMXģʽ
	//VMX_ONָ��Ĳ��������������VMXON����������ַ
	PHYSICAL_ADDRESS phys = MmGetPhysicalAddress(Vcpu->VMXON);
	int res = __vmx_on((PULONG64)&phys);
	if (res)
	{
		DbgPrint("VTFrame:__vmx_onָ��ִ��ʧ�ܣ�%d",res);
		return FALSE;
	}

	// ���VMCS��״̬����������Ϊ����Ծ��
	phys = MmGetPhysicalAddress(Vcpu->VMCS);
	if (__vmx_vmclear((PULONG64)&phys))
	{
		DbgPrint("VTFrame:__vmx_vmclearָ��ִ��ʧ��");
		return FALSE;
	}

	// ����VMCS��������״̬����Ϊ��Ծ��
	if (__vmx_vmptrld((PULONG64)&phys))
	{
		DbgPrint("VTFrame:__vmx_vmclearָ��ִ��ʧ��");
		return FALSE;
	}

	//VMX Rootģʽ�����ã�����VMCS�ǻ�Ծ��
	return TRUE;
}

VOID VmxSubvertCPU(IN PVCPU Vcpu)
{
	PHYSICAL_ADDRESS phys = { 0 };
	phys.QuadPart = MAXULONG64;

	//ǰ����Ȼ����Ҳ��������ȫ���ڴ�,���������������g_data���ڴ棬��g_data�ĳ�ԱVCPU�еĳ�Ա�󲿷���ָ��
	//������������Ҫ����ָ����ָ����ڴ�

	//����VMX������ڴ�����,�����Ǵ�С������ķ�Χ
	Vcpu->VMXON = MmAllocateContiguousMemory(sizeof(VMX_VMCS), phys);
	Vcpu->VMCS = MmAllocateContiguousMemory(sizeof(VMX_VMCS), phys);
	Vcpu->VMMStack = MmAllocateContiguousMemory(KERNEL_STACK_SIZE, phys);

	if (!Vcpu->VMXON || !Vcpu->VMCS || !Vcpu->VMMStack)
	{
		DbgPrint("VTFrame:VMX�ڴ���������ʧ��\n");
		goto failed;
	}

	//�����Ƿ�ҳ�ڴ�   ˵ʵ��,�Ҳ�̫����ʲô��˼ MDL��ûŪ������������
	UtilProtectNonpagedMemory(Vcpu->VMXON, sizeof(VMX_VMCS), PAGE_READWRITE);
	UtilProtectNonpagedMemory(Vcpu->VMCS, sizeof(VMX_VMCS), PAGE_READWRITE);
	UtilProtectNonpagedMemory(Vcpu->VMMStack, KERNEL_STACK_SIZE, PAGE_READWRITE);

	//����ڴ�����
	RtlZeroMemory(Vcpu->VMXON, sizeof(VMX_VMCS));
	RtlZeroMemory(Vcpu->VMCS, sizeof(VMX_VMCS));
	RtlZeroMemory(Vcpu->VMMStack, KERNEL_STACK_SIZE);

	// ��ͼ������������Ͻ���VMXģʽ
	if (VmxEnterRoot(Vcpu))
	{
		//VMCS�������������
		VmxSetupVMCS(Vcpu);
	
		// ����EPTҳ��
		Vcpu->ept_PML4T = BuildEPTTable();

		////����EPT����
		EptEnable(Vcpu->ept_PML4T);

		//��vmlauch֮ǰ����CPU��״̬����������ɹ������������������ĵ�Native����������״̬��ΪON
		Vcpu->VmxState = VMX_STATE_TRANSITION;

		DbgPrint("VTFrame:CPU:%d:���ڿ���VT\n", CPU_IDX);
		
		//CPU����+1
		InterlockedIncrement(&g_data->vcpus);
		int res = __vmx_vmlaunch();
		
		
		//ִ�е�����ͱ�ʾ����VTʧ����,CPU����-1
		InterlockedDecrement(&g_data->vcpus);
		Vcpu->VmxState = VMX_STATE_OFF;

		DbgPrint("VTFrame:CPU:%d:__vmx_vmlaunchִ��ʧ��,������:%d", CPU_IDX, res);

		//�ر�VMXģʽ
		__vmx_off();

	}

	//�ͷ��ڴ�
failed:;
	if (Vcpu->VMXON)
		MmFreeContiguousMemory(Vcpu->VMXON);
	if (Vcpu->VMCS)
		MmFreeContiguousMemory(Vcpu->VMCS);
	if (Vcpu->VMMStack)
		MmFreeContiguousMemory(Vcpu->VMMStack);

	Vcpu->VMXON = NULL;
	Vcpu->VMCS = NULL;
	Vcpu->VMMStack = NULL;
}

VOID VmxShutdown(IN PVCPU Vcpu)
{
	//�ȵ�VMM�н��д���
	__vmx_vmcall(VTFrame_UNLOAD, 0, 0, 0);
	VmxVMCleanup(KGDT64_R3_DATA | RPL_MASK, KGDT64_R3_CMTEB | RPL_MASK);

	//�ͷŵ�VCPU�е�VMXON VMCS VMMStack���ڴ�
	if (Vcpu->VMXON)
		MmFreeContiguousMemory(Vcpu->VMXON);
	if (Vcpu->VMCS)
		MmFreeContiguousMemory(Vcpu->VMCS);
	if (Vcpu->VMMStack)
		MmFreeContiguousMemory(Vcpu->VMMStack);
	
	//��ָ������ΪNULL
	Vcpu->VMXON = NULL;
	Vcpu->VMCS = NULL;
	Vcpu->VMMStack = NULL;
}


inline VOID IntelRestoreCPU(IN PVCPU Vcpu)
{
	// ��ǰCPU������VT��ж��
	if (Vcpu->VmxState > VMX_STATE_OFF)
		VmxShutdown(Vcpu);
}

VOID VmxInitializeCPU(IN PVCPU Vcpu, IN ULONG64 SystemDirectoryTableBase)
{
	//�˺������Ա���һЩ������дVMCS������һЩ����Ĵ�����ֵ
	KeSaveStateForHibernate(&Vcpu->HostState);
	
	//�˺���������һЩ��������Ϣ����RIP��RSP��һЩͨ�üĴ�����ֵ
	RtlCaptureContext(&Vcpu->HostState.ContextFrame);
	
	//������ִ��vmlauch����vm entry������,Ҳ����RtlCaptureContext��������һ��
	//��ΪVMCS���GUEST_RIP����������RtlCaptureContext�����

	
	//ÿ��CPU�Ľṹ����һ����ʶCPU����״̬�ı���VmxState��
	//����ȡֵ��ʼ��Ϊ0��Ҳ����VMX_STATE_OFF����vmlauchָ��ִ��ǰ������ֵ����ֵΪVMX_STATE_TRANSITION
	if (g_data->cpu_data[CPU_IDX].VmxState == VMX_STATE_TRANSITION)
	{
		//�������ʾCPUִ��vmlauch�ɹ���
		//��CPU��״̬��ʶΪVMX_STATE_ON
		g_data->cpu_data[CPU_IDX].VmxState = VMX_STATE_ON;
		//�������������RtlCaptureContext���Ӧ�������Ǳ��棬��������ǻָ�
		//�ٴλָ������ģ�����������RtlCaptureContext��������,�������൱��һ��goto����if�жϵ�����
		RtlRestoreContext(&g_data->cpu_data[CPU_IDX].HostState.ContextFrame, NULL);
	}
	else if (g_data->cpu_data[CPU_IDX].VmxState == VMX_STATE_OFF)
	{
		
		//�������ʾ��CPU�ǻ�û�п���VT
		//��CR3���浽CPU�ṹVCPU��
		Vcpu->SystemDirectoryTableBase = SystemDirectoryTableBase;
		//����VT����Ҫ���ݣ�VMX�߸�CPU����Ȩ
		VmxSubvertCPU(Vcpu);
	}
	
	//������ͱ�ʾ���CPU����VT�ɹ��ˡ�������������
}

inline VOID IntelSubvertCPU(IN PVCPU Vcpu,IN PVOID SystemDirectoryTableBase)
{
	VmxInitializeCPU(Vcpu,(ULONG64)SystemDirectoryTableBase);
}


//����VT��ж��VT��DPC����
//����ڶ�������Ϊ�������������CR3�����ǿ���VT
//ΪNULL������ж��VT
/*
VOID HvmpHVCallbackDPC(PRKDPC Dpc, PVOID Context, PVOID SystemArgument1, PVOID SystemArgument2)
{
	//��ȡ��ǰCPU��VCPU�ṹ
	PVCPU pVCPU = &g_data->cpu_data[CPU_IDX];

	//ARGUMENT_PRESENT�����жϲ����Ƿ�ΪNULL
	if (ARGUMENT_PRESENT(Context))
	{
		//����VT
		//���뵱ǰCPU��VCPU�ṹ���������������CR3
		IntelSubvertCPU(pVCPU,Context);
	}
	else
	{
		//ж��VT
		//���뵱ǰCPU��VCPU�ṹ
		IntelRestoreCPU(pVCPU);
	}

	
	//�ȴ����е�DPCͬ��
	KeSignalCallDpcSynchronize(SystemArgument2);
	//���DPC״̬Ϊ�����
	KeSignalCallDpcDone(SystemArgument1);
}
*/

/*
�ߵ�����������ʹ����ǲ�ͬ��CPU�ڵ����ˣ���������͵��õĺ����������Ҫ��Ӷ�˴���
*/
VOID SetupVT(PVOID Context)
{
	
	//��ȡ��ǰCPU��VCPU�ṹ
	PVCPU pVCPU = &g_data->cpu_data[CPU_IDX];

	//ARGUMENT_PRESENT�����жϲ����Ƿ�ΪNULL
	if (ARGUMENT_PRESENT(Context))
	{
		//����VT
		//���뵱ǰCPU��VCPU�ṹ���������������CR3
		IntelSubvertCPU(pVCPU, Context);
	}
	else
	{
		//ж��VT
		//���뵱ǰCPU��VCPU�ṹ
		IntelRestoreCPU(pVCPU);
	}
}

/************************************************************************/
/* ��ÿһ���߼�CPU�������ڴ棬����VCPU����ṹ�Ĵ�С*CPU����+ULONG��С��CPU����*/
/************************************************************************/
BOOLEAN  AllocGlobalMemory()
{
	//��ȡCPU��
	ULONG cpu_count = KeNumberProcessors;

	
	//ȫ�ֱ���g_data��һ��CPU������CPU�ṹ������Ľṹ��

	//���������ǵȼ۵ģ�FIELD_OFFSET(type,field)��������ǻ�ȡtype�ṹ���г�field�ֶ���,�����ֶε��ܴ�С
	//Ϊ�˿���չ�ԣ�Ӧ��ʹ���������
	ULONG_PTR size = FIELD_OFFSET(GLOBAL_DATA, cpu_data) + cpu_count * sizeof(VCPU);
	//ULONG_PTR size = sizeof(LONG) + cpu_count * sizeof(VCPU);

	//�����ڴ�
	g_data = (PGLOBAL_DATA)ExAllocatePoolWithTag(NonPagedPoolNx, size, VF_POOL_TAG);
	RtlZeroMemory(g_data, size);

	//MSRBitmap
	g_data->MSRBitmap = ExAllocatePoolWithTag(NonPagedPoolNx, PAGE_SIZE, VF_POOL_TAG);
	RtlZeroMemory(g_data->MSRBitmap, PAGE_SIZE);


	if (g_data == NULL)
	{
		DbgPrint("VTFrame:ȫ�ֱ����ڴ�����ʧ��\n");
		return FALSE;
	}

	DbgPrint("VTFrame:ȫ�ֱ����ڴ�����ɹ�\n");
	return TRUE;
}



VOID FreeGlobalData(IN PGLOBAL_DATA pData)
{
	if (pData == NULL)
		return;

	ULONG cpu_count = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
	for (ULONG i = 0; i < cpu_count; i++)
	{
		PVCPU Vcpu = &pData->cpu_data[i];
		if (Vcpu->VMXON)
			MmFreeContiguousMemory(Vcpu->VMXON);
		if (Vcpu->VMCS)
			MmFreeContiguousMemory(Vcpu->VMCS);
		if (Vcpu->VMMStack)
			MmFreeContiguousMemory(Vcpu->VMMStack);

	}

	ExFreePoolWithTag(pData, VF_POOL_TAG);
}

BOOLEAN StartVT()
{

	for (int i = 0; i < KeNumberProcessors; i++)
	{
		KeSetSystemAffinityThread((KAFFINITY)(1 << i));

		//������ʵ���ô���cr3 Ϊ�˼��ݣ����ø���
		SetupVT((PVOID)__readcr3());

		KeRevertToUserAffinityThread();
	}
	// �����淽����������VT�Ķ�˴���ʱ������̻߳�ͬʱ���ã�����EPTҳ���ڴ������ʱ�Ῠ�٣������������Ǹ���

	//�����ں�ģ�鵼����API KeGenericCallDpc����һ��DPC,�������뱾�����CR3
	//�����������������ÿһ��CPU���������DPC���̣���������ʵ��VT�Զ�˵�֧��
	//����CR3����ΪDPC���̿����Ǳ���ں˻�Ӧ�ò�ĳ����ڵ��ã�����ܻᵼ������дVMCS��ʱ��CR3����д���ִ���

	//KeGenericCallDpc(HvmpHVCallbackDPC,(PVOID)__readcr3());
	return TRUE;
}