; accessor_stubs -- exports of the SteamVR proxy whose signature we do not know (interface accessors of Valve's
; openvr_api.dll: VRHeadsetView, VRPaths, LiquidVR, ...). Each stub jumps to the real DLL's export of the same name
; with every argument register and the stack untouched. The pointer comes from g_eyecapAccessors[idx] (proxy_exports.cpp);
; while it is 0 the stub goes through EyeCapAccessorSlow, which saves the argument registers (rcx, rdx, r8, r9,
; xmm0-xmm3), calls EyeCapResolveAccessor(idx) (loads the real DLL on first use), restores them and jumps -- or returns 0
; when the real DLL lacks that export.
; The .pushreg on rcx / rdx / r8 / r9 (volatile registers) is intentional: they are pushed only to be restored before the
; tail jump (they hold the caller's arguments), and the unwind codes must describe every push of the prologue so an
; exception or a stack walk inside EyeCapResolveAccessor unwinds this frame correctly.

EXTERN EyeCapResolveAccessor:PROC
EXTERN g_eyecapAccessors:QWORD

_TEXT SEGMENT

EyeCapAccessorSlow PROC FRAME
    push rcx
    .pushreg rcx
    push rdx
    .pushreg rdx
    push r8
    .pushreg r8
    push r9
    .pushreg r9
    sub rsp, 104
    .allocstack 104
    .endprolog
    movdqu xmmword ptr [rsp+32], xmm0
    movdqu xmmword ptr [rsp+48], xmm1
    movdqu xmmword ptr [rsp+64], xmm2
    movdqu xmmword ptr [rsp+80], xmm3
    mov ecx, r10d
    call EyeCapResolveAccessor
    movdqu xmm0, xmmword ptr [rsp+32]
    movdqu xmm1, xmmword ptr [rsp+48]
    movdqu xmm2, xmmword ptr [rsp+64]
    movdqu xmm3, xmmword ptr [rsp+80]
    add rsp, 104
    pop r9
    pop r8
    pop rdx
    pop rcx
    test rax, rax
    jz missing
    jmp rax
missing:
    ret
EyeCapAccessorSlow ENDP

ACCESSOR MACRO name, idx
    LOCAL slow
name PROC
    mov rax, qword ptr [g_eyecapAccessors + idx * 8]
    test rax, rax
    jz slow
    jmp rax
slow:
    mov r10d, idx
    jmp EyeCapAccessorSlow
name ENDP
ENDM

; same order as kAccessorNames in proxy_exports.cpp
ACCESSOR LiquidVR, 0
ACCESSOR VRCompositorSystemInternal, 1
ACCESSOR VRControlPanel, 2
ACCESSOR VRDriverDirectInternal, 3
ACCESSOR VRHeadsetView, 4
ACCESSOR VRPaths, 5
ACCESSOR VRRenderModelsInternal, 6
ACCESSOR VRSceneGraph, 7
ACCESSOR VRTrackedCameraInternal, 8
ACCESSOR VRVirtualDisplay, 9

_TEXT ENDS
END
