#ifndef HANDLERS_H
#define HANDLERS_H

#include <string>
#include <map>

struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::string headers;
    std::string body;
    std::string sessionId;
    std::map<std::string, std::string> formParams;
};

struct HttpResponse {
    std::string status = "200 OK";
    std::string contentType = "text/html";
    std::string body = "";
    std::string extraHeaders = "";

    void redirect(const std::string& location, const std::string& setCookie = "") {
        status = "302 Found";
        extraHeaders = "Location: " + location + "\r\n";
        if (!setCookie.empty()) {
            extraHeaders += "Set-Cookie: " + setCookie + "\r\n";
        }
        body = "";
    }
};

// HTTP server to receive the complete request headers
void handleClient(int client_sock);

#endif