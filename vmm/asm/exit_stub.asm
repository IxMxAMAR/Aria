; vmm/asm/exit_stub.asm — VM exit handler entry point

EXTERN vmm_handle_exit:PROC

.code

vm_exit_stub PROC
    ; Interrupts are disabled for the duration of the handler. VM entry restores
    ; the guest's RFLAGS from GUEST_RFLAGS, so the guest's IF is unaffected.
    cli
    push    r15
    push    r14
    push    r13
    push    r12
    push    r11
    push    r10
    push    r9
    push    r8
    push    rdi
    push    rsi
    push    rbp
    sub     rsp, 8
    push    rbx
    push    rdx
    push    rcx
    push    rax

    mov     rcx, rsp

    sub     rsp, 20h
    call    vmm_handle_exit
    add     rsp, 20h

    pop     rax
    pop     rcx
    pop     rdx
    pop     rbx
    add     rsp, 8
    pop     rbp
    pop     rsi
    pop     rdi
    pop     r8
    pop     r9
    pop     r10
    pop     r11
    pop     r12
    pop     r13
    pop     r14
    pop     r15

    vmresume

    ; VMRESUME failed — read error code and store at address 0xB8000
    ; (VGA text buffer — visible even without UEFI console)
    mov     rdx, 04400h         ; VMCS_VM_INSTRUCTION_ERROR
    vmread  rax, rdx            ; rax = error code
    mov     rdx, 0B8000h        ; VGA text buffer
    ; Write "E=" then error digit to VGA
    mov     word ptr [rdx], 0C45h    ; 'E' green on black
    mov     word ptr [rdx+2], 0C3Dh  ; '=' green on black
    add     al, 30h                   ; convert to ASCII digit
    mov     ah, 0Ch                   ; green attribute
    mov     word ptr [rdx+4], ax     ; error digit

    ; Also try: write to serial port 0x3F8 directly
    mov     dx, 03F8h
    mov     al, 52h    ; 'R' for resume-fail
    out     dx, al

    cli
    hlt
    jmp     $

vm_exit_stub ENDP

; host_fault_halt — any exception taken in host context lands here. The IDT
; built by efi/loader.c points every vector at this routine, so a host-side
; fault halts visibly instead of triple-faulting the machine.
host_fault_halt PROC
    mov     rdx, 0B8000h
    mov     word ptr [rdx], 0C48h      ; 'H'
    mov     word ptr [rdx+2], 0C46h    ; 'F'
    cli
    hlt
    jmp     $
host_fault_halt ENDP

; aria_vmcall — issue a hypercall with the call number in rcx and the token in rdx.
; Returns the hypervisor's status in rax.
aria_vmcall PROC
    mov     rax, rcx
    mov     rcx, rdx
    vmcall
    ret
aria_vmcall ENDP

; aria_atomic_add64 — atomically add rdx to the qword at rcx.
; Returns the value the location held before the add in rax.
; The exit counters and the TSC-offset accumulator are shared by every processor
; once the APs are launched, so the read-modify-write has to be indivisible.
aria_atomic_add64 PROC
    mov     rax, rdx
    lock xadd qword ptr [rcx], rax
    ret
aria_atomic_add64 ENDP

END
