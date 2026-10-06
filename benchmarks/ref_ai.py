#!/usr/bin/env python3
# Vayu :: benchmarks/ref_ai.py
#
# Python reference for the AI pipeline benchmark.
# Matches bench_ai.vy and ref_ai.c case-for-case.
#
# Run: python3 benchmarks/ref_ai.py

import json
import time

def now_ms():
    return time.perf_counter() * 1000.0

SAMPLE = '{"id":"chatcmpl-abc123","object":"chat.completion","created":1700000000,"model":"gpt-4","choices":[{"index":0,"message":{"role":"assistant","content":"Arena allocators free all request-scoped memory in O(1) by releasing an entire block, eliminating per-object malloc/free overhead. This is critical for agent workloads where each request creates hundreds of temporary strings, JSON objects, and embedding inputs."},"finish_reason":"stop"}],"usage":{"prompt_tokens":42,"completion_tokens":52,"total_tokens":94}}'

TOOL_JSON = '{"status":"ok","data":{"query":"arena allocator","results":[{"title":"Arena Allocator Design","score":0.92},{"title":"Memory Management","score":0.87}],"total":2}}'

CHUNKS = [
    "Arena allocators free all request-scoped memory in O(1) by releasing an entire block.",
    "SIMD JSON scanning processes 16 bytes per instruction using SSE4.2 on x86 or NEON on ARM.",
    "Connection pooling eliminates 50-200ms of TLS handshake overhead per repeated API call.",
    "Pipeline fusion combines multiple data transformations into a single pass over the data.",
    "Zero-copy string slicing returns (pointer, length) into existing storage, avoiding allocation.",
    "io_uring provides async I/O with shared ring buffers, reducing syscall count significantly.",
    "Backpressure prevents upstream stages from overwhelming slower downstream stages with data.",
    "Adaptive I/O chooses zero-copy vs buffered based on payload size and hardware capabilities.",
]

DOC = "This document discusses arena allocators and their role in high-performance AI agent systems where memory allocation overhead is critical for latency sensitive workloads processing thousands of requests per second with minimal garbage collection pauses"

HAYSTACK = 'The quick brown fox jumps over the lazy dog. Arena allocators free all request-scoped memory in O(1). SIMD scanning finds structural characters at 16 bytes per cycle.'

print("=== Python AI pipeline reference ===\n")

sink = None

# 1. JSON parse
N_JSON = 50000
t = now_ms()
last = ""
for _ in range(N_JSON):
    doc = json.loads(SAMPLE)
    last = doc["choices"][0]["message"]["content"]
dt = now_ms() - t
print(f"json_parse\t{dt:.0f}ms\t{N_JSON} parses\tresult_len={len(last)}")

# 2. JSON access
doc = json.loads(SAMPLE)
N_ACCESS = 500000
t = now_ms()
content = ""
for _ in range(N_ACCESS):
    content = doc["choices"][0]["message"]["content"]
dt = now_ms() - t
print(f"json_access\t{dt:.0f}ms\t{N_ACCESS} accesses")
sink = content

# 3. Context assembly
N_CTX = 100000
t = now_ms()
ctx = ""
for _ in range(N_CTX):
    parts = [f"[{i+1}] {c}" for i, c in enumerate(CHUNKS)]
    ctx = "\n".join(parts)
dt = now_ms() - t
print(f"context_build\t{dt:.0f}ms\t{N_CTX} assemblies\tctx_len={len(ctx)}")

# 4. Chunk pipeline
N_DOCS = 500
t = now_ms()
total_chunks = 0
for _ in range(N_DOCS):
    words = DOC.split()
    chunks = [" ".join(words[i:i+8]) for i in range(0, len(words), 8)]
    total_chunks += len(chunks)
dt = now_ms() - t
print(f"chunk_pipeline\t{dt:.0f}ms\t{N_DOCS} docs\ttotal_chunks={total_chunks}")

# 5. Hash map (string keys)
N_MAP = 10000
t = now_ms()
state = {}
for i in range(N_MAP):
    state[f"tool_result_{i}"] = i * 2
total = sum(state[f"tool_result_{i}"] for i in range(N_MAP))
dt = now_ms() - t
print(f"hash_map_str\t{dt:.0f}ms\t{N_MAP*2} ops\tsum={total}")

# 6. Multi-JSON parse
N_TOOL = 20000
t = now_ms()
total_score = 0.0
for _ in range(N_TOOL):
    r = json.loads(TOOL_JSON)
    total_score += r["data"]["results"][0]["score"]
dt = now_ms() - t
print(f"multi_parse\t{dt:.0f}ms\t{N_TOOL} tool_jsons")
sink = total_score

# 7. String scan
N_SCAN = 1000000
t = now_ms()
found = 0
for _ in range(N_SCAN):
    pos = HAYSTACK.find('"')
    if pos >= 0:
        found += 1
dt = now_ms() - t
print(f"string_scan\t{dt:.0f}ms\t{N_SCAN} scans")

print(f"\nPython sink: {sink}")
