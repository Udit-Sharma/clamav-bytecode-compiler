; RUN: opt < %s -constmerge -S | FileCheck %s

; Don't merge constants with specified sections.

@T1G1 = internal constant i32 1, section "foo"
@T1G2 = internal constant i32 1, section "bar"
@T1G3 = internal constant i32 1, section "bar"

; CHECK: @T1G1
; CHECK: @T1G2
; CHECK: @T1G3

define void @test1(i32** %P1, i32** %P2, i32** %P3) {
        store i32* @T1G1, i32** %P1
        store i32* @T1G2, i32** %P2
        store i32* @T1G3, i32** %P3
        ret void
}

@T2a = internal constant i32 224
@T2b = internal addrspace(30) constant i32 224

; CHECK: @T2a
; CHECK: @T2b

define void @test2(i32** %P1, i32 addrspace(30)** %P2) {
        store i32* @T2a, i32** %P1
        store i32 addrspace(30)*  @T2b, i32 addrspace(30)** %P2
        ret void
}
