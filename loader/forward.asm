option casemap:none
EXTERN crml_resolve:PROC
.code
FORWARD MACRO index
forward_&index PROC FRAME
    sub rsp, 0A8h
    .allocstack 0A8h
    .endprolog
    mov [rsp+20h], rcx
    mov [rsp+28h], rdx
    mov [rsp+30h], r8
    mov [rsp+38h], r9
    movdqu [rsp+40h], xmm0
    movdqu [rsp+50h], xmm1
    movdqu [rsp+60h], xmm2
    movdqu [rsp+70h], xmm3
    mov ecx, index
    call crml_resolve
    mov rcx, [rsp+20h]
    mov rdx, [rsp+28h]
    mov r8, [rsp+30h]
    mov r9, [rsp+38h]
    movdqu xmm0, [rsp+40h]
    movdqu xmm1, [rsp+50h]
    movdqu xmm2, [rsp+60h]
    movdqu xmm3, [rsp+70h]
    add rsp, 0A8h
    jmp rax
forward_&index ENDP
ENDM
FORWARD 0
FORWARD 1
FORWARD 2
FORWARD 3
FORWARD 4
FORWARD 5
FORWARD 6
FORWARD 7
FORWARD 8
FORWARD 9
FORWARD 10
FORWARD 11
FORWARD 12
FORWARD 13
END
