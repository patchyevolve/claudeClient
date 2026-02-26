#include <cstdlib>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

//tool interface

class Tool {
public:
    virtual std::string execute(const json& args) = 0;
    virtual ~Tool() = default;
};

// read-Tool
class ReadFileTool : public Tool {
public:
    std::string execute(const json& args) override {
        if (!args.contains("path") || !args["path"].is_string()) {
            return "ERROR : invalid argumnets.";
        }

        std::string path = args["path"];

        //basic path traversal protection
        if (path.find("..") != std::string::npos) {
            return "ERROR : path traversal detected.";
        }

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            return "ERROR : could not open file.";
        }
        
        file.seekg(0, std::ios::end);
        std::streamsize size = file.tellg();
        if (size <= 0) {
            return "ERROR: could not determine file size.";
        }
        file.seekg(0, std::ios::beg);

        const std::streamsize MAX_SIZE = 1'000'000;

        if (size > MAX_SIZE) {
            return "ERROR: File too Large";
        }

        std::string content(size, '\0');
        file.read(&content[0], size);

        return content;
    }
};


// Tool-Registry

class ToolRegistry {
public:
    void register_tool(const std::string& name, Tool* tool) {
        tools[name] = tool;
    }

    Tool* get(const std::string& name) {
        if (tools.count(name)) return tools[name];
        return nullptr;
    }

private:
    std::unordered_map<std::string, Tool*> tools;
};


// MAIN

int main(int argc, char* argv[]) {
    
    // api calling section

    if (argc < 3 || std::string(argv[1]) != "-p") {
        std::cerr << "Expected first argument to be '-p'" << std::endl;
        return 1;
    }

    std::string prompt = argv[2];

    if (prompt.empty()) {
        std::cerr << "Prompt must not be empty" << std::endl;
        return 1;
    }

    const char* api_key_env = std::getenv("OPENROUTER_API_KEY");
    const char* base_url_env = std::getenv("OPENROUTER_BASE_URL");

    std::string api_key = api_key_env ? api_key_env : "";
    std::string base_url = base_url_env ? base_url_env : "https://openrouter.ai/api/v1";

    if (api_key.empty()) {
        std::cerr << "OPENROUTER_API_KEY is not set" << std::endl;
        return 1;
    }


    // Tool setup

    ToolRegistry registry;
    ReadFileTool readTool;
    registry.register_tool("read_file", &readTool);

    json tools = json::array({
        {
            {"type", "function"},
            {"function", {
                {"name", "read_file"},
                {"description", "Read and return the contents of a file"},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"path",{
                            {"type", "string"},
                            {"description", "The path to the file to read"}
                        }}
                    }},
                    {"required", json::array({"path"})}
                }},
            }}
        }
    });

    // conversion state
    json messages = json::array({
        {{"role", "user"}, {"content", prompt}}
    });

    // TOOL LOOP

    while (true) {

        json request_body = {
            {"model", "anthropic/claude-haiku-4.5"},
            {"messages", messages},
            {"tools", tools },
            {"tool_choice", "auto"}
        };


        //  giving request to models 

        /*This code snippet uses the cpr (C++ Requests) library to send an HTTP POST
        request to an OpenAI-compatible API endpoint (like DeepSeek or OpenAI)
        to generate text. It sets necessary authentication (Bearer token)
        and content type headers, passing a JSON-dumped request body,
        and stores the resulting API response.
        */

        cpr::Response response = cpr::Post(
            cpr::Url{ base_url + "/chat/completions" },
            cpr::Header{
                {"Authorization", "Bearer " + api_key},
                {"Content-Type", "application/json"}
            },
            cpr::Body{ request_body.dump() }  // gets json dump
        );

        // connection check

        if (response.error) {
            std::cerr << "HTTP error: " << response.error.message << std::endl;
            return 1;
        }

        if (response.status_code < 200 || response.status_code >= 300) {
            std::cerr << "HTTP error: " << response.status_code << "\n"
                << response.text << std::endl;
            return 1;
        }

        // converting string of JSON data into native data type to make the work easy on them like usaul cpp objects
        json result;
        try{
            result = json::parse(response.text);
        }
        catch (...) {
            std::cerr << "Invalid JSON response \n";
        }

        if (!result.contains("choices")||
            !result["choices"].is_array() ||
            result["choices"].empty() ||
            !result["choices"][0].contains("message")) {

            std::cerr << "No choices in response" << std::endl;
            return 1;
        }

        json message = result["choices"][0]["message"];

        // append  assistant message
        if (!message.contains("role")) {
            message["role"] = "assistant";
        }
        messages.push_back(message);

        //check for the tool calls
        if (message.contains("tool_calls")) {
            for (auto& call : message["tool_calls"]) {
                std::string tool_id = call["id"];
                std::string name = call["function"]["name"];
                std::string args_str = call["function"]["arguments"];

                json args = json::parse(args_str);

                Tool* tool = registry.get(name);
                std::string tool_result = "ERROR: TOOL NOT FOUND";

                if (tool) {
                    tool_result = tool->execute(args);
                }

                //append tool result
                messages.push_back({
                    {"role", "tool"},
                    {"tool_call_id", tool_id},
                    {"content", tool_result}
                });
            }

            continue;
        }

        if (message.contains("content")) {
            std::cout << message["content"].get<std::string>() << std::endl;
        }

        break;
    }

    return 0;
}
