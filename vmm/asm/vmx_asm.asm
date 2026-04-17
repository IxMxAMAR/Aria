; vmm/asm/vmx_asm.asm — VMX intrinsic stubs for MASM x64

.code

vmx_do_launch PROC
    pushfq
    push    rbx
    push    rsi
    push    rdi
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15

    mov     rax, rsp
    mov     rdx, 0681Ch
    vmwrite rdx, rax
    jc      launch_fail
    jz      launch_fail

    lea     rax, guest_resume
    mov     rdx, 0681Eh
    vmwrite rdx, rax
    jc      launch_fail
    jz      launch_fail

    vmlaunch

launch_fail:
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rdi
    pop     rsi
    pop     rbx
    popfq
    mov     eax, 1
    ret

guest_resume:
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rdi
    pop     rsi
    pop     rbx
    popfq
    xor     eax, eax
    ret

vmx_do_launch ENDP

__read_cs PROC
    mov     ax, cs
    ret
__read_cs ENDP

__read_ss PROC
    mov     ax, ss
    ret
__read_ss ENDP

__read_ds PROC
    mov     ax, ds
    ret
__read_ds ENDP

__read_es PROC
    mov     ax, es
    ret
__read_es ENDP

__read_fs PROC
    mov     ax, fs
    ret
__read_fs ENDP

__read_gs PROC
    mov     ax, gs
    ret
__read_gs ENDP

__read_tr PROC
    str     ax
    ret
__read_tr ENDP

__read_ldtr PROC
    sldt    ax
    ret
__read_ldtr ENDP

__read_rflags PROC
    pushfq
    pop     rax
    ret
__read_rflags ENDP

__sgdt PROC
    sgdt    [rcx]
    ret
__sgdt ENDP

__sidt PROC
    sidt    [rcx]
    ret
__sidt ENDP

__invept PROC
    invept  rcx, oword ptr [rdx]
    ret
__invept ENDP

; INVVPID instruction wrapper
; void __invvpid(uint64_t type, void* descriptor)
__invvpid PROC
    invvpid rcx, oword ptr [rdx]
    ret
__invvpid ENDP

; VMCLEAR instruction wrapper
; uint8_t __vmx_vmclear(uint64_t* pa)
__vmx_vmclear PROC
    vmclear qword ptr [rcx]
    setc    al
    ret
__vmx_vmclear ENDP

; VMXON instruction wrapper
; uint8_t __vmx_on(uint64_t* pa)
__vmx_on PROC
    vmxon   qword ptr [rcx]
    setc    al
    ret
__vmx_on ENDP

; VMPTRLD instruction wrapper
; uint8_t __vmx_vmptrld(uint64_t* pa)
__vmx_vmptrld PROC
    vmptrld qword ptr [rcx]
    setc    al
    ret
__vmx_vmptrld ENDP

; XSETBV instruction wrapper
; void __xsetbv(uint32_t idx, uint64_t val)
; Windows x64 ABI: rcx=idx, rdx=val
; XSETBV expects: ecx=idx, edx:eax=val
__xsetbv PROC
    mov     eax, edx
    shr     rdx, 32
    xsetbv
    ret
__xsetbv ENDP

; RDTSC wrapper
; uint64_t __rdtsc(void)
__rdtsc PROC
    rdtsc
    shl     rdx, 32
    or      rax, rdx
    ret
__rdtsc ENDP

; XSAVE instruction wrapper
; void __xsave(void* area, uint64_t mask)
; Windows x64 ABI: rcx=area, rdx=mask
; XSAVE expects: edx:eax=feature mask, [rcx]=save area
__xsave PROC
    mov     eax, edx
    shr     rdx, 32
    xsave   [rcx]
    ret
__xsave ENDP

; XRSTOR instruction wrapper
; void __xrstor(void* area, uint64_t mask)
__xrstor PROC
    mov     eax, edx
    shr     rdx, 32
    xrstor  [rcx]
    ret
__xrstor ENDP

; LTR instruction wrapper
; void __load_tr(uint16_t sel)
__load_tr PROC
    ; rcx = selector (uint16_t), push to stack and use memory operand
    push    rcx
    ltr     word ptr [rsp]
    pop     rcx
    ret
__load_tr ENDP

; LGDT instruction wrapper
; void __lgdt(void* gdtr)
__lgdt PROC
    ; LGDT m16&64 — ml64 can't infer size, encode manually
    ; 0F 01 /2 with ModRM [rcx] = 0F 01 11
    db      0Fh, 01h, 11h
    ret
__lgdt ENDP

END
