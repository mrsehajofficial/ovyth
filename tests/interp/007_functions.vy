function add(a, b) {
    return a + b
}
function fact(n) {
    if n <= 1 { return 1 }
    return n * fact(n - 1)
}
print(add(2, 3))
print(fact(5))
dbl = function(x) { return x * 2 }
print(dbl(21))
print(add(10, 20), "named:", add(b: 5, a: 5))
