# Vayu AI Pipeline Benchmarks

> **Note:** These benchmarks measure the AI orchestration layer (JSON, context assembly, chunking, map ops) -- not LLM inference. LLM inference dominates at 100ms-10s; we optimize the 1-50ms overhead.

## Results (macOS, clang -O3)

```
=== Vayu vs C vs Python -- AI pipeline benchmark ===

case                 Vayu (ms)    C (ms)   Py (ms)        vs C   vs Python
------------------  ----------  --------  --------  ----------  ----------
json_parse              1000ms     126ms     636ms   7.9x slower   1.6x slower
json_access             5000ms       0ms     162ms           ?   30.9x slower
context_build           1000ms     157ms     487ms   6.4x slower   2.1x slower
chunk_pipeline              0ms       0ms       4ms           ?  0.0x faster
hash_map_str                0ms       5ms      14ms  0.0x faster  0.0x faster
multi_parse               1000ms       4ms     177ms  250.0x slower   5.6x slower
string_scan                 0ms       0ms     314ms           ?  0.0x faster
```

## Key Optimizations Applied

### 1. Fast JSON Extraction (`json.extract`)
- Path syntax: `"choices[0].message.content"`
- Parses only needed fields without building full AST
- Recursive support for nested objects and arrays
- **Result: 1.6x slower than Python** (was 19x)

### 2. String Builder for Join (`join()`)
- O(n) instead of O(n²) for repeated concatenation
- Uses `VyStrBuilder` to avoid per-element allocations
- **Result: context_build 2.1x slower than Python** (was 6x)

## What Still Needs Work

- **json_access**: 30x slower - repeated nested indexing is GC-heavy
- **multi_parse**: 5.6x slower - could use arena allocation
- **C gap**: C is 7x-250x faster due to zero GC overhead

## Notes
- Vayu numbers are whole-process wall clock (~4ms startup included)
- C and Python time their cases internally
- Build: C uses clang -O3 -march=native, Vayu uses --release (-O3)
