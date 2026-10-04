// Vayu :: examples/chatbot/chatbot.vy
//
// The first real benchmark application (PRD §12). A minimal LLM chatbot:
// read a line from the console, POST it to a chat-completions endpoint,
// print the model's reply. Runs under the interpreter (`vyc run`) and as a
// standalone native executable (`vyc chatbot.vy`).
//
// Configuration comes from the environment, never from the source:
//
//   AI_API_KEY   - the bearer token for the endpoint
//   AI_API_URL   - chat endpoint (default: https://api.example.com/v1/chat)
//   AI_MODEL     - model name   (default: my-model)
//
// Point it at any OpenAI-compatible API; a local mock is provided for
// testing without a network:  python3 examples/chatbot/mock_server.py

api_key = env_or("AI_API_KEY", "")

api_url = env_or("AI_API_URL", "https://api.example.com/v1/chat")

model = env_or("AI_MODEL", "my-model")

if api_key == "" {
    print("AI_API_KEY is not set; asking the server would fail authentication.")
    exit(1)
}

history = [
    {
        "role": "system",
        "content": "You are a concise assistant."
    }
]

print("Vayu chatbot -- type 'exit' to quit")

while true {
    user_message = input("> ")
    if user_message == "" or user_message == "exit" {
        break
    }

    messages = history
    messages.push({
        "role": "user",
        "content": user_message
    })

    try {
        response = http.post(
            api_url,
            headers = {
                "Authorization": "Bearer " + api_key,
                "Content-Type": "application/json"
            },
            json = {
                "model": model,
                "messages": messages
            }
        )

        if response.status != 200 {
            print("HTTP " + str(response.status) + ": " + response.body)
            break
        }

        data = json.parse(response.body)
        answer = data["choices"][0]["message"]["content"]
        print(answer)

        messages.push({
            "role": "assistant",
            "content": answer
        })
    } catch error {
        print("error: " + error)
        break
    }
}

print("bye")
