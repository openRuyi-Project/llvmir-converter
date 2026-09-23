; Template defaults used by target-feature regression coverage.
target triple = "x86_64-pc-linux-gnu"

define void @template() #0 {
entry:
  ret void
}

attributes #0 = { "target-features"="-rtm,-avx,+sse2" }