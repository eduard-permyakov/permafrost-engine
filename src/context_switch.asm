; PUBLIC sched_switch_ctx
; PUBLIC sched_task_exit_trampoline

_TEXT SEGMENT

sched_switch_ctx PROC
Lsave_ctx LABEL DWORD
    lea rax, OFFSET Lback
    push rax
    mov [rcx + 0h], rbx
    mov [rcx + 8h], rsp
    mov [rcx + 10h], rbp
    mov [rcx + 18h], rdi
    mov [rcx + 20h], rsi
    mov [rcx + 28h], r12
    mov [rcx + 30h], r13
    mov [rcx + 38h], r14
    mov [rcx + 40h], r15
    stmxcsr [rcx + 48h]
    fstcw   [rcx + 4ch]
    movdqa [rcx + 50h], xmm6
    movdqa [rcx + 60h], xmm7
    movdqa [rcx + 70h], xmm8
    movdqa [rcx + 80h], xmm9
    movdqa [rcx + 90h], xmm10
    movdqa [rcx + 0a0h], xmm11
    movdqa [rcx + 0b0h], xmm12
    movdqa [rcx + 0c0h], xmm13
    movdqa [rcx + 0d0h], xmm14
    movdqa [rcx + 0e0h], xmm15
Lload_ctx LABEL DWORD
    mov  rbx, [rdx + 0h]
    mov  rsp, [rdx + 8h]
    mov rbp, [rdx + 10h]
    mov rdi, [rdx + 18h]
    mov rsi, [rdx + 20h]
    mov r12, [rdx + 28h]
    mov r13, [rdx + 30h]
    mov r14, [rdx + 38h]
    mov r15, [rdx + 40h]
    ldmxcsr [rdx + 48h]
    fldcw   [rdx + 4ch]
    movdqa xmm6, [rdx + 50h]
    movdqa xmm7, [rdx + 60h]
    movdqa xmm8, [rdx + 70h]
    movdqa xmm9, [rdx + 80h]
    movdqa xmm10, [rdx + 90h]
    movdqa xmm11, [rdx + 0a0h]
    movdqa xmm12, [rdx + 0b0h]
    movdqa xmm13, [rdx + 0c0h]
    movdqa xmm14, [rdx + 0d0h]
    movdqa xmm15, [rdx + 0e0h]
    mov rcx, rdx ; write result to ctx mem
    mov rdx, r9
    mov rax, r8
Lback LABEL DWORD
    ret
sched_switch_ctx ENDP

extern sched_task_exit: PROC

sched_task_exit_trampoline PROC
    mov rcx, rax
    jmp sched_task_exit
sched_task_exit_trampoline ENDP

_TEXT ENDS

END


