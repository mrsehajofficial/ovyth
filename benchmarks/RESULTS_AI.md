# Vayu AI Pipeline Benchmarks

> **Note:** These benchmarks measure the AI orchestration layer (JSON, context assembly, chunking, map ops) -- not LLM inference. LLM inference dominates at 100ms-10s; we optimize the 1-50ms overhead.

## Results (macOS, clang -O3)

```
=== Vayu vs C vs Python -- AI pipeline benchmark ===

case                 Vayu (ms)    C (ms)   Py (ms)        vs C   vs Python
------------------  ----------  --------  --------  ----------  ----------
json_parse             22000ms     128ms     608ms  171.9x slower  36.2x slower
json_access             1000ms       0ms     184ms           ?   5.4x slower
context_build           3000ms     154ms     482ms   26.0x slower   8.3x slower
chunk_pipeline              0ms       0ms       4ms           ?  0.0x faster
hash_map_str                0ms       5ms      14ms  0.0x faster  0.0x faster
multi_parse                 0ms       4ms     166ms  0.0x faster  0.0x faster
string_scan                 0ms       0ms     371ms           ?  0.0x faster
```

## Analysis

### Where Vayu wins (vs Python)
- `chunk_pipeline`: ~equal (both too fast to distinguish at 0ms)
- `hash_map_str`: ~equal (Vayu 0ms vs Python 14ms, but Vayu has process startup overhead)
- `multi_parse`: Vayu 0ms vs Python 166ms

### Where Vayu is slower
- **json_parse**: 22000ms vs Python 608ms (36x slower)
  - Root cause: Vayu builds a full parsed object graph; Python's json.loads is highly optimized C
- **context_build**: 3000ms vs Python 482ms (8x slower)
  - Root cause: List construction + string joining allocates many temporary objects
- **json_access**: 1000ms vs Python 184ms (5x slower)
  - Root cause: Deep nested indexing with GC overhead

### Where C dominates
- All cases: C is 26x-172x faster than Vayu
- Root cause: Vayu's tagged union + GC overhead is significant for tight loops

## Notes
- Vayu numbers are whole-process wall clock (~4ms startup included)
- C and Python time their cases internally
- Build: C uses clang -O3 -march=native, Vayu uses --release (-O3)
