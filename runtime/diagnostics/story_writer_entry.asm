; Capture the reviewed binding's nonvolatile RBX before compiled C++ can reuse
; it. Only the bridge's exact caller guard may interpret this value as a VM.
option casemap:none
EXTERN crml_story_writer_bridge:PROC
.code
crml_story_writer_entry PROC FRAME
    sub rsp, 38h
    .allocstack 38h
    .endprolog
    mov r9, rbx
    mov rax, [rsp+38h]
    mov [rsp+20h], rax
    call crml_story_writer_bridge
    add rsp, 38h
    ret
crml_story_writer_entry ENDP

END
