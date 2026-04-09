; Template LL file for target features
; This file provides target-features that will be merged into the input IR

define void @template() {
entry:
  ret void
}

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"target-features", !"+64bit,+fxsr,+sse,+sse2"}