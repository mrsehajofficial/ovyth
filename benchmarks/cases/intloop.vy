// Integer arithmetic loop, 20M iterations.
//
// The body is deliberately not `total = total + i * 3 - 1`: clang -O2 and
// above recognise that affine recurrence and replace the entire loop with a
// closed-form expression (try it with plain C), so the binary stops doing
// arithmetic and the case measures process startup instead. The xor/shift
// form below is the same shape -- one int, one counter, one comparison --
// but has no closed form, so every implementation has to actually run the
// loop. Result check: all three print 202067735460992.
total = 0
i = 0
while i < 20000000 {
    total = total + (i ^ (i >> 3)) - 1
    i = i + 1
}
print(total)
