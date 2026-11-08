#include "kvClient.h"
#include "utils.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

using namespace std;

// 64kb
const size_t KV_CHUNK_SIZE = 65536;

struct KVNode {
    string ip;
    int port;
    char start_key;
};

int getTargetPort(const string& cmd){
    // extract the row key from the command
    stringstream ss(cmd);
    string action;
    string rowKey;
    ss >> action >> rowKey;

    // if no row key found, fallback to 8001
    if(rowKey.empty()) return 8001;

    int sock = socket(PF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    // backend coordinator port
    addr.sin_port = htons(9000);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if(connect(sock, (struct sockaddr*) &addr, sizeof(addr)) == 0) {
        // ask the coordinator for the primary node handling this rowKey
        string lookupCmd = "LOOKUP " + rowKey + "\r\n";
        send(sock, lookupCmd.c_str(), lookupCmd.length(), 0);

        char buf[256];
        ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
        close(sock);

        if(n > 0) {
            buf[n] = '\0';
            string response(buf);
            stringstream ssResponse(response);
            string status;
            string ip;
            int port = 8001;

            // expecting "+OK 127.0.0.1 8002" from the coordinator
            if(ssResponse >> status >> ip >> port && status == "+OK") {
                return port;
            }
        }
    } 
    
    else close(sock);
    
    // fallback if coordinator is down or returns an error
    return 8001;
}

bool socketSendAll(int sock, const string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = send(sock, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool socketReadLine(int sock, string& line) {
    line.clear();
    char buffer[4096];
    
    // allow up to 128 KB to support the 64 KB chunk sizes
    while (line.size() < 131072) { 
        // peek incoming data without removing it from the socket queue
        ssize_t n = recv(sock, buffer, sizeof(buffer), MSG_PEEK);
        if (n <= 0) return false;

        string peeked(buffer, n);
        size_t newLinePos = peeked.find('\n');

        if (newLinePos != string::npos) {
            // new line found, read up to the newline.
            recv(sock, buffer, newLinePos + 1, 0);
            line.append(buffer, newLinePos + 1);
            
            if (line.size() >= 2 && line[line.size() - 2] == '\r') {
                line.resize(line.size() - 2);
                return true;
            }
        } else {
            // no newline in this batch, consume it all and keep looking
            recv(sock, buffer, n, 0);
            line.append(buffer, n);
        }
    }
    return false;
}

string sendCommandKV(const string& cmd) {
    int target_port = getTargetPort(cmd);
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return "-ERR Node Unavailable\r\n";

    timeval timeout{3, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(target_port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(sock);
        return "-ERR Node Unavailable\r\n";
    }

    string greeting;
    string response;
    bool ok = socketReadLine(sock, greeting) &&
              greeting == "+OK KVStorage ready" &&
              socketSendAll(sock, cmd + "\r\n") &&
              socketReadLine(sock, response);
    close(sock);

    if (!ok) return "-ERR KV Communication Failed\r\n";
    return response + "\r\n";
}

string getSessionUser(const string& headers) {
    // Find the Cookie header
    size_t cookie_pos = headers.find("Cookie: ");
    if (cookie_pos == string::npos) return "";
    
    // Extract just the cookie line
    size_t end_pos = headers.find("\r\n", cookie_pos);
    string cookie_line = headers.substr(cookie_pos + 8, end_pos - (cookie_pos + 8));
    
    // Find the session_id token
    size_t sid_pos = cookie_line.find("session_id=");
    if (sid_pos == string::npos) return "";
    
    // Extract the token value up to the next semicolon or end of line
    size_t sid_end = cookie_line.find(";", sid_pos);
    if (sid_end == string::npos) sid_end = cookie_line.length();
    
    string session_id = cookie_line.substr(sid_pos + 11, sid_end - (sid_pos + 11));
    
    // Query the KV Backend to verify the session
    string kv_resp = sendCommandKV("GET sessions " + session_id);
    
    if (kv_resp.find("+OK ") != string::npos) {
        // Extract the username and strip the \r\n
        string user = kv_resp.substr(4);
        if (!user.empty() && user.back() == '\n') user.pop_back();
        if (!user.empty() && user.back() == '\r') user.pop_back();
        return user;
    }
    
    return ""; // Session invalid or expired
}

bool getLoggedInUser(const string& session_id, string& username) {
    username.clear();
    if (!validTokenKV(session_id, 128)) return false;

    string response = sendCommandKV("GET sessions " + session_id);
    size_t line_end = response.find("\r\n");
    if (line_end != string::npos) response.resize(line_end);

    if (response.rfind("+OK ", 0) != 0) return false;
    username = response.substr(4);
    return validUsername(username);
}

bool uploadFileChunks(const string& user, const string& filename, const string& file_data) {
    string file_row = "file:" + user + ":" + filename;
    size_t total_size = file_data.size();
    size_t total_chunks = (total_size == 0) ? 0 : (total_size + KV_CHUNK_SIZE - 1) / KV_CHUNK_SIZE;

    // Store file metadata
    string meta_cmd = "PUT " + file_row + " meta_chunks " + to_string(total_chunks);
    if (sendCommandKV(meta_cmd).find("+OK") == string::npos) return false;

    // Upload each chunk sequentially
    for (size_t i = 0; i < total_chunks; ++i) {
        size_t offset = i * KV_CHUNK_SIZE;
        size_t len = min(KV_CHUNK_SIZE, total_size - offset);
        string slice = file_data.substr(offset, len);
        string b64_chunk = base64Encode(slice);

        if (sendCommandKV("PUT " + file_row + " c_" + to_string(i) + " " + b64_chunk).find("+OK") == string::npos) {
            cerr << "[ERROR] Upload failed at chunk " << i << " for " << filename << endl;
            return false;
        }
    }

    // Register the file in the user's directory listing
    sendCommandKV("PUT files:" + user + " " + filename + " [FILE]");

    // Update the __file_index__
    string idx_resp = sendCommandKV("GET files:" + user + " __file_index__");
    string current_idx = "";
    if (idx_resp.find("+OK ") != string::npos) {
        current_idx = idx_resp.substr(4);
        while (!current_idx.empty() && (current_idx.back() == '\r' || current_idx.back() == '\n')) current_idx.pop_back();
    }
    
    // Only append if it doesn't already exist in the comma-separated string
    if (current_idx.empty() || current_idx.find(filename) == string::npos) {
        if (!current_idx.empty()) current_idx += ",";
        current_idx += filename;
        sendCommandKV("PUT files:" + user + " __file_index__ " + current_idx);
    }
    
    return true;
}

bool downloadFileChunks(const string& user, const string& filename, string& out_data) {
    out_data.clear();
    string file_row = "file:" + user + ":" + filename;

    // Fetch total chunk count
    string resp = sendCommandKV("GET " + file_row + " meta_chunks");
    if (resp.find("+OK ") == string::npos) return false;

    string chunk_count_str = resp.substr(4);
    while (!chunk_count_str.empty() && (chunk_count_str.back() == '\r' || chunk_count_str.back() == '\n')) {
        chunk_count_str.pop_back();
    }
    size_t total_chunks = stoul(chunk_count_str);

    // Fetch and decode every chunk in order
    for (size_t i = 0; i < total_chunks; ++i) {
        string chunk_col = "c_" + to_string(i);
        string chunk_resp = sendCommandKV("GET " + file_row + " " + chunk_col);

        if (chunk_resp.find("+OK ") == string::npos) {
            cerr << "[ERROR] Missing chunk " << i << " for " << filename << endl;
            return false;
        }

        string b64_chunk = chunk_resp.substr(4);
        while (!b64_chunk.empty() && (b64_chunk.back() == '\r' || b64_chunk.back() == '\n')) {
            b64_chunk.pop_back();
        }

        out_data += base64Decode(b64_chunk);
    }

    return true;
}