BITS 16
ORG 0x1000
start:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7000
    mov si, msg1
    call puts
    mov si, msg2
    call puts
    mov si, msg3
    call puts
    mov si, msg4
    call puts
    mov si, msg5
    call puts
    hlt
    jmp $
puts:
    lodsb
    test al, al
    jz .done
    out 0x3F8, al
    jmp puts
.done:
    ret
msg1 db "========================================", 0x0A, 0
msg2 db "   MiniOS v2 - Running inside MiniHV   ", 0x0A, 0
msg3 db "========================================", 0x0A, 0
msg4 db "[OS]   Hello from KVM - tout fonctionne!", 0x0A, 0
msg5 db "[OS]   Arret sur HLT.", 0x0A, 0
