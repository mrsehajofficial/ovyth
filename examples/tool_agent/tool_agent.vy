// Vayu :: examples/tool_agent/tool_agent.vy
//
// A minimal AI agent with tool calling.
//
// The agent loop:
//   1. Send user message to LLM with tool definitions
//   2. If LLM responds with tool_calls → execute the tool → send result back
//   3. Repeat until LLM gives a text answer
//
// Tools available:
//   - search(query)     → mock keyword search
//   - calculator(expr)  → evaluate simple math
//   - weather(city)     → mock weather data
//
// This demonstrates:
//   - Structured tool dispatch (no reflection/dynamic lookup)
//   - JSON parse of tool call arguments
//   - Multi-turn conversation loop
//   - Per-iteration timing (LLM call overhead vs. tool overhead)
//
// Run against mock server:
//   AI_API_KEY=test AI_API_URL=http://127.0.0.1:8642/v1/chat AI_MODEL=mock-model \
//     vyc run examples/tool_agent/tool_agent.vy

api_key = env_or("AI_API_KEY", "")
api_url = env_or("AI_API_URL", "https://api.example.com/v1/chat")
model   = env_or("AI_MODEL",   "my-model")

// ----------------------------------------------------------------- tool definitions

// Tools described in OpenAI function-calling format
tool_definitions = [
    {
        "type": "function",
        "function": {
            "name": "search",
            "description": "Search for information on a topic",
            "parameters": {
                "type": "object",
                "properties": {
                    "query": {"type": "string", "description": "The search query"}
                },
                "required": ["query"]
            }
        }
    },
    {
        "type": "function",
        "function": {
            "name": "calculator",
            "description": "Evaluate a mathematical expression",
            "parameters": {
                "type": "object",
                "properties": {
                    "expression": {"type": "string", "description": "Math expression to evaluate"}
                },
                "required": ["expression"]
            }
        }
    },
    {
        "type": "function",
        "function": {
            "name": "weather",
            "description": "Get current weather for a city",
            "parameters": {
                "type": "object",
                "properties": {
                    "city": {"type": "string", "description": "City name"}
                },
                "required": ["city"]
            }
        }
    }
]

// ----------------------------------------------------------------- tool implementations

// Direct dispatch: no reflection, no dynamic lookup.
// In a compiled Vayu future: this becomes a jump table keyed by tool ID.

function tool_search(args) {
    query = args["query"]
    // Mock: return canned results based on keywords
    if query.contains("Vayu") {
        return "Vayu is a compiled language for AI automation. It compiles .vy files to native executables. Arena allocators and SIMD JSON parsing make it fast for agent workloads."
    }
    if query.contains("arena") or query.contains("allocator") {
        return "Arena allocators batch-free all objects in O(1), eliminating per-object malloc/free overhead. Critical for request-scoped workloads."
    }
    return "No results found for: " + query
}

function tool_calculator(args) {
    expr = args["expression"]
    // Very basic: handle simple arithmetic by parsing
    // (In production: use a real expression evaluator)
    // For demo, just return a canned response
    return "Result of '" + expr + "' = (calculation engine not implemented in demo)"
}

function tool_weather(args) {
    city = args["city"]
    // Mock weather data
    if city == "London" {
        return '{"city":"London","temp":15,"unit":"C","condition":"cloudy"}'
    }
    if city == "New York" {
        return '{"city":"New York","temp":22,"unit":"C","condition":"sunny"}'
    }
    return '{"city":"' + city + '","temp":20,"unit":"C","condition":"unknown"}'
}

// Dispatcher: routes tool name → implementation
function execute_tool(name, args) {
    if name == "search"     { return tool_search(args) }
    if name == "calculator" { return tool_calculator(args) }
    if name == "weather"    { return tool_weather(args) }
    return "Error: unknown tool '" + name + "'"
}

// ----------------------------------------------------------------- agent loop

if api_key == "" {
    print("AI_API_KEY not set. Running demo without LLM...")
    print("")
    print("Demo: simulating tool dispatch")
    print("  Tool: search({\"query\": \"Vayu arena allocator\"})")
    result = tool_search({"query": "Vayu arena allocator"})
    print("  Result: " + result)
    print("")
    print("  Tool: weather({\"city\": \"London\"})")
    result = tool_weather({"city": "London"})
    print("  Result: " + result)
    exit(0)
}

messages = [
    {
        "role": "system",
        "content": "You are a helpful assistant. Use the available tools to answer questions. Call tools when needed."
    },
    {
        "role": "user",
        "content": "Search for information about Vayu language arena allocator, then tell me the weather in London."
    }
]

print("=== Vayu Tool Agent ===")
print("User: " + messages[1]["content"])
print("")

max_iterations = 5
iteration = 0
total_llm_ms = 0.0
total_tool_ms = 0.0
tool_calls_made = 0

while iteration < max_iterations {
    iteration = iteration + 1

    // --- LLM call ---
    t0 = now()
    try {
        response = http.post(
            api_url,
            headers = {
                "Authorization": "Bearer " + api_key,
                "Content-Type":  "application/json"
            },
            json = {
                "model":       model,
                "messages":    messages,
                "tools":       tool_definitions,
                "tool_choice": "auto"
            }
        )

        llm_ms = (now() - t0) * 1000.0
        total_llm_ms = total_llm_ms + llm_ms

        if response.status != 200 {
            print("LLM error: HTTP " + str(response.status))
            break
        }

        data    = json.parse(response.body)
        choice  = data["choices"][0]
        message = choice["message"]

        // Add assistant message to history
        messages.push(message)

        // Check if tool calls were made
        if message.get("tool_calls") != nil and len(message["tool_calls"]) > 0 {
            tool_calls = message["tool_calls"]
            print("LLM requested " + str(len(tool_calls)) + " tool call(s) [" + str(round(llm_ms)) + "ms]")

            // Execute each tool call
            i = 0
            while i < len(tool_calls) {
                tc   = tool_calls[i]
                name = tc["function"]["name"]
                args_json = tc["function"]["arguments"]

                // Parse arguments JSON
                t1 = now()
                args = json.parse(args_json)
                result = execute_tool(name, args)
                tool_ms = (now() - t1) * 1000.0
                total_tool_ms = total_tool_ms + tool_ms
                tool_calls_made = tool_calls_made + 1

                print("  tool=" + name + " result=" + result + " [" + str(round(tool_ms)) + "ms]")

                // Add tool result to messages
                messages.push({
                    "role":         "tool",
                    "tool_call_id": tc["id"],
                    "content":      result
                })
                i = i + 1
            }
        } else {
            // Final text answer
            content = message["content"]
            print("")
            print("Agent: " + content)
            break
        }

    } catch err {
        print("Error: " + err)
        break
    }
}

// ----------------------------------------------------------------- stats

print("")
print("=== Agent stats ===")
print("  iterations:     " + str(iteration))
print("  tool_calls:     " + str(tool_calls_made))
print("  total_llm_ms:   " + str(round(total_llm_ms)))
print("  total_tool_ms:  " + str(round(total_tool_ms)))
if tool_calls_made > 0 {
    print("  avg_tool_ms:    " + str(round(total_tool_ms / tool_calls_made)))
}
