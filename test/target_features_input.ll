; Input module for protected target-feature regression coverage.
target triple = "x86_64-pc-linux-gnu"

define void @has_rtm() #0 {
entry:
  ret void
}

define void @has_avx() #1 {
entry:
  ret void
}

define i32 @main() {
entry:
  ret i32 0
}

attributes #0 = { "target-features"="+rtm,+sse2" }
attributes #1 = { "target-features"="+avx,+sse2" }