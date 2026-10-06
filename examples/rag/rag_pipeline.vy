// Vayu :: examples/rag/rag_pipeline.vy
//
// A minimal but realistic RAG (Retrieval Augmented Generation) pipeline.
//
// Stages:
//   documents → chunk → embed (mock) → store → query → retrieve → generate
//
// This example demonstrates how Vayu handles the AI data pipeline:
//   - Zero unnecessary copies between stages (documents shared as slices)
//   - JSON parsing for API responses
//   - HTTP POST to LLM with assembled context
//   - Timing each stage to measure pipeline latency
//
// Run against the mock server:
//   AI_API_KEY=test AI_API_URL=http://127.0.0.1:8642/v1/chat AI_MODEL=mock-model \
//     vyc run examples/rag/rag_pipeline.vy
//
// Or against a real OpenAI-compatible API with your key set.

api_key = env_or("AI_API_KEY", "")
api_url = env_or("AI_API_URL", "https://api.example.com/v1/chat")
model   = env_or("AI_MODEL",   "my-model")

// ----------------------------------------------------------------- corpus

// Our document corpus (would be loaded from disk in production)
documents = [
    "Vayu is a compiled language for AI automation and agent workloads. It compiles .vy files to native executables with zero VM overhead.",
    "RAG stands for Retrieval Augmented Generation. It retrieves relevant context from a knowledge base before sending a prompt to an LLM.",
    "Vector search finds the most semantically similar chunks to a query by computing cosine similarity between embedding vectors.",
    "Arena allocators free all objects in O(1) by releasing an entire memory block, avoiding per-object malloc/free overhead.",
    "JSON parsing is on the hot path for every LLM API call. SIMD scanning can process 16 bytes per instruction instead of one.",
    "Connection pooling reuses TCP/TLS sessions across requests, eliminating 50-200ms of handshake overhead per API call.",
    "Pipeline fusion combines multiple data transformations into a single pass, reducing allocations and improving cache locality.",
    "io_uring provides asynchronous I/O using shared submission/completion rings, reducing syscall overhead for high-throughput workloads.",
]

// ----------------------------------------------------------------- chunking

// In production: split by sentence/paragraph boundaries, respect token limits.
// Here: fixed-size word chunks with overlap.
function chunk_document(text, chunk_size, overlap) {
    words = text.split(" ")
    chunks = []
    i = 0
    while i < len(words) {
        end = i + chunk_size
        if end > len(words) {
            end = len(words)
        }
        chunk_words = []
        j = i
        while j < end {
            chunk_words.push(words[j])
            j = j + 1
        }
        chunks.push(join(" ", chunk_words))
        i = i + chunk_size - overlap
        if i < 0 {
            i = 0
        }
    }
    return chunks
}

t0 = now()
all_chunks = []
doc_id = 0
while doc_id < len(documents) {
    doc_chunks = chunk_document(documents[doc_id], 8, 2)
    chunk_idx = 0
    while chunk_idx < len(doc_chunks) {
        all_chunks.push({
            "text":   doc_chunks[chunk_idx],
            "doc_id": doc_id,
            "chunk":  chunk_idx
        })
        chunk_idx = chunk_idx + 1
    }
    doc_id = doc_id + 1
}
t_chunk = (now() - t0) * 1000.0
print("stage=chunking chunks=" + str(len(all_chunks)) + " ms=" + str(round(t_chunk)))

// ----------------------------------------------------------------- embedding (mock)

// In production: POST to an embeddings endpoint (e.g. text-embedding-3-small).
// Mock: use a simple hash-based pseudo-embedding for demo purposes.
function mock_embed(text) {
    // Produce a 4-dim pseudo-vector from character frequencies
    v0 = 0.0
    v1 = 0.0
    v2 = 0.0
    v3 = 0.0
    i = 0
    while i < len(text) {
        c = text[i]
        if c == "a" or c == "e" or c == "i" or c == "o" or c == "u" {
            v0 = v0 + 1.0
        }
        if c >= "a" and c <= "m" {
            v1 = v1 + 1.0
        }
        if c >= "n" and c <= "z" {
            v2 = v2 + 1.0
        }
        v3 = v3 + 1.0
        i = i + 1
    }
    // Normalize
    mag = (v0*v0 + v1*v1 + v2*v2 + 1.0) ^ 0.5
    return [v0 / mag, v1 / mag, v2 / mag, v3 / mag]
}

t0 = now()
chunk_idx = 0
while chunk_idx < len(all_chunks) {
    emb = mock_embed(all_chunks[chunk_idx]["text"])
    all_chunks[chunk_idx]["embedding"] = emb
    chunk_idx = chunk_idx + 1
}
t_embed = (now() - t0) * 1000.0
print("stage=embedding chunks=" + str(len(all_chunks)) + " ms=" + str(round(t_embed)))

// ----------------------------------------------------------------- retrieval

// Cosine similarity between two 4-dim vectors
function cosine_sim(a, b) {
    dot = a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3]
    mag_a = (a[0]*a[0] + a[1]*a[1] + a[2]*a[2] + a[3]*a[3]) ^ 0.5
    mag_b = (b[0]*b[0] + b[1]*b[1] + b[2]*b[2] + b[3]*b[3]) ^ 0.5
    if mag_a == 0.0 or mag_b == 0.0 { return 0.0 }
    return float(dot) / (mag_a * mag_b)
}

// Top-k retrieval by cosine similarity
function retrieve(query, k) {
    q_emb = mock_embed(query)
    scores = []
    i = 0
    while i < len(all_chunks) {
        score = cosine_sim(q_emb, all_chunks[i]["embedding"])
        scores.push({"score": score, "idx": i})
        i = i + 1
    }
    // Simple selection sort for top-k (in production: use SIMD heap)
    j = 0
    while j < k and j < len(scores) {
        max_j = j
        m = j + 1
        while m < len(scores) {
            if scores[m]["score"] > scores[max_j]["score"] {
                max_j = m
            }
            m = m + 1
        }
        // Swap
        tmp = scores[j]
        scores[j] = scores[max_j]
        scores[max_j] = tmp
        j = j + 1
    }
    results = []
    i = 0
    while i < k and i < len(scores) {
        idx = scores[i]["idx"]
        results.push({
            "text":  all_chunks[idx]["text"],
            "score": scores[i]["score"],
            "doc":   all_chunks[idx]["doc_id"]
        })
        i = i + 1
    }
    return results
}

query = "How does Vayu handle memory allocation for AI workloads?"

t0 = now()
top_chunks = retrieve(query, 3)
t_retrieve = (now() - t0) * 1000.0
print("stage=retrieval top_k=3 ms=" + str(round(t_retrieve)))

// ----------------------------------------------------------------- context assembly

t0 = now()
context_parts = []
i = 0
while i < len(top_chunks) {
    context_parts.push("[" + str(i+1) + "] " + top_chunks[i]["text"] +
                       " (score=" + str(round(top_chunks[i]["score"] * 100.0) / 100.0) + ")")
    i = i + 1
}
context = join("\n", context_parts)
t_assemble = (now() - t0) * 1000.0
print("stage=context_assembly ms=" + str(round(t_assemble)))

// ----------------------------------------------------------------- LLM call

if api_key == "" {
    print("(skipping LLM call -- AI_API_KEY not set)")
    print("context assembled: " + str(len(context)) + " chars")
    print("retrieved chunks:")
    i = 0
    while i < len(top_chunks) {
        print("  [" + str(i+1) + "] score=" + str(top_chunks[i]["score"]) + " text=" + top_chunks[i]["text"])
        i = i + 1
    }
} else {
    t0 = now()
    messages = [
        {"role": "system", "content": "You are a helpful assistant. Use the provided context to answer questions."},
        {"role": "user",   "content": "Context:\n" + context + "\n\nQuestion: " + query}
    ]

    try {
        response = http.post(
            api_url,
            headers = {
                "Authorization": "Bearer " + api_key,
                "Content-Type":  "application/json"
            },
            json = {
                "model":    model,
                "messages": messages
            }
        )

        t_llm = (now() - t0) * 1000.0
        print("stage=llm_call ms=" + str(round(t_llm)))

        if response.status != 200 {
            print("LLM error: HTTP " + str(response.status) + " " + response.body)
        } else {
            data   = json.parse(response.body)
            answer = data["choices"][0]["message"]["content"]
            print("answer: " + answer)
        }
    } catch err {
        print("error: " + err)
    }
}

// ----------------------------------------------------------------- summary

print("=== RAG pipeline complete ===")
print("  chunks:       " + str(len(all_chunks)))
print("  chunk_ms:     " + str(round(t_chunk)))
print("  embed_ms:     " + str(round(t_embed)))
print("  retrieve_ms:  " + str(round(t_retrieve)))
print("  assemble_ms:  " + str(round(t_assemble)))
