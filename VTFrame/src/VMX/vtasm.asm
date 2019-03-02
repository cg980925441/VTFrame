extern VmxpExitHandler:proc
extern RtlCaptureContext:proc

.CODE

;win10��֧��RtlCaptureContext����Ϊwin10����ָ�����RtlCaptureContext�����˸Ķ�������ĳЩ����£��޷�ִ�е�vmresume
VmxVMEntry PROC
    push    rcx                 ; save RCX, as we will need to orverride it
    lea     rcx, [rsp+8h]       ; store the context in the stack, bias for
                                ; the return address and the push we just did.
    call    RtlCaptureContext   ; save the current register state.
                                ; note that this is a specially written function
                                ; which has the following key characteristics:
                                ;   1) it does not taint the value of RCX
                                ;   2) it does not spill any registers, nor
                                ;      expect home space to be allocated for it

    jmp     VmxpExitHandler     ; jump to the C code handler. we assume that it
                                ; compiled with optimizations and does not use
                                ; home space, which is true of release builds.
VmxVMEntry ENDP

; �궨��push����ͨ�üĴ���
PUSHAQ MACRO
    push    rax
    push    rcx
    push    rdx
    push    rbx
    push    -1      ; ռ��λ�������rsp�϶�����Guest��Rsp��
    push    rbp
    push    rsi
    push    rdi
    push    r8
    push    r9
    push    r10
    push    r11
    push    r12
    push    r13
    push    r14
    push    r15
ENDM


; �궨��pop����ͨ�üĴ���
POPAQ MACRO
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     rdi
    pop     rsi
    pop     rbp
    add     rsp, 8    ; ��pop��rsp����ΪAsmVmmEntryPointǰ���ж�ջƽ�����,����rsp����ԭ����GuestRsp
    pop     rbx
    pop     rdx
    pop     rcx
    pop     rax
ENDM

;����HyperPlatform��д��,�����Լ��ֶ�����һ��CONTENT�ṹ
AsmVmmEntryPoint PROC

	;����һ��ͨ�üĴ���
    PUSHAQ                  ; -8 * 16=80H

	;rcx����ͨ�üĴ����ṹMYCONTEXT�ṹ��ָ���ˣ�����VmxpExitHandler
    mov rcx, rsp

    ; ����һ����ʧ�Ĵ���xmm0-xmm5
    sub rsp, 60h		;10h*6=60h
    movaps xmmword ptr [rsp +  0h], xmm0
    movaps xmmword ptr [rsp + 10h], xmm1
    movaps xmmword ptr [rsp + 20h], xmm2
    movaps xmmword ptr [rsp + 30h], xmm3
    movaps xmmword ptr [rsp + 40h], xmm4
    movaps xmmword ptr [rsp + 50h], xmm5

	; Ԥ��һ�¶�ջ�ռ�
    sub rsp, 20h
    call     VmxpExitHandler
    add rsp, 20h

    movaps xmm0, xmmword ptr [rsp +  0h]
    movaps xmm1, xmmword ptr [rsp + 10h]
    movaps xmm2, xmmword ptr [rsp + 20h]
    movaps xmm3, xmmword ptr [rsp + 30h]
    movaps xmm4, xmmword ptr [rsp + 40h]
    movaps xmm5, xmmword ptr [rsp + 50h]
    add rsp, 60h

    POPAQ

	; ִ�е������ջ�ͼĴ������Ѿ��ͷ���VM-Extitʱ��һ����
    vmresume

	; ������ͱ�ʾvmresumeʧ����
    int 3
AsmVmmEntryPoint ENDP

VmxVMCleanup PROC
    mov     ds, cx              ; set DS to parameter 1
    mov     es, cx              ; set ES to parameter 1
    mov     fs, dx              ; set FS to parameter 2
    ret                         ; return
VmxVMCleanup ENDP

VmxpResume PROC 
    vmresume
    ret
VmxpResume ENDP

__vmx_vmcall PROC
    vmcall
    ret
__vmx_vmcall ENDP

__invept PROC
    invept rcx, OWORD PTR [rdx]
    ret
__invept ENDP

__invvpid PROC
    invvpid rcx, OWORD PTR [rdx]
    ret
__invvpid ENDP

AsmWriteCR2 PROC
    mov cr2, rcx
    ret
AsmWriteCR2 ENDP

PURGE PUSHAQ
PURGE POPAQ

END