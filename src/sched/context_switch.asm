;
;  This file is part of Permafrost Engine. 
;  Copyright (C) 2023 Eduard Permyakov 
;
;  Permafrost Engine is free software: you can redistribute it and/or modify
;  it under the terms of the GNU General Public License as published by
;  the Free Software Foundation, either version 3 of the License, or
;  (at your option) any later version.
;
;  Permafrost Engine is distributed in the hope that it will be useful,
;  but WITHOUT ANY WARRANTY; without even the implied warranty of
;  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;  GNU General Public License for more details.
;
;  You should have received a copy of the GNU General Public License
;  along with this program.  If not, see <http://www.gnu.org/licenses/>.
; 
;  Linking this software statically or dynamically with other modules is making 
;  a combined work based on this software. Thus, the terms and conditions of 
;  the GNU General Public License cover the whole combination. 
;  
;  As a special exception, the copyright holders of Permafrost Engine give 
;  you permission to link Permafrost Engine with independent modules to produce 
;  an executable, regardless of the license terms of these independent 
;  modules, and to copy and distribute the resulting executable under 
;  terms of your choice, provided that you also meet, for each linked 
;  independent module, the terms and conditions of the license of that 
;  module. An independent module is a module which is not derived from 
;  or based on Permafrost Engine. If you modify Permafrost Engine, you may 
;  extend this exception to your version of Permafrost Engine, but you are not 
;  obliged to do so. If you do not wish to do so, delete this exception 
;  statement from your version.
;
;

; PUBLIC sched_switch_ctx
; PUBLIC sched_task_exit_trampoline

extern sched_task_exit: PROC

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

sched_task_exit_trampoline PROC
    mov rcx, rax
    jmp sched_task_exit
sched_task_exit_trampoline ENDP

_TEXT ENDS

END


