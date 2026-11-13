; isr.asm - 中断服务程序桩
BITS 32

SECTION .text

; ---- CPU 异常（不带错误码）----
%macro ISR_NOERR 1
global isr%1
isr%1:
    push dword 0
    push dword %1
    jmp isr_common
%endmacro

; ---- CPU 异常（CPU 已压错误码）----
%macro ISR_ERR 1
global isr%1
isr%1:
    push dword %1
    jmp isr_common
%endmacro

; ---- 硬件中断（IRQ0-15 → INT 0x20-0x2F）----
%macro IRQ 2
global irq%1
irq%1:
    push dword 0
    push dword %2
    jmp isr_common
%endmacro

ISR_NOERR 0    ; 除零
ISR_NOERR 1    ; 调试
ISR_NOERR 2    ; NMI
ISR_NOERR 3    ; 断点
ISR_NOERR 4    ; 溢出
ISR_NOERR 5    ; 边界
ISR_NOERR 6    ; 非法指令
ISR_NOERR 7    ; 设备不可用
ISR_ERR   8    ; 双重故障
ISR_NOERR 9    ; 协处理器段越界
ISR_ERR   10   ; 无效 TSS
ISR_ERR   11   ; 段不存在
ISR_ERR   12   ; 栈故障
ISR_ERR   13   ; 通用保护
ISR_ERR   14   ; 缺页
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17   ; 对齐检查
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

; IRQ 0-15 → 向量 0x20-0x2F
IRQ 0,  0x20
IRQ 1,  0x21
IRQ 2,  0x22
IRQ 3,  0x23
IRQ 4,  0x24
IRQ 5,  0x25
IRQ 6,  0x26
IRQ 7,  0x27
IRQ 8,  0x28
IRQ 9,  0x29
IRQ 10, 0x2A
IRQ 11, 0x2B
IRQ 12, 0x2C
IRQ 13, 0x2D
IRQ 14, 0x2E
IRQ 15, 0x2F

; ---- 公共入口 ----
EXTERN isr_handler
isr_common:
    pusha
    push ds
    push es
    push fs
    push gs

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp
    call isr_handler
    add esp, 4

    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8                ; 丢弃 int_no / err_code
    iret