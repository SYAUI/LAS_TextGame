; entry.asm - 32位内核入口（诊断版）
BITS 32
SECTION .text.entry
GLOBAL _start
EXTERN kmain
EXTERN __bss_start
EXTERN __bss_end

%macro VGA_PUT 2
    push eax
    mov al, %1
    mov ah, 0x0F
    mov word [0xB8000 + %2*2], ax
    pop eax
%endmacro

; 关键修复：_start 必须是 .text.entry 的第一个字节（正好在 0x10000） ★★★
_start:
    VGA_PUT 'A', 0

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000

    VGA_PUT 'B', 1

    ; 清 BSS
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    xor eax, eax
    rep stosb

    VGA_PUT 'C', 2

    ; 填 IDT
    mov edi, idt_table
    mov ecx, 256
.fill_idt:
    mov eax, isr_default
    mov word [edi], ax
    mov word [edi+2], 0x08
    mov byte [edi+4], 0
    mov byte [edi+5], 0x8E
    shr eax, 16
    mov word [edi+6], ax
    add edi, 8
    loop .fill_idt

    VGA_PUT 'D', 3

    lidt [idt_descriptor]

    VGA_PUT 'E', 4

    call kmain

    VGA_PUT 'F', 5

    cli
.hang:
    hlt
    jmp .hang

; 移到 _start 之后，不影响入口地址
isr_default:
    cli
    hlt
    jmp isr_default

SECTION .data
idt_descriptor:
    dw 256*8 - 1
    dd idt_table

SECTION .bss
idt_table:
    resb 256*8
