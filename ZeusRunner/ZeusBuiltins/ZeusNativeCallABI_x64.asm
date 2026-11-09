OPTION CASEMAP:NONE

.code
PUBLIC zeus_dynamic_native_invoke

; RCX = native function address, RDX = DynamicNativeCallFrame*
;
; The Windows x64 convention hands out argument slots by position: the n-th
; argument takes the n-th slot, and its class decides whether that slot is
; one of RCX/RDX/R8/R9 or one of XMM0-XMM3. The first four slots are
; registers, the rest are spilled onto the stack in argument order. The
; caller therefore hands over the finished placement rather than the
; argument kinds:
;
;   [frame +  0] values          one 8-byte payload per argument, in order
;   [frame +  8] generalSlots    int32[4]: index into values for RCX/RDX/R8/R9
;   [frame + 16] vectorSlots     int32[4]: index into values for XMM0..XMM3
;   [frame + 24] stackValues     spilled arguments, compacted in argument order
;   [frame + 32] stackCount
;   [frame + 36] returnsFloatingPoint  0 = integer, 1 = double, 2 = float
;   [frame + 40] vectorRegisterCount    number of vector registers used (AL)
;
; A slot index of -1 means the register is unused by this call. A float
; payload occupies the low four bytes of its slot, so copying the whole slot
; into a vector register also leaves the callee with the value it expects.
;
; Local frame (232 bytes):
;   [rsp +   0 ..  31] shadow space required by the Windows x64 ABI
;   [rsp +  32 .. 159] outgoing stack arguments
;   [rsp + 160 .. 167] values base
;   [rsp + 168 .. 175] general slots base
;   [rsp + 176 .. 183] vector slots base
;   [rsp + 184 .. 191] stack values base
;   [rsp + 192 .. 195] stack count
;   [rsp + 196 .. 199] return kind
;   [rsp + 200 .. 203] vector register count
;   [rsp + 204 .. 231] padding
;
; The frame size is 232 rather than a round 224 because the Windows x64 ABI
; wants the stack pointer 16-byte aligned at the call site, and the return
; address already leaves it 8 bytes off after the prologue.

zeus_dynamic_native_invoke PROC FRAME
    sub rsp, 232
    .allocstack 232
    .endprolog

    mov r11, rcx                     ; the callee address
    mov rax, rdx
    mov r10, qword ptr [rax]         ; values base
    mov [rsp + 160], r10
    mov rax, qword ptr [rdx + 8]
    mov [rsp + 168], rax
    mov rax, qword ptr [rdx + 16]
    mov [rsp + 176], rax
    mov rax, qword ptr [rdx + 24]
    mov [rsp + 184], rax
    mov eax, dword ptr [rdx + 32]
    mov [rsp + 192], eax
    mov eax, dword ptr [rdx + 36]
    mov [rsp + 196], eax
    mov eax, dword ptr [rdx + 40]
    mov [rsp + 200], eax

    ; Spilled arguments sit above the shadow space, the first one lowest.
    mov rax, qword ptr [rsp + 184]
    mov ecx, dword ptr [rsp + 192]
    xor edx, edx
stack_argument_loop:
    cmp edx, ecx
    jge stack_arguments_done
    mov r8, qword ptr [rax + rdx * 8]
    mov qword ptr [rsp + 32 + rdx * 8], r8
    inc edx
    jmp stack_argument_loop
stack_arguments_done:

    ; General purpose bank: RCX, RDX, R8, R9.
    mov rax, qword ptr [rsp + 168]
    mov eax, dword ptr [rax + 0]
    cmp eax, 0
    jl general_0_unused
    movsxd rax, eax
    mov rcx, qword ptr [r10 + rax * 8]
    jmp general_0_done
general_0_unused:
    xor ecx, ecx
general_0_done:

    mov rax, qword ptr [rsp + 168]
    mov eax, dword ptr [rax + 4]
    cmp eax, 0
    jl general_1_unused
    movsxd rax, eax
    mov rdx, qword ptr [r10 + rax * 8]
    jmp general_1_done
general_1_unused:
    xor edx, edx
general_1_done:

    mov rax, qword ptr [rsp + 168]
    mov eax, dword ptr [rax + 8]
    cmp eax, 0
    jl general_2_unused
    movsxd rax, eax
    mov r8, qword ptr [r10 + rax * 8]
    jmp general_2_done
general_2_unused:
    xor r8d, r8d
general_2_done:

    mov rax, qword ptr [rsp + 168]
    mov eax, dword ptr [rax + 12]
    cmp eax, 0
    jl general_3_unused
    movsxd rax, eax
    mov r9, qword ptr [r10 + rax * 8]
    jmp general_3_done
general_3_unused:
    xor r9d, r9d
general_3_done:

    ; Vector bank: XMM0, XMM1, XMM2, XMM3.
    mov rax, qword ptr [rsp + 176]
    mov eax, dword ptr [rax + 0]
    cmp eax, 0
    jl vector_0_unused
    movsxd rax, eax
    movq xmm0, qword ptr [r10 + rax * 8]
    jmp vector_0_done
vector_0_unused:
    xorps xmm0, xmm0
vector_0_done:

    mov rax, qword ptr [rsp + 176]
    mov eax, dword ptr [rax + 4]
    cmp eax, 0
    jl vector_1_unused
    movsxd rax, eax
    movq xmm1, qword ptr [r10 + rax * 8]
    jmp vector_1_done
vector_1_unused:
    xorps xmm1, xmm1
vector_1_done:

    mov rax, qword ptr [rsp + 176]
    mov eax, dword ptr [rax + 8]
    cmp eax, 0
    jl vector_2_unused
    movsxd rax, eax
    movq xmm2, qword ptr [r10 + rax * 8]
    jmp vector_2_done
vector_2_unused:
    xorps xmm2, xmm2
vector_2_done:

    mov rax, qword ptr [rsp + 176]
    mov eax, dword ptr [rax + 12]
    cmp eax, 0
    jl vector_3_unused
    movsxd rax, eax
    movq xmm3, qword ptr [r10 + rax * 8]
    jmp vector_3_done
vector_3_unused:
    xorps xmm3, xmm3
vector_3_done:

    ; A variadic callee reads AL to learn how many vector registers it may
    ; read, so the count has to be in place before the call.
    mov al, byte ptr [rsp + 200]

    call r11

    cmp dword ptr [rsp + 196], 1
    je return_double
    cmp dword ptr [rsp + 196], 2
    je return_float
    add rsp, 232
    ret

return_double:
    movq rax, xmm0
    add rsp, 232
    ret

return_float:
    movd eax, xmm0
    add rsp, 232
    ret
zeus_dynamic_native_invoke ENDP

END
