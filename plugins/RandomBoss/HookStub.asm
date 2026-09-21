; HookStub.asm — the x64 detour stubs for the RandomBoss code hooks.
;
; Both hooks below replace an engine instruction that works on a register the
; x64 calling convention cannot hand to a C++ function (R14 for the map
; placement key read, RAX for the purple flag writer), so the replacement is
; written in asm: it calls the C++ body for the decision, replays the displaced
; instruction(s) and resumes in the engine.
;
; Build: CMake assembles this file with an explicit ml64 custom command rather
; than the VS MASM rule (see CMakeLists.txt for why).

.code

; First detour: the per-instance placement-record key read (RVA 0x679895,
; unique in-module). The game does  mov r14d,[rax+04]  to load the enemy key
; out of one placement record (RAX = record: +0x00 instanceId, +0x04 key,
; +0x08 flags). R14 is not a C++ argument register, so the replacement must
; happen here: call the C++ body with the record, write its answer into R14D,
; replay both displaced instructions, then resume at site+7.
;
; Register contract: RCX/RDX/R8-R11/RBP and the flags are saved and restored;
; RBX/RDI/RSI/R12-R15 are never touched; RAX is deliberately left clobbered —
; both successor paths overwrite EAX before reading it (mov eax,[rdi+0x20] on
; the je target, mov eax,[rbx] on the fall-through), so it is dead here.
EXTERN g_mapResume : QWORD
EXTERN MapBossHookBody : PROC

MapBossHookStub PROC
    pushfq
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    push rbp
    mov  rbp, rsp              ; keep the incoming RSP to restore it exactly
    and  rsp, -16              ; ABI: RSP must be 16-byte aligned at the call
    sub  rsp, 20h              ; shadow space for the C++ body
    mov  rcx, rax              ; arg1 = placement record
    mov  rdx, rdi              ; arg2 = the entity whose [+0x20] selected it.
                               ; RDI is callee-saved, so the C++ body's own use
                               ; of it cannot disturb the game's copy.
    call MapBossHookBody       ; returns the key to use in EAX
    mov  rsp, rbp
    pop  rbp
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rdx
    pop  rcx
    popfq
    mov  r14d, eax             ; the key every downstream consumer will see
    test rbx, rbx              ; displaced instruction #2: sets the ZF the
                               ; following je depends on
    jmp  qword ptr [g_mapResume]
MapBossHookStub ENDP

; Second detour: the per-frame 一難 (purple) flag writer, pattern + 14 bytes in.
; The engine is about to execute `mov [rax+0E8h],cl` with RAX = the entity it is
; updating. Both RAX and RCX must survive the C++ call because the displaced
; instruction uses them, so unlike the map stub this one saves RAX as well and
; replays the store itself before resuming at site+6.
;
; RAX is the only register the C++ body needs, and it is passed in RCX (the x64
; argument register) to keep the body's signature ordinary C.
EXTERN g_mapPurpleResume : QWORD
EXTERN MapPurpleHookBody : PROC

MapPurpleHookStub PROC
    pushfq
    push rax
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    push rbp
    mov  rbp, rsp
    and  rsp, -16
    sub  rsp, 20h
    mov  rcx, rax                  ; arg1 = the entity being written
    call MapPurpleHookBody
    mov  rsp, rbp
    pop  rbp
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rdx
    pop  rcx
    pop  rax
    popfq
    mov  byte ptr [rax+0E8h], cl   ; displaced instruction
    jmp  qword ptr [g_mapPurpleResume]
MapPurpleHookStub ENDP

END
