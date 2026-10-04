function fib(k) {
    if k < 2 { return k }
    return fib(k - 1) + fib(k - 2)
}
print(fib(25))
