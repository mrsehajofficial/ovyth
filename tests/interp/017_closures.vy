// Closures: capture-by-reference over the enclosing scope. Both backends
// must agree (see tests/interp.sh). Covers read-only capture, shared
// read+write capture, write-only capture (a setter), closures returned from
// functions, and closures stored in a list and called through an index.

// read-only capture: an adder factory
function make_adder(n) {
    return function(x) { return x + n }
}
add5 = make_adder(5)
add10 = make_adder(10)
print(add5(37))
print(add10(37))

// shared capture: two closures over the same counter, writes visible both ways
function make_counter(start) {
    count = start
    inc = function() {
        count = count + 1
        return count
    }
    get = function() { return count }
    return [inc, get]
}
pair = make_counter(10)
inc = pair[0]
get = pair[1]
print(inc())
print(inc())
print(get())

// write-only capture: a setter mutates a var the sibling getter reads
function make_box(initial) {
    value = initial
    setter = function(x) { value = x }
    getter = function() { return value }
    return [setter, getter]
}
box = make_box(0)
box[0](99)
print(box[1]())

// each closure creation gets its own box: independent counters
function counter_from(n) {
    return function() {
        n = n + 1
        return n
    }
}
a = counter_from(0)
b = counter_from(100)
print(a())
print(a())
print(b())
