BITS 16
ORG 0x1000

start:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7000

    mov si, msg_boot
    call puts
    mov si, msg_ready
    call puts
    mov si, msg_prompt
    call puts

main_loop:
    in al, 0x3F8
    test al, al
    jz main_loop

    out 0x3F8, al

    cmp al, 'h'
    je cmd_help
    cmp al, 's'
    je cmd_status
    cmp al, 'r'
    je cmd_reboot
    cmp al, 'q'
    je cmd_quit

    mov si, msg_unknown
    call puts
    mov si, msg_prompt
    call puts
    jmp main_loop

cmd_help:
    mov si, msg_nl
    call puts
    mov si, msg_help
    call puts
    mov si, msg_prompt
    call puts
    jmp main_loop

cmd_status:
    mov si, msg_nl
    call puts
    mov si, msg_status
    call puts
    mov si, msg_prompt
    call puts
    jmp main_loop

cmd_reboot:
    mov si, msg_nl
    call puts
    mov si, msg_reboot
    call puts
    jmp start

cmd_quit:
    mov si, msg_nl
    call puts
    mov si, msg_quit
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

msg_boot    db 13, 10, "=====================================", 13, 10
            db "   MiniOS Loop v1.0 / MiniHV / KVM   ", 13, 10
            db "=====================================", 13, 10, 0
msg_ready   db "[BOOT] Systeme pret.", 13, 10, 0
msg_help    db "  h = aide  s = statut  r = reboot  q = quitter", 13, 10, 0
msg_status  db "  CPU: mode reel 16 bits", 13, 10
            db "  RAM: 128 Mo allouee", 13, 10
            db "  Port serie: COM1 0x3F8", 13, 10
            db "  Hyperviseur: MiniHV/KVM", 13, 10, 0
msg_reboot  db "[REBOOT] Redemarrage...", 13, 10, 0
msg_quit    db "[QUIT] Arret. HLT.", 13, 10, 0
msg_unknown db 13, 10, "  Commande inconnue. Tapez h.", 13, 10, 0
msg_prompt  db "MiniOS> ", 0
msg_nl      db 13, 10, 0
