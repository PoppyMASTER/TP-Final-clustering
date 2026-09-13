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

msg1 db "[BOOT] MiniOS demarrage...", 0x0A, 0
msg2 db "[OS]   Hello from inside KVM!", 0x0A, 0
msg3 db "[OS]   Tache 1 : OK", 0x0A, 0
msg4 db "[OS]   Arret sur HLT.", 0x0A, 0
