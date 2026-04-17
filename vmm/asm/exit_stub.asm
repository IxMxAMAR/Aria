; vmm/asm/exit_stub.asm — VM exit handler entry point

EXTERN vmm_handle_exit:PROC

.code

vm_exit_stub PROC
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

END
