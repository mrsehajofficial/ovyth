#!/usr/bin/env python3
"""Reference implementation of the Vayu benchmark suite, in Python.

Used by benchmarks/compare.sh for a like-for-like comparison (spec section 35):
same algorithm, same input, same output as benchmarks/cases/*.vy. Python here is
the dynamic baseline -- it is not expected to lose on expressiveness, only on
raw loop throughput.
"""
import json
import sys
import time


def bench_intloop(n=20_000_000):
    total = 0
    i = 0
    while i < n:
        total = total + i * 3 - 1
        i = i + 1
    return total


def bench_fib(k=25):
    def fib(n):
        if n < 2:
            return n
        return fib(n - 1) + fib(n - 2)
    return fib(k)


def bench_strconcat(n=40_000):
    # Python strings are immutable and CPython optimises `s += x` in place when
    # refcount==1, so this is the *fair* comparison, not a strawman.
    acc = ""
    i = 0
    while i < n:
        acc = acc + "hello world "
        i = i + 1
    return len(acc)


def bench_listappend(n=500_000):
    xs = []
    i = 0
    while i < n:
        xs.append(i)
        i = i + 1
    return len(xs), sum(xs)


def bench_mapops(n=100_000):
    m = {}
    i = 0
    while i < n:
        m["key" + str(i)] = i
        i = i + 1
    j = 0
    s = 0
    while j < n:
        s = s + m["key" + str(j)]
        j = j + 1
    return len(m), s


def bench_json():
    doc = json.loads(
        '{"name":"vayu","tags":["a","b","c"],"n":42,"ok":true,'
        '"nested":{"x":1,"y":[1,2,3]}}'
    )
    return doc["name"], doc["n"], doc["ok"]


CASES = [
    ("intloop", bench_intloop),
    ("fib", bench_fib),
    ("strconcat", bench_strconcat),
    ("listappend", bench_listappend),
    ("mapops", bench_mapops),
    ("json", bench_json),
]

if __name__ == "__main__":
    only = sys.argv[1] if len(sys.argv) > 1 else None
    for name, fn in CASES:
        if only and only != name:
            continue
        t0 = time.perf_counter()
        r = fn()
        dt = (time.perf_counter() - t0) * 1000.0
        print(f"{name} {dt:.0f} {r}")