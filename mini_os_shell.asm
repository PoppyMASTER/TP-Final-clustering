; mini_os_shell.asm — MiniShell pour MiniHV
BITS 16
ORG 0x1000

%define CMD_BUF     0x5000
%define CWD_BUF     0x5100
%define FS_TABLE    0x5200
%define FS_ENTRY    32
%define FS_MAX      32
%define FS_TYPE_DIR  0x01
%define FS_TYPE_FILE 0x02
%define STACK_TOP   0x7000

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, STACK_TOP
    sti
    mov di, CWD_BUF
    mov byte [di], '/'
    mov byte [di+1], 0
    call fs_init
    mov si, msg_banner
    call puts

shell_loop:
    mov si, str_p1
    call puts
    mov si, CWD_BUF
    call puts
    mov si, str_p2
    call puts
    call readline
    call parse_cmd
    jmp shell_loop

readline:
    mov di, CMD_BUF
    xor cx, cx
.loop:
    in al, 0x3F8
    test al, al
    jz .loop
    cmp al, 0x0D
    je .done
    cmp al, 0x0A
    je .done
    cmp al, 0x08
    je .bs
    cmp al, 0x7F
    je .bs
    cmp cx, 62
    jge .loop
    mov [di], al
    inc di
    inc cx
    out 0x3F8, al
    jmp .loop
.bs:
    test cx, cx
    jz .loop
    dec di
    dec cx
    mov byte [di], 0
    mov al, 0x08
    out 0x3F8, al
    mov al, ' '
    out 0x3F8, al
    mov al, 0x08
    out 0x3F8, al
    jmp .loop
.done:
    mov byte [di], 0
    mov al, 0x0D
    out 0x3F8, al
    mov al, 0x0A
    out 0x3F8, al
    ret

puts:
    lodsb
    test al, al
    jz .done
    out 0x3F8, al
    jmp puts
.done:
    ret

putnl:
    mov al, 0x0D
    out 0x3F8, al
    mov al, 0x0A
    out 0x3F8, al
    ret

skip_sp:
    mov al, [si]
    cmp al, ' '
    jne .done
    inc si
    jmp skip_sp
.done:
    ret

parse_cmd:
    mov si, CMD_BUF
    call skip_sp
    mov al, [si]
    test al, al
    jz .done
    cmp al, 'l'
    jne .p2
    cmp byte [si+1], 's'
    jne .p2
    jmp do_ls
.p2:
    cmp al, 'p'
    jne .p3
    cmp byte [si+1], 'w'
    jne .p3
    jmp do_pwd
.p3:
    cmp al, 'm'
    jne .p4
    cmp byte [si+1], 'k'
    jne .p4
    add si, 6
    call skip_sp
    jmp do_mkdir
.p4:
    cmp al, 't'
    jne .p5
    cmp byte [si+1], 'o'
    jne .p5
    add si, 6
    call skip_sp
    jmp do_touch
.p5:
    cmp al, 'r'
    jne .p6
    cmp byte [si+1], 'm'
    jne .p6
    cmp byte [si+2], ' '
    jne .p6
    add si, 3
    call skip_sp
    jmp do_rm
.p6:
    cmp al, 'c'
    jne .p7
    cmp byte [si+1], 'a'
    jne .p7
    cmp byte [si+2], 't'
    jne .p7
    add si, 4
    call skip_sp
    jmp do_cat
.p7:
    cmp al, 'e'
    jne .p8
    cmp byte [si+1], 'c'
    jne .p8
    add si, 5
    call skip_sp
    jmp do_echo
.p8:
    cmp al, 'c'
    jne .p9
    cmp byte [si+1], 'l'
    jne .p9
    jmp do_clear
.p9:
    cmp al, 'h'
    jne .p10
    jmp do_help
.p10:
    cmp al, 'r'
    jne .p11
    cmp byte [si+1], 'e'
    jne .p11
    jmp do_reboot
.p11:
    cmp al, 'e'
    je do_exit
    cmp al, 'q'
    je do_exit
    mov si, e_unk1
    call puts
    mov si, CMD_BUF
    call puts
    mov si, e_unk2
    call puts
.done:
    ret

fs_init:
    mov di, FS_TABLE
    mov cx, FS_MAX * FS_ENTRY
    xor al, al
    rep stosb
    mov si, d_bin
    mov bl, FS_TYPE_DIR
    call fs_add
    mov si, d_etc
    mov bl, FS_TYPE_DIR
    call fs_add
    mov si, d_home
    mov bl, FS_TYPE_DIR
    call fs_add
    mov si, d_tmp
    mov bl, FS_TYPE_DIR
    call fs_add
    mov si, f_readme
    mov bl, FS_TYPE_FILE
    call fs_add
    mov si, f_conf
    mov bl, FS_TYPE_FILE
    call fs_add
    ret

fs_add:
    mov di, FS_TABLE
    mov cx, FS_MAX
.find:
    mov al, [di]
    test al, al
    jz .found
    add di, FS_ENTRY
    loop .find
    ret
.found:
    mov [di], bl
    push si
    push di
    inc di
.cp:
    mov al, [si]
    mov [di], al
    inc si
    inc di
    test al, al
    jnz .cp
    pop di
    pop si
    ret

fs_find:
    mov di, FS_TABLE
    mov cx, FS_MAX
.loop:
    mov al, [di]
    test al, al
    jz .skip
    push si
    push cx
    push di
    inc di
.cmp:
    mov al, [si]
    mov bl, [di]
    cmp al, bl
    jne .nomatch
    test al, al
    jz .match
    inc si
    inc di
    jmp .cmp
.match:
    pop di
    pop cx
    pop si
    ret
.nomatch:
    pop di
    pop cx
    pop si
.skip:
    add di, FS_ENTRY
    loop .loop
    xor di, di
    ret

do_ls:
    mov si, s_lshdr
    call puts
    mov di, FS_TABLE
    mov cx, FS_MAX
    xor bx, bx
.loop:
    mov al, [di]
    test al, al
    jz .next
    cmp al, FS_TYPE_DIR
    je .isdir
    mov si, s_fpfx
    call puts
    jmp .name
.isdir:
    mov si, s_dpfx
    call puts
.name:
    push di
    push cx
    lea si, [di+1]
    call puts
    call putnl
    pop cx
    pop di
    inc bx
.next:
    add di, FS_ENTRY
    loop .loop
    test bx, bx
    jnz .end
    mov si, s_empty
    call puts
.end:
    ret

do_mkdir:
    mov al, [si]
    test al, al
    jz .noarg
    mov bl, FS_TYPE_DIR
    call fs_add
    ret
.noarg:
    mov si, e_noarg
    call puts
    ret

do_touch:
    mov al, [si]
    test al, al
    jz .noarg
    mov bl, FS_TYPE_FILE
    call fs_add
    ret
.noarg:
    mov si, e_noarg
    call puts
    ret

do_rm:
    mov al, [si]
    test al, al
    jz .noarg
    call fs_find
    test di, di
    jz .notfound
    push cx
    mov cx, FS_ENTRY
    xor al, al
    rep stosb
    pop cx
    ret
.noarg:
    mov si, e_noarg
    call puts
    ret
.notfound:
    mov si, e_notfound
    call puts
    ret

do_cat:
    mov al, [si]
    test al, al
    jz .noarg
    call fs_find
    test di, di
    jz .notfound
    mov al, [di]
    cmp al, FS_TYPE_FILE
    jne .isdir
    mov si, s_catcontent
    call puts
    ret
.isdir:
    mov si, e_isdir
    call puts
    ret
.noarg:
    mov si, e_noarg
    call puts
    ret
.notfound:
    mov si, e_notfound
    call puts
    ret

do_pwd:
    mov si, CWD_BUF
    call puts
    call putnl
    ret

do_echo:
    call puts
    call putnl
    ret

do_clear:
    mov si, s_clear
    call puts
    ret

do_help:
    mov si, s_help
    call puts
    ret

do_reboot:
    mov si, s_reboot
    call puts
    jmp start

do_exit:
    mov si, s_exit
    call puts
    hlt
    jmp $

msg_banner: db 0x0D,0x0A
            db "  +-----------------------------------------+",0x0D,0x0A
            db "  |   MiniShell v1.0 / MiniHV / KVM         |",0x0D,0x0A
            db "  |   ls mkdir touch rm cat pwd echo help   |",0x0D,0x0A
            db "  +-----------------------------------------+",0x0D,0x0A
            db 0x0D,0x0A,0
str_p1:     db "root@minios:",0
str_p2:     db "$ ",0
s_lshdr:    db "total 0",0x0D,0x0A,0
s_empty:    db "(vide)",0x0D,0x0A,0
s_dpfx:     db "drwxr-xr-x  ",0
s_fpfx:     db "-rw-r--r--  ",0
s_catcontent: db "# Fichier MiniOS",0x0D,0x0A,"# Genere par MiniHV/KVM",0x0D,0x0A,0
s_clear:    db 0x1B,"[2J",0x1B,"[H",0
s_help:     db 0x0D,0x0A
            db "  ls              Lister les fichiers",0x0D,0x0A
            db "  mkdir <nom>     Creer un dossier",0x0D,0x0A
            db "  touch <nom>     Creer un fichier",0x0D,0x0A
            db "  rm    <nom>     Supprimer",0x0D,0x0A
            db "  cat   <nom>     Afficher un fichier",0x0D,0x0A
            db "  pwd             Repertoire courant",0x0D,0x0A
            db "  echo  <texte>   Afficher du texte",0x0D,0x0A
            db "  clear           Effacer l ecran",0x0D,0x0A
            db "  reboot          Redemarrer",0x0D,0x0A
            db "  exit            Quitter",0x0D,0x0A,0x0D,0x0A,0
s_reboot:   db "[REBOOT] Redemarrage...",0x0D,0x0A,0
s_exit:     db "[EXIT] Arret. HLT.",0x0D,0x0A,0
e_unk1:     db "minish: ",0
e_unk2:     db ": commande introuvable",0x0D,0x0A,0
e_notfound: db "Aucun fichier ou dossier de ce type",0x0D,0x0A,0
e_isdir:    db "cat: est un dossier",0x0D,0x0A,0
e_noarg:    db "operande manquante",0x0D,0x0A,0
d_bin:      db "bin",0
d_etc:      db "etc",0
d_home:     db "home",0
d_tmp:      db "tmp",0
f_readme:   db "README.txt",0
f_conf:     db "minihv.conf",0
