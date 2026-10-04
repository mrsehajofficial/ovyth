// String accumulation. The native backend lowers `acc = acc + x` inside a
// loop to a growable string builder, which turns quadratic concatenation into
// linear. These cases pin the semantics that rewrite must not change.
acc = ""
i = 0
while i < 4 {
    acc = acc + "ab"
    i = i + 1
}
print(acc)

acc2 = ""
i = 0
while i < 3 {
    acc2 += "x"
    i = i + 1
}
print(acc2)

// Appending a non-string renders it, as `+` would.
acc3 = ""
i = 0
while i < 3 {
    acc3 = acc3 + i
    i = i + 1
}
print(acc3)

// Nested loops append to the same accumulator.
acc4 = ""
i = 0
while i < 2 {
    j = 0
    while j < 3 {
        acc4 = acc4 + "n"
        j = j + 1
    }
    i = i + 1
}
print(acc4)

// Two independent accumulators must not interfere.
a = ""
b = ""
i = 0
while i < 2 {
    a = a + "a"
    b = b + "b"
    i = i + 1
}
print(a, b)

// break / continue out of a building loop.
acc5 = ""
i = 0
while i < 5 {
    acc5 = acc5 + "c"
    i = i + 1
    if i == 3 { break }
}
print(acc5)

acc6 = ""
i = 0
while i < 6 {
    i = i + 1
    if i % 2 == 0 { continue }
    acc6 = acc6 + "o"
}
print(acc6)

// The accumulator is a normal string afterwards.
print(acc, acc + "!")
