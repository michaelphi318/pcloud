#include "utils.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <chrono>
#include <iomanip>

using namespace std;

string readHTMLfile(const string& filepath) {
    ifstream file(filepath);
    if(!file.is_open()) return "<h1>500 Internal Server Error</h1><p>Template missing: " + filepath + "</p>";
    stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

void findAndReplaceTemplate(string& html, const string& placeholder, const string& data) {
    size_t pos = 0;
    while((pos = html.find(placeholder, pos)) != string::npos) {
        html.replace(pos, placeholder.length(), data);
        pos += data.length();
    }
}

string escapeHTML(const string& input) {
    string output;
    output.reserve(input.size());
    for (char c : input) {
        switch (c) {
            case '&': output += "&amp;"; break;
            case '<': output += "&lt;"; break;
            case '>': output += "&gt;"; break;
            case '"': output += "&quot;"; break;
            case '\'': output += "&#39;"; break;
            default: output += c; break;
        }
    }
    return output;
}

bool validTokenKV(const string& value, size_t max_length) {
    if (value.empty() || value.size() > max_length) return false;
    for (unsigned char c : value) {
        if (!isalnum(c) && c != '_' && c != '-') return false;
    }
    return true;
}

bool validUsername(const string& username) {
    return validTokenKV(username, 64);
}

string decodePercentURL(string str) {
    string ret;
    auto hex_value = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    for (size_t i = 0; i < str.size(); ++i) {
        if (str[i] == '%' && i + 2 < str.size()) {
            int hi = hex_value(str[i + 1]);
            int lo = hex_value(str[i + 2]);
            if (hi >= 0 && lo >= 0) {
                ret += static_cast<char>((hi << 4) | lo);
                i += 2;
            } else {
                ret += str[i];
            }
        } else if (str[i] == '+') {
            ret += ' ';
        } else {
            ret += str[i];
        }
    }
    return ret;
}

map<string, string> parseFormData(const string& body) {
    map<string, string> params;
    stringstream ss(body);
    string pair;
    while (getline(ss, pair, '&')) {
        size_t eq = pair.find('=');
        if (eq != string::npos) {
            string key = decodePercentURL(pair.substr(0, eq));
            string val = decodePercentURL(pair.substr(eq + 1));
            params[key] = val;
        }
    }
    return params;
}

UploadedFile parseMultipartFile(const string& body, const string& content_type) {
    UploadedFile result;
    
    string boundary_prefix = "boundary=";
    size_t b_pos = content_type.find(boundary_prefix);
    if (b_pos == string::npos) return result;
    
    string boundary_val = content_type.substr(b_pos + boundary_prefix.length());
    
    // Strip carriage returns, spaces, or browser quotation marks
    while (!boundary_val.empty() && (boundary_val.back() == '\r' || boundary_val.back() == '\n' || boundary_val.back() == ' ')) {
        boundary_val.pop_back();
    }
    if (!boundary_val.empty() && boundary_val.front() == '"') boundary_val.erase(0, 1);
    if (!boundary_val.empty() && boundary_val.back() == '"') boundary_val.pop_back();

    string boundary = "--" + boundary_val;
    
    // Scan through all parts of the multipart form data
    size_t part_start = body.find(boundary);
    while (part_start != string::npos) {
        size_t header_end = body.find("\r\n\r\n", part_start);
        if (header_end == string::npos) break;
        
        string part_headers = body.substr(part_start, header_end - part_start);
        size_t fn_pos = part_headers.find("filename=\"");
        
        // If we found the chunk containing the file, extract it!
        if (fn_pos != string::npos) {
            size_t fn_end = part_headers.find("\"", fn_pos + 10);
            if (fn_end != string::npos) {
                result.filename = part_headers.substr(fn_pos + 10, fn_end - (fn_pos + 10));
            }

            size_t data_start = header_end + 4;
            
            // HTTP multipart appends \r\n before the final boundary
            size_t data_end = body.find("\r\n" + boundary, data_start);
            
            // Fallback in case \r\n is omitted by the client
            if (data_end == string::npos) {
                data_end = body.find(boundary, data_start);
            }

            if (data_end != string::npos) {
                result.file_data = body.substr(data_start, data_end - data_start);
            }
            return result; 
        }
        
        // Move to the next boundary chunk
        part_start = body.find(boundary, header_end);
    }
    
    return result;
}

static const string b64_chars = 
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

string base64Encode(const string& in) {
    string out;
    int val = 0, valb = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(b64_chars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(b64_chars[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

string base64Decode(const string& in) {
    string out;
    vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) T[b64_chars[i]] = i;
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (T[c] == -1) break;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

string errorPage(const string& title, const string& message) {
    return "<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><title>" +
           escapeHTML(title) + "</title></head><body><h1>" + escapeHTML(title) +
           "</h1><p>" + escapeHTML(message) +
           "</p><p><a href=\"/inbox\">Back to inbox</a></p></body></html>";
}

void writeClusterLog(const std::string& level, const std::string& node, const std::string& message) {
    // append mode ensures multiple processes can safely write to it
    std::ofstream logfile("../backend/kvStorage/cluster.log ", std::ios_base::app);
    if(!logfile.is_open()) return;

    auto now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    std::tm* local_tm = std::localtime(&now_c);

    logfile << "[" << std::put_time(local_tm, "%H:%M:%S") << "] " 
            << level << ": " << node << " " << message << std::endl;
}

