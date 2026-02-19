;
;   IsrStubs.asm
;   Hardware interrupt (IRQ) entry stubs for APIC
;   Copyright (c) 2025 Daniel Hammer
;

[bits 64]
section .text

; External C++ handler function
extern HalIrqDispatch

; Macro to define an IRQ stub
; Each stub pushes the IRQ number and jumps to the common handler
%macro IRQ_STUB 1
global IrqStub%1
IrqStub%1:
    push rax                ; Save rax (we use it for the IRQ number)
    mov  rax, %1            ; IRQ number
    jmp  IrqCommon
%endmacro

; Common IRQ handler: saves all general-purpose registers,
; calls the C++ dispatch function, restores registers, and returns.
IrqCommon:
    ; rax is already on the stack and holds the IRQ number
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Pass IRQ number as first argument (rdi)
    mov  rdi, rax

    ; Align stack to 16 bytes before call (we pushed 15 registers x 8 = 120 bytes
    ; + return address 8 = 128, which is 16-byte aligned, so we're good)
    cld
    call HalIrqDispatch

    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rdi
    pop  rsi
    pop  rbp
    pop  rbx
    pop  rdx
    pop  rcx
    pop  rax

    iretq

; Define stubs for IRQs 0..47 (vectors 32..79)
; 0-23: legacy ISA IRQs via IOAPIC, 24-47: MSI vectors
IRQ_STUB 0
IRQ_STUB 1
IRQ_STUB 2
IRQ_STUB 3
IRQ_STUB 4
IRQ_STUB 5
IRQ_STUB 6
IRQ_STUB 7
IRQ_STUB 8
IRQ_STUB 9
IRQ_STUB 10
IRQ_STUB 11
IRQ_STUB 12
IRQ_STUB 13
IRQ_STUB 14
IRQ_STUB 15
IRQ_STUB 16
IRQ_STUB 17
IRQ_STUB 18
IRQ_STUB 19
IRQ_STUB 20
IRQ_STUB 21
IRQ_STUB 22
IRQ_STUB 23
IRQ_STUB 24
IRQ_STUB 25
IRQ_STUB 26
IRQ_STUB 27
IRQ_STUB 28
IRQ_STUB 29
IRQ_STUB 30
IRQ_STUB 31
IRQ_STUB 32
IRQ_STUB 33
IRQ_STUB 34
IRQ_STUB 35
IRQ_STUB 36
IRQ_STUB 37
IRQ_STUB 38
IRQ_STUB 39
IRQ_STUB 40
IRQ_STUB 41
IRQ_STUB 42
IRQ_STUB 43
IRQ_STUB 44
IRQ_STUB 45
IRQ_STUB 46
IRQ_STUB 47

; Spurious interrupt handler (vector 0xFF) - do nothing, no EOI
global IrqStubSpurious
IrqStubSpurious:
    iretq

; Export the stub table for C++ to reference
section .data
global IrqStubTable
IrqStubTable:
    dq IrqStub0
    dq IrqStub1
    dq IrqStub2
    dq IrqStub3
    dq IrqStub4
    dq IrqStub5
    dq IrqStub6
    dq IrqStub7
    dq IrqStub8
    dq IrqStub9
    dq IrqStub10
    dq IrqStub11
    dq IrqStub12
    dq IrqStub13
    dq IrqStub14
    dq IrqStub15
    dq IrqStub16
    dq IrqStub17
    dq IrqStub18
    dq IrqStub19
    dq IrqStub20
    dq IrqStub21
    dq IrqStub22
    dq IrqStub23
    dq IrqStub24
    dq IrqStub25
    dq IrqStub26
    dq IrqStub27
    dq IrqStub28
    dq IrqStub29
    dq IrqStub30
    dq IrqStub31
    dq IrqStub32
    dq IrqStub33
    dq IrqStub34
    dq IrqStub35
    dq IrqStub36
    dq IrqStub37
    dq IrqStub38
    dq IrqStub39
    dq IrqStub40
    dq IrqStub41
    dq IrqStub42
    dq IrqStub43
    dq IrqStub44
    dq IrqStub45
    dq IrqStub46
    dq IrqStub47
