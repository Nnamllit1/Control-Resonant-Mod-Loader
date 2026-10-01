; Unknown response methods retain their original x64 argument ABI. The helper
; prepares pass-through state, then only the this pointer changes in the tailcall.
option casemap:none
EXTERN crml_native_ui_forward:PROC
.code
FORWARD_SLOT MACRO name, slot_number, slot_offset
name PROC FRAME
    sub rsp, 0C8h
    .allocstack 0C8h
    .endprolog
    mov [rsp+20h], rcx
    mov [rsp+28h], rdx
    mov [rsp+30h], r8
    mov [rsp+38h], r9
    movdqu [rsp+40h], xmm0
    movdqu [rsp+50h], xmm1
    movdqu [rsp+60h], xmm2
    movdqu [rsp+70h], xmm3
    movdqu [rsp+80h], xmm4
    movdqu [rsp+90h], xmm5
    mov [rsp+0A0h], r10
    mov [rsp+0A8h], r11
    mov edx, slot_number
    call crml_native_ui_forward
    mov rcx, rax
    mov rdx, [rsp+28h]
    mov r8, [rsp+30h]
    mov r9, [rsp+38h]
    movdqu xmm0, [rsp+40h]
    movdqu xmm1, [rsp+50h]
    movdqu xmm2, [rsp+60h]
    movdqu xmm3, [rsp+70h]
    movdqu xmm4, [rsp+80h]
    movdqu xmm5, [rsp+90h]
    mov r10, [rsp+0A0h]
    mov r11, [rsp+0A8h]
    add rsp, 0C8h
    mov rax, [rcx]
    jmp QWORD PTR [rax+slot_offset]
name ENDP
ENDM
FORWARD_SLOT crml_native_ui_forward0, 0, 0
FORWARD_SLOT crml_native_ui_forward2, 2, 10h
FORWARD_SLOT crml_native_ui_forward3, 3, 18h
FORWARD_SLOT crml_native_ui_forward4, 4, 20h
FORWARD_SLOT crml_native_ui_forward6, 6, 30h
FORWARD_SLOT crml_native_ui_forward7, 7, 38h
END
