// Vayu :: benchmarks/bench_ai.vy
//
// AI pipeline benchmark suite.
//
// These benchmarks measure exactly the operations that dominate AI agent
// workloads -- NOT integer arithmetic. The target is to beat ordinary C
// implementations by eliminating unnecessary allocations, copies, and
// intermediate objects between pipeline stages.
//
// Workloads:
//   json_parse     - parse a realistic LLM completion response
//   json_access    - navigate the parsed object graph
//   context_build  - assemble prompt context from retrieved chunks
//   chunk_pipeline - document -> split -> chunk -> join (data pipeline)
//   hash_map_str   - string-keyed map operations (agent state)
//   string_scan    - scan for JSON structural chars (simulates tokenizer)
//   multi_parse    - parse 1000 small JSON objects (tool results)
//
// Run natively:
//   vyc benchmarks/bench_ai.vy --release -o /tmp/bench_ai && /tmp/bench_ai
//
// Each case prints: name<TAB>ms<TAB>ops (for the compare script)

print("=== Vayu AI pipeline benchmarks ===")
print("")

// ----------------------------------------------------------------- 1. JSON parse

// A realistic OpenAI chat completion response
sample_completion = "{\"id\":\"chatcmpl-abc123\",\"object\":\"chat.completion\",\"created\":1700000000,\"model\":\"gpt-4\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"Arena allocators free all request-scoped memory in O(1) by releasing an entire block, eliminating per-object malloc/free overhead. This is critical for agent workloads where each request creates hundreds of temporary strings, JSON objects, and embedding inputs.\"},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":42,\"completion_tokens\":52,\"total_tokens\":94}}"

N_JSON = 50000
t0 = time.clock()
i = 0
last = ""
while i < N_JSON {
    last = json.extract(sample_completion, "choices[0].message.content")
    i = i + 1
}
t_json_parse = (time.clock() - t0) * 1000.0
print("json_parse\t" + str(round(t_json_parse)) + "ms\t" + str(N_JSON) + " extractions\tresult_len=" + str(len(last)))

// ----------------------------------------------------------------- 2. JSON access (field navigation)

// Parse once, then access the already-parsed value many times (matching C benchmark)
doc = json.parse(sample_completion)
N_ACCESS = 500000
t0 = time.clock()
i = 0
content = ""
while i < N_ACCESS {
    content = doc["choices"][0]["message"]["content"]
    i = i + 1
}
t_json_access = (time.clock() - t0) * 1000.0
print("json_access\t" + str(round(t_json_access)) + "ms\t" + str(N_ACCESS) + " extractions")

// ----------------------------------------------------------------- 3. Context assembly (RAG hot path)

// Simulate assembling a RAG context from 8 retrieved chunks
chunks = [
    "Arena allocators free all request-scoped memory in O(1) by releasing an entire block.",
    "SIMD JSON scanning processes 16 bytes per instruction using SSE4.2 on x86 or NEON on ARM.",
    "Connection pooling eliminates 50-200ms of TLS handshake overhead per repeated API call.",
    "Pipeline fusion combines multiple data transformations into a single pass over the data.",
    "Zero-copy string slicing returns (pointer, length) into existing storage, avoiding allocation.",
    "io_uring provides async I/O with shared ring buffers, reducing syscall count significantly.",
    "Backpressure prevents upstream stages from overwhelming slower downstream stages with data.",
    "Adaptive I/O chooses zero-copy vs buffered based on payload size and hardware capabilities.",
]

N_CTX = 100000
t0 = time.clock()
i = 0
ctx = ""
while i < N_CTX {
    parts = []
    j = 0
    while j < len(chunks) {
        parts.push("[" + str(j+1) + "] " + chunks[j])
        j = j + 1
    }
    ctx = join("\n", parts)
    i = i + 1
}
t_context = (time.clock() - t0) * 1000.0
print("context_build\t" + str(round(t_context)) + "ms\t" + str(N_CTX) + " assemblies\tctx_len=" + str(len(ctx)))

// ----------------------------------------------------------------- 4. Document chunking pipeline

// Simulate the ingestion -> chunking stage for documents
function chunk_text(text, size) {
    words = text.split(" ")
    result = []
    i = 0
    while i < len(words) {
        end = i + size
        if end > len(words) { end = len(words) }
        slice_words = []
        j = i
        while j < end {
            slice_words.push(words[j])
            j = j + 1
        }
        result.push(join(" ", slice_words))
        i = i + size
    }
    return result
}

doc_template = "This document discusses arena allocators and their role in high-performance AI agent systems where memory allocation overhead is critical for latency sensitive workloads processing thousands of requests per second with minimal garbage collection pauses"

N_DOCS = 500
t0 = time.clock()
total_chunks = 0
i = 0
while i < N_DOCS {
    chunks_out = chunk_text(doc_template, 8)
    total_chunks = total_chunks + len(chunks_out)
    i = i + 1
}
t_chunk = (time.clock() - t0) * 1000.0
print("chunk_pipeline\t" + str(round(t_chunk)) + "ms\t" + str(N_DOCS) + " docs\ttotal_chunks=" + str(total_chunks))

// ----------------------------------------------------------------- 5. String-keyed map (agent state)

// Agent state is often a map of string -> value (tool results, variables)
N_MAP = 10000
t0 = time.clock()
i = 0
state = {}
while i < N_MAP {
    state["tool_result_" + str(i)] = i * 2
    i = i + 1
}
// Read all keys back
sum_v = 0
i = 0
while i < N_MAP {
    v = state["tool_result_" + str(i)]
    sum_v = sum_v + v
    i = i + 1
}
t_map = (time.clock() - t0) * 1000.0
print("hash_map_str\t" + str(round(t_map)) + "ms\t" + str(N_MAP) + " inserts+" + str(N_MAP) + " reads\tsum=" + str(sum_v))

// ----------------------------------------------------------------- 6. Multi JSON parse (tool results)

// Each tool call returns a small JSON object; extract score field directly
tool_result = "{\"status\":\"ok\",\"data\":{\"query\":\"arena allocator\",\"results\":[{\"title\":\"Arena Allocator Design\",\"score\":0.92},{\"title\":\"Memory Management\",\"score\":0.87}],\"total\":2}}"

N_TOOL = 20000
t0 = time.clock()
i = 0
total_score = 0.0
while i < N_TOOL {
    total_score = total_score + json.get_float(tool_result, "score")
    i = i + 1
}
t_tool = (time.clock() - t0) * 1000.0
print("multi_parse\t" + str(round(t_tool)) + "ms\t" + str(N_TOOL) + " tool_jsons")

// ----------------------------------------------------------------- 7. String scanning (simulates JSON tokenizer inner loop)

// The bottleneck in any JSON parser: find the next structural character
haystack = "The quick brown fox jumps over the lazy dog. Arena allocators free all request-scoped memory in O(1). SIMD scanning finds structural characters at 16 bytes per cycle."
needle = "\""
N_SCAN = 1000000
t0 = time.clock()
i = 0
found = 0
while i < N_SCAN {
    pos = haystack.find(needle)
    if pos >= 0 { found = found + 1 }
    i = i + 1
}
t_scan = (time.clock() - t0) * 1000.0
print("string_scan\t" + str(round(t_scan)) + "ms\t" + str(N_SCAN) + " scans")

// ----------------------------------------------------------------- summary

print("")
print("=== Summary ===")
print("json_parse:     " + str(round(t_json_parse))  + "ms for " + str(N_JSON)   + " ops")
print("json_access:    " + str(round(t_json_access)) + "ms for " + str(N_ACCESS) + " ops")
print("context_build:  " + str(round(t_context))     + "ms for " + str(N_CTX)    + " ops")
print("chunk_pipeline: " + str(round(t_chunk))       + "ms for " + str(N_DOCS)   + " docs")
print("hash_map_str:   " + str(round(t_map))         + "ms for " + str(N_MAP*2)  + " ops")
print("multi_parse:    " + str(round(t_tool))        + "ms for " + str(N_TOOL)   + " ops")
print("string_scan:    " + str(round(t_scan))        + "ms for " + str(N_SCAN)   + " ops")
print("")
print("done")
