; boot.asm - 引导扇区 (LBA=1 读取 kernel)
BITS 16
ORG 0x7C00

KERNEL_LOAD_SEG  equ 0x1000
KERNEL_LOAD_ADDR equ 0x10000
KERNEL_SECTORS   equ 128

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    mov [boot_drive], dl

    ; 显示 'R'
    mov ah, 0x0E
    mov al, 'R'
    int 0x10

    ; ---- 读盘循环 ----
    mov si, 1               ; 关键修复：从 LBA=1 开始（kernel 紧跟在 boot 后）
    mov di, KERNEL_SECTORS
    xor bp, bp

read_loop:
    ; LBA -> CHS
    mov ax, si
    xor dx, dx
    mov bx, 18
    div bx
    mov cx, dx
    inc cl
    xor dx, dx
    mov bx, 2
    div bx
    mov ch, al
    mov dh, dl

    pusha
    push es

    mov ax, KERNEL_LOAD_SEG
    mov es, ax
    mov bx, bp
    mov dl, [cs:boot_drive]
    mov ax, 0x0201
    int 0x13
    jc disk_error

    pop es
    popa

    inc si
    add bp, 512
    dec di
    jnz read_loop

    ; 验证 0x10000 处是 kernel 的 _start（0x50 = push eax）
    mov ax, KERNEL_LOAD_SEG
    mov es, ax
    mov al, [es:0]
    cmp al, 0x50
    jne verify_fail

    ; 显示 'L'
    mov ah, 0x0E
    mov al, 'L'
    int 0x10

    ; A20
    in al, 0x92
    or al, 2
    out 0x92, al

    ; GDT
    lgdt [gdt_descriptor]

    ; 保护模式
    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp 0x08:protected_mode_entry

verify_fail:
    mov ah, 0x0E
    mov al, 'E'
    int 0x10
    jmp $

disk_error:
    mov ah, 0x0E
    mov al, 'D'
    int 0x10
    jmp $

boot_drive db 0

gdt_start:
    dd 0x0
    dd 0x0
gdt_code:
    dw 0xFFFF
    dw 0x0
    db 0x0
    db 10011010b
    db 11001111b
    db 0x0
gdt_data:
    dw 0xFFFF
    dw 0x0
    db 0x0
    db 10010010b
    db 11001111b
    db 0x0
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

BITS 32
protected_mode_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000

    ; 32 位下写 VGA：'X'
    mov word [0xB8000], 0x0F58

    jmp 0x08:KERNEL_LOAD_ADDR

times 510-($-$$) db 0
dw 0xAA55
