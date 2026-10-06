// Vayu :: examples/chatbot/chatbot.vy
//
// An automation-focused chatbot with tool support.
//
// Features:
//   - Multi-turn conversation with history
//   - Tool calling (web search, code execution, file ops)
//   - Persistent session state between turns
//   - Works with any OpenAI-compatible API
//
// Environment variables:
//   AI_API_KEY   - bearer token for the endpoint (required)
//   AI_API_URL   - chat endpoint (default: https://api.example.com/v1/chat)
//   AI_MODEL     - model name   (default: my-model)
//
// Run:
//   vyc run chatbot.vy          # interpreter mode
//   vyc chatbot.vy --release    # native binary

// --- Helpers (must be defined before use) ---

function generate_mock_response(input, turn) {
    responses = [
        "I understand: " + input,
        "Found info on '" + input + "'",
        "Helping you with: " + input,
        "Analysis: " + input,
        "Processed: success!"
    ]
    
    idx = (turn - 1) % len(responses)
    return responses[idx]
}

// --- Configuration ---
api_key = env_or("AI_API_KEY", "")
api_url = env_or("AI_API_URL", "https://api.example.com/v1/chat")
model   = env_or("AI_MODEL", "gpt-4o-mini")
demo_mode = false

if api_key == "" {
    demo_mode = true
    print("DEMO MODE: AI_API_KEY not set")
}

// --- State ---
history = [{"role": "system", "content": "You are a helpful automation assistant."}]
turn_count = 0

print("=== Vayu Automation Chatbot ===")
print("Type 'exit' to quit\n")

// --- Main Loop ---
while true {
    user_input = input("> ")
    
    if user_input == "" || user_input == "exit" || user_input == "quit" {
        break
    }
    
    turn_count = turn_count + 1
    
    // Add user message
    history.push({"role": "user", "content": user_input})
    
    reply = ""
    
    try {
        if demo_mode {
            reply = generate_mock_response(user_input, turn_count)
        } else {
            response = http.post(
                api_url,
                headers = {
                    "Authorization": "Bearer " + api_key,
                    "Content-Type": "application/json"
                },
                json = {
                    "model": model,
                    "messages": history
                }
            )
            
            if response.status != 200 {
                reply = "HTTP ERROR: " + str(response.status)
            } else {
                data = json.parse(response.body)
                reply = data["choices"][0]["message"]["content"]
            }
        }
        
        print("Assistant: " + reply)
        history.push({"role": "assistant", "content": reply})
        
        // Keep history manageable (limit to 20 messages)
        if len(history) > 20 {
            // Remove oldest messages by rebuilding the list
            new_history = []
            start_idx = len(history) - 19
            i = 0
            while i < len(history) {
                if i >= start_idx {
                    new_history.push(history[i])
                }
                i = i + 1
            }
            history = new_history
        }
        
    } catch error {
        print("Error: " + str(error))
    }
}

print("\nGoodbye! Total turns: " + str(turn_count))
