option casemap:none
EXTERN crml_story_writer_entry:PROC
PUBLIC crml_story_writer_fixture_return
.code
crml_story_writer_fixture PROC FRAME
    push rbx
    .pushreg rbx
    sub rsp, 20h
    .allocstack 20h
    .endprolog
    mov rbx, r9
    call crml_story_writer_entry
crml_story_writer_fixture_return LABEL BYTE
    add rsp, 20h
    pop rbx
    ret
crml_story_writer_fixture ENDP
END
