#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <map>
#include <vector>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
#include <cstdlib>
#include <sys/time.h>

using namespace std;

// HTML & Templating
string read_html_file(const string& filepath) {
    ifstream file(filepath);
    if (!file.is_open()) return "<h1>500 Internal Server Error</h1><p>Template missing: " + filepath + "</p>";
    stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

void inject_template_var(string& html, const string& placeholder, const string& data) {
    size_t pos = 0;
    while ((pos = html.find(placeholder, pos)) != string::npos) {
        html.replace(pos, placeholder.length(), data);
        pos += data.length();
    }
}

// HTTP Parsing & Encoding
string url_decode(string str) {
    string ret;
    char ch;
    int i, ii;
    for (i = 0; i < str.length(); i++) {
        if (str[i] == '%') {
            sscanf(str.substr(i + 1, 2).c_str(), "%x", &ii);
            ch = static_cast<char>(ii);
            ret += ch;
            i = i + 2;
        } else if (str[i] == '+') {
            ret += ' ';
        } else {
            ret += str[i];
        }
    }
    return ret;
}


map<string, string> parse_form_data(const string& body) {
    map<string, string> params;
    stringstream ss(body);
    string pair;
    while (getline(ss, pair, '&')) {
        size_t eq = pair.find('=');
        if (eq != string::npos) {
            string key = url_decode(pair.substr(0, eq));
            string val = url_decode(pair.substr(eq + 1));
            params[key] = val;
        }
    }
    return params;
}

//parse node backend
struct KVNode {
    string ip;
    int port;
    char start_key;
};

int get_target_port(const string& cmd){
    ifstream file("../backend/kvStorage/nodes.conf");
    string line;
    // Scan nodes.conf and return the first responding port
    while (getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        stringstream ls(line);
        string ip; int port; char sk;
        if (ls >> ip >> port >> sk) {
            int sock = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in addr;
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
            
            if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                close(sock);
                return port;
            }
            close(sock);
        }
    }
    return 8001; 
}


// send endpoint to backend
string send_kv_command(const string& cmd) {
    int target_port = get_target_port(cmd);
    // Connects to the KV backend proxy node
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(target_port); 
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return "-ERR Node Unavailable\r\n";
    }

    char buffer[4096] = {0};
    //Read the initial "+OK KVStorage ready" greeting
    recv(sock, buffer, sizeof(buffer) - 1, 0); 
    
    // Send the actual command
    string formatted_cmd = cmd + "\r\n"; // must end in \r\n
    send(sock, formatted_cmd.c_str(), formatted_cmd.length(), 0);
    
    // Receive the response
    memset(buffer, 0, sizeof(buffer));
    recv(sock, buffer, sizeof(buffer) - 1, 0);
    
    close(sock);
    return string(buffer);
}

string get_session_user(const string& headers) {
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
    string kv_resp = send_kv_command("GET sessions " + session_id);
    
    if (kv_resp.find("+OK ") != string::npos) {
        // Extract the username and strip the \r\n
        string user = kv_resp.substr(4);
        if (!user.empty() && user.back() == '\n') user.pop_back();
        if (!user.empty() && user.back() == '\r') user.pop_back();
        return user;
    }
    
    return ""; // Session invalid or expired
}

struct UploadedFile {
    string filename;
    string file_data;
};

UploadedFile parse_multipart_file(const string& body, const string& content_type) {
    UploadedFile result;
    
    // Extract the boundary string from the Content-Type header
    string boundary_prefix = "boundary=";
    size_t b_pos = content_type.find(boundary_prefix);
    if (b_pos == string::npos) return result;
    
    string boundary = "--" + content_type.substr(b_pos + boundary_prefix.length());
    
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
            result.filename = part_headers.substr(fn_pos + 10, fn_end - (fn_pos + 10));

            size_t data_start = header_end + 4;
            size_t data_end = body.find("\r\n" + boundary, data_start);
            if (data_end != string::npos) {
                result.file_data = body.substr(data_start, data_end - data_start);
            }
            return result; 
        }
        
        // If this chunk wasn't the file (e.g., hidden inputs), jump to the next boundary
        part_start = body.find(boundary, header_end);
    }
    
    return result;
}

static const string b64_chars = 
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

string base64_encode(const string& in) {
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

string base64_decode(const string& in) {
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

// HTTP server to receive the complete request headers
void handle_client(int client_sock) {
    // Stop waiting if a client sends no data for 5 seconds
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    char buffer[8192];
    string request = "";

    // Keep reading until we receive all HTTP headers
    while (request.find("\r\n\r\n") == string::npos) {
        int bytes_read = recv(client_sock, buffer, sizeof(buffer), 0);

        if (bytes_read <= 0) {
            close(client_sock);
            return;
        }

        request.append(buffer, bytes_read);

        // Prevent excessively large request headers
        if (request.size() > 16384) {
            close(client_sock);
            return;
        }
    }

    size_t header_end = request.find("\r\n\r\n");


    string headers = request.substr(0, header_end);
    string body = request.substr(header_end + 4);

    // Extract session ID from the Cookie header
    string session_id = "";
    size_t cookie_pos = headers.find("session_id=");

    if (cookie_pos != string::npos) {
        size_t start = cookie_pos + 11;
        size_t end = headers.find_first_of(";\r\n", start);
        session_id = headers.substr(start, end - start);
    }

    // Extract Method and Path
    stringstream hdr_stream(headers);
    string method, path, version;
    hdr_stream >> method >> path >> version;

    // Check for query parameters (e.g., /?error=invalid)
    string query = "";
    size_t q_mark = path.find('?');
    if (q_mark != string::npos) {
        query = path.substr(q_mark + 1);
        path = path.substr(0, q_mark);
    }

    // Extract Content-Length for POST bodies

    // Read and validate POST request body
    if (method == "POST") {
        const size_t MAX_BODY_SIZE = 1024 * 1024; // 1MB
        size_t content_length = 0;
        bool valid_length = false;
        bool too_large = false;

        size_t cl_pos = headers.find("Content-Length: ");

        if (cl_pos != string::npos) {
            size_t start = cl_pos + 16;
            size_t end = headers.find("\r\n", start);
            string length_str = headers.substr(start, end - start);

            // Check that Content-Length contains only digits
            valid_length = !length_str.empty();

            for (char c : length_str) {
                if (c < '0' || c > '9') {
                    valid_length = false;
                    break;
                }

                size_t digit = c - '0';

                // Avoid overflow and reject oversized bodies
                if (content_length > (MAX_BODY_SIZE - digit) / 10) {
                    too_large = true;
                    break;
                }

                content_length = content_length * 10 + digit;
            }
        }

        string error_status = "";

        if (too_large) {
            error_status = "413 Payload Too Large";
        } else if (!valid_length || cl_pos == string::npos) {
            error_status = "400 Bad Request";
        } else {
            // Keep reading until the full POST body arrives
            while (body.size() < content_length) {
                char temp[4096];
                size_t remaining = content_length - body.size();
                size_t to_read = min(remaining, sizeof(temp));

                int bytes = recv(client_sock, temp, to_read, 0);

                if (bytes <= 0) {
                    error_status = "400 Bad Request";
                    break;
                }

                body.append(temp, bytes);
            }
        }

        // Reject invalid or incomplete requests
        if (!error_status.empty()) {
            string response = "HTTP/1.1 " + error_status + "\r\n"
                              "Content-Length: 0\r\n"
                              "Connection: close\r\n\r\n";

            send(client_sock, response.c_str(), response.size(), 0);
            close(client_sock);
            return;
        }

        // Use only the bytes specified by Content-Length
        body.resize(content_length);
    }


    // Prepare Response Variables
    string status = "200 OK";
    string content_type = "text/html";
    string response_body = "";
    string extra_headers = "";

    // endpoint forwarding backend
    // -- ROUTING LOGIC --
    if (method == "GET") {
        if (path == "/" || path == "/login") {
            response_body = read_html_file("login.html");
            if (query.find("error=invalid_credentials") != string::npos) {
                string error_html = R"(<div style="background-color: #ffe4e4; color: #990000; padding: 12px; text-align: center; border-bottom: 1px solid #ffcccc; font-weight: 600;">Invalid username or password. Please try again.</div>)";
                inject_template_var(response_body, "{{ERROR_BANNER}}", error_html);
            } else {
                inject_template_var(response_body, "{{ERROR_BANNER}}", "");
            }
        }
        else if (path == "/signup") { 
            response_body = read_html_file("signup.html"); 
        }
        else if (path == "/dashboard" || path == "/inbox" || path == "/compose" || path == "/drive") {
            // ENFORCE AUTHENTICATION
            string active_user = get_session_user(headers);
            if (active_user.empty()) {
                status = "302 Found";
                extra_headers = "Location: /login\r\n";
                response_body = "";
            } else {
                // Render the protected pages dynamically
                if (path == "/dashboard") {
                    response_body = read_html_file("dashboard.html"); 
                    inject_template_var(response_body, "{{USERNAME}}", active_user);
                }
                else if (path == "/inbox") { response_body = read_html_file("inbox.html"); }
                else if (path == "/compose") { response_body = read_html_file("compose.html"); }
                else if (path == "/drive") { response_body = read_html_file("drive.html"); }
            }
        }
        else if (path == "/api/download") {
            string active_user = get_session_user(headers);
            if (active_user.empty()) {
                status = "401 Unauthorized";
                response_body = "<h1>401 Unauthorized</h1>";
            } else {
                string filename = "";
                size_t f_pos = query.find("file=");
                if (f_pos != string::npos) {
                    filename = url_decode(query.substr(f_pos + 5));
                }

                if (!filename.empty()) {
                    string kv_resp = send_kv_command("GET files:" + active_user + " " + filename);
                    if (kv_resp.find("+OK ") != string::npos) {
                        string b64_data = kv_resp.substr(4);
                        string raw_file_bytes = base64_decode(b64_data);
                        status = "200 OK";
                        content_type = "application/octet-stream";
                        extra_headers = "Content-Disposition: attachment; filename=\"" + filename + "\"\r\n";
                        response_body = raw_file_bytes;
                    } else {
                        status = "404 Not Found";
                        response_body = "<h1>404 File Not Found in KV Store</h1>";
                    }
                }
            }
        }
        else if (path == "/admin") { 
            response_body = read_html_file("admin.html"); 
        }
        else if (path == "/logout") {
            if (!session_id.empty()) {
                send_kv_command("DELETE sessions " + session_id);
            }
            status = "302 Found";
            extra_headers = "Location: /login\r\nSet-Cookie: session_id=; Max-Age=0; Path=/\r\n";
        }
        else {
            status = "404 Not Found";
            response_body = "<h1>404 Not Found</h1>";
        }
    } 
    else if (method == "POST") {
        map<string, string> form = parse_form_data(body);

        if (path == "/api/login") {
            string user = form["username"];
            string pass = form["password"];
            string kv_resp = send_kv_command("GET users " + user);
            
            if (kv_resp.find("+OK " + pass) != string::npos) {
                string session_id = to_string(time(0)) + "_" + user;
                send_kv_command("PUT sessions " + session_id + " " + user);
                status = "302 Found";
                extra_headers = "Location: /dashboard\r\nSet-Cookie: session_id=" + session_id + "; HttpOnly; Path=/\r\n";
            } else {
                status = "302 Found";
                extra_headers = "Location: /?error=invalid_credentials\r\n";
            }
        } 
        else if (path == "/api/signup") {
            string user = form["username"];
            string pass = form["password"];
            string kv_resp = send_kv_command("CPUT users " + user + " [EMPTY] " + pass);
            
            if (kv_resp.find("+OK") != string::npos) {
                status = "302 Found";
                extra_headers = "Location: /login\r\n";
            } else {
                status = "302 Found";
                extra_headers = "Location: /signup?error=user_exists\r\n";
            }
        }
        else if (path == "/api/upload") {
            string active_user = get_session_user(headers);
            if (active_user.empty()) {
                status = "401 Unauthorized";
                response_body = "<h1>401 Unauthorized</h1>";
            } else {
                size_t ct_pos = headers.find("Content-Type: ");
                string req_content_type = "";
                if (ct_pos != string::npos) {
                    size_t ct_end = headers.find("\r\n", ct_pos);
                    req_content_type = headers.substr(ct_pos + 14, ct_end - (ct_pos + 14));
                }

                UploadedFile uploaded = parse_multipart_file(body, req_content_type);
                if (!uploaded.filename.empty() && !uploaded.file_data.empty()) {
                    string b64_data = base64_encode(uploaded.file_data);
                    string kv_cmd = "PUT files:" + active_user + " " + uploaded.filename + " " + b64_data;
                    send_kv_command(kv_cmd);
                    
                    status = "302 Found";
                    extra_headers = "Location: /drive\r\n";
                } else {
                    status = "400 Bad Request";
                    response_body = "<h1>Upload Failed</h1>";
                }
            }
        }
        // MOVED FROM GET BLOCK: Delete, Rename, and Create Folder
        else if (path == "/api/delete") {
            string active_user = get_session_user(headers);
            if (active_user.empty()) {
                status = "401 Unauthorized";
            } else {
                string filename = form["filename"];
                if (!filename.empty()) {
                    send_kv_command("DELETE files:" + active_user + " " + filename);
                }
                status = "302 Found";
                extra_headers = "Location: /drive\r\n";
            }
        }
        else if (path == "/api/rename") {
            string active_user = get_session_user(headers);
            if (active_user.empty()) {
                status = "401 Unauthorized";
            } else {
                string old_name = form["old_name"];
                string new_name = form["new_name"];
                
                if (!old_name.empty() && !new_name.empty()) {
                    string kv_resp = send_kv_command("GET files:" + active_user + " " + old_name);
                    if (kv_resp.find("+OK ") != string::npos) {
                        string b64_data = kv_resp.substr(4);
                        send_kv_command("PUT files:" + active_user + " " + new_name + " " + b64_data);
                        send_kv_command("DELETE files:" + active_user + " " + old_name);
                    }
                }
                status = "302 Found";
                extra_headers = "Location: /drive\r\n";
            }
        }
        else if (path == "/api/create_folder") {
            string active_user = get_session_user(headers);
            if (active_user.empty()) {
                status = "401 Unauthorized";
            } else {
                string folder_name = form["folder_name"];
                if (!folder_name.empty()) {
                    string dir_key = folder_name + "/";
                    send_kv_command("PUT files:" + active_user + " " + dir_key + " [DIR_MARKER]");
                }
                status = "302 Found";
                extra_headers = "Location: /drive\r\n";
            }
        }
        else {
            status = "404 Not Found";
            response_body = "<h1>404 API Endpoint Not Found</h1>";
        }
    }

    // Construct and Send Raw HTTP Response
    string response = "HTTP/1.1 " + status + "\r\n" +
                      "Content-Type: " + content_type + "\r\n" +
                      "Content-Length: " + to_string(response_body.length()) + "\r\n" +
                      extra_headers +
                      "Connection: close\r\n\r\n" + 
                      response_body;

    send(client_sock, response.c_str(), response.length(), 0);
    close(client_sock);
}

int main() {
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8000); // Frontend listens on 8000
    addr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(server_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        cerr << "[ERROR] Bind failed on port 8000" << endl;
        return 1;
    }

    listen(server_sock, 50);
    cout << "[FRONTEND] POSIX Web Server initializing on port 8000..." << endl;

    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);
        
        if (client_sock >= 0) {
            // Detach a new std::thread to handle the request concurrently without blocking the accept loop
            thread(handle_client, client_sock).detach();
        }
    }

    close(server_sock);
    return 0;
}