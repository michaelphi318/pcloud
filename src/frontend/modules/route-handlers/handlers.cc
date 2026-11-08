#include "handlers.h"
#include "utils.h"
#include "kvClient.h"
#include "webmail.h"
#include <iostream>
#include <sstream>
#include <fstream>
#include <unordered_map>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <algorithm>

using namespace std;

// create webmail service which sharing with KV storage
extern webmail::Webmail webmail_service;

using RouteHandler = void (*)(const HttpRequest&, HttpResponse&);

/**
 * admin console helper function 
 */
std::string sendAdminCommand(int port, const std::string& cmd) {
    int sock = socket(PF_INET, SOCK_STREAM, 0);
    if(sock < 0) return "";

    struct sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    // timeout so the frontend doesn't hang if a node is dead
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*) &tv, sizeof tv);

    if(connect(sock, (struct sockaddr*) &addr, sizeof(addr)) < 0) {
        close(sock);
        return "";
    }

    // ignore +OK greeting for KV Storage nodes
    if (port != 9000) {
        char dumpBuf[256];
        recv(sock, dumpBuf, sizeof(dumpBuf), 0);
    }

    std::string fullCmd = cmd + "\r\n";
    send(sock, fullCmd.c_str(), fullCmd.length(), 0);

    std::string result = "";
    char buffer[4096];
    ssize_t n;
    while((n = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[n] = '\0';
        result += buffer;

        // if we read less than max buffer size, reached the end of data
        if(n < 4095) break;
    }
    
    close(sock);
    return result;
}

/**
 * get live cluster logs
 */
std::string getLiveLogs() {
    std::ifstream logfile("../backend/kvStorage/cluster.log ");
    if(!logfile.is_open()) return "<span class=\"log-warn\">No cluster logs found. Awaiting events...</span><br>";

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(logfile, line)) {
        if(!line.empty()) lines.push_back(line);
    }

    // grab only the last 15 lines
    size_t start = lines.size() > 15 ? lines.size() - 15 : 0;
    std::string formattedLogs = "";

    for(size_t i = start; i < lines.size(); i++) {
        std::string cssClass = "log-info";
        size_t splitPos = std::string::npos;

        if(lines[i].find(" ERROR:") != std::string::npos) {
            cssClass = "log-error";
            splitPos = lines[i].find(" ERROR:") + 7;
        }
        else if(lines[i].find(" WARN:") != std::string::npos) {
            cssClass = "log-warn";
            splitPos = lines[i].find(" WARN:") + 6;
        } 
        else if(lines[i].find(" INFO:") != std::string::npos) {
            cssClass = "log-info";
            splitPos = lines[i].find(" INFO:") + 6;
        }

        // Colorize the timestamp/level prefix, leave the message raw white text
        if(splitPos != std::string::npos && lines[i].length() > splitPos) {
            std::string prefix = lines[i].substr(0, splitPos);
            std::string msg = lines[i].substr(splitPos);
            formattedLogs += "<span class=\"" + cssClass + "\">" + prefix + "</span>" + msg + "<br>";
        }
        else {
            formattedLogs += "<span class=\"" + cssClass + "\">" + lines[i] + "</span><br>";
        }
    }
    return formattedLogs;
}

// GET handlers

void handleGetLogin(const HttpRequest& req, HttpResponse& res) {
    res.body = readHTMLfile("pages/login.html");
    if (req.query.find("error=invalid_credentials") != string::npos) {
        string error_html = R"(<div style="background-color: #ffe4e4; color: #990000; padding: 12px; text-align: center; border-bottom: 1px solid #ffcccc; font-weight: 600;">Invalid username or password. Please try again.</div>)";
        findAndReplaceTemplate(res.body, "{{ERROR_BANNER}}", error_html);
    } else {
        findAndReplaceTemplate(res.body, "{{ERROR_BANNER}}", "");
    }
}

void handleGetSignup(const HttpRequest& req, HttpResponse& res) {
    res.body = readHTMLfile("pages/signup.html");
}

void handleGetDashboard(const HttpRequest& req, HttpResponse& res) {
    string activeUser = getSessionUser(req.headers);
    if (activeUser.empty()) {
        res.redirect("/login");
        return;
    }
    res.body = readHTMLfile("pages/dashboard.html");
    findAndReplaceTemplate(res.body, "{{USERNAME}}", activeUser);
}

void handleGetDrive(const HttpRequest& req, HttpResponse& res) {
    string activeUser = getSessionUser(req.headers);
    if (activeUser.empty()) {
        res.redirect("/login");
        return;
    }

    res.body = readHTMLfile("pages/drive.html");
    string idxResp = sendCommandKV("GET files:" + activeUser + " __file_index__");
    string fileListStr = "";

    if (idxResp.find("+OK ") != string::npos) {
        fileListStr = idxResp.substr(4);
        while (!fileListStr.empty() && (fileListStr.back() == '\r' || fileListStr.back() == '\n')) {
            fileListStr.pop_back();
        }
    }

    string rowsHtml = "";
    if (!fileListStr.empty()) {
        stringstream ss(fileListStr);
        string item;
        while (getline(ss, item, ',')) {
            if (item.empty()) continue;
            bool isDir = (item.back() == '/');

            if (isDir) {
                string folderDisplay = item.substr(0, item.length() - 1);
                rowsHtml += 
                    "<div class=\"file-row\">"
                        "<div class=\"file-icon\">"
                            "<svg width=\"24\" height=\"24\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\">"
                                "<path d=\"M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z\"></path>"
                            "</svg>"
                        "</div>"
                        "<div class=\"file-name\"><a href=\"/drive?path=/" + escapeHTML(folderDisplay) + "\">" + escapeHTML(folderDisplay) + "</a></div>"
                        "<div class=\"file-size\">--</div>"
                        "<div class=\"file-actions\">"
                            "<form method=\"POST\" action=\"/api/delete\" style=\"display:inline;\">"
                                "<input type=\"hidden\" name=\"filename\" value=\"" + escapeHTML(item) + "\">"
                                "<button type=\"submit\" class=\"action-link danger\" style=\"background:none;border:none;cursor:pointer;\">Delete</button>"
                            "</form>"
                        "</div>"
                    "</div>";
            } else {
                bool isImg = (item.rfind(".png") != string::npos || item.rfind(".jpg") != string::npos || item.rfind(".jpeg") != string::npos);
                string iconSvg = isImg 
                    ? "<rect x=\"3\" y=\"3\" width=\"18\" height=\"18\" rx=\"2\" ry=\"2\"></rect><circle cx=\"8.5\" cy=\"8.5\" r=\"1.5\"></circle><polyline points=\"21 15 16 10 5 21\"></polyline>"
                    : "<path d=\"M13 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V9z\"></path><polyline points=\"13 2 13 9 20 9\"></polyline>";

                rowsHtml += 
                    "<div class=\"file-row\">"
                        "<div class=\"file-icon\">"
                            "<svg width=\"24\" height=\"24\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\">" + iconSvg + "</svg>"
                        "</div>"
                        "<div class=\"file-name\">" + escapeHTML(item) + "</div>"
                        "<div class=\"file-size\">--</div>"
                        "<div class=\"file-actions\">"
                            "<a href=\"/api/download?file=" + escapeHTML(item) + "\" class=\"action-link\">Download</a>"
                            "<form method=\"POST\" action=\"/api/delete\" style=\"display:inline;\">"
                                "<input type=\"hidden\" name=\"filename\" value=\"" + escapeHTML(item) + "\">"
                                "<button type=\"submit\" class=\"action-link danger\" style=\"background:none;border:none;cursor:pointer;margin-left:10px;\">Delete</button>"
                            "</form>"
                        "</div>"
                    "</div>";
            }
        }
    }

    if (rowsHtml.empty()) {
        rowsHtml = "<div class=\"file-row\" style=\"justify-content: center; color: #777; padding: 40px;\">No files or folders uploaded yet.</div>";
    }

    findAndReplaceTemplate(res.body, "{{FILE_LIST}}", rowsHtml);
}

void handleGetAccount(const HttpRequest& req, HttpResponse& res) {
    string activeUser = getSessionUser(req.headers);
    if (activeUser.empty()) {
        res.redirect("/login");
        return;
    }
    res.body = readHTMLfile("pages/account.html");
    findAndReplaceTemplate(res.body, "{{USERNAME}}", escapeHTML(activeUser));
}

void handleGetDownload(const HttpRequest& req, HttpResponse& res) {
    string activeUser = getSessionUser(req.headers);
    if (activeUser.empty()) {
        res.status = "401 Unauthorized";
        res.body = "<h1>401 Unauthorized</h1>";
        return;
    }

    string filename = "";
    size_t fPos = req.query.find("file=");
    if (fPos != string::npos) {
        filename = decodePercentURL(req.query.substr(fPos + 5));
    }

    string rawFileBytes;
    if (!filename.empty() && downloadFileChunks(activeUser, filename, rawFileBytes)) {
        res.status = "200 OK";
        res.contentType = "application/octet-stream";
        res.extraHeaders = "Content-Disposition: attachment; filename=\"" + filename + "\"\r\n";
        res.body = rawFileBytes;
    } else {
        res.status = "404 Not Found";
        res.body = "<h1>404 File Not Found in KV Store</h1>";
    }
}

void handleGetInbox(const HttpRequest& req, HttpResponse& res) {
    string username;
    if (!getLoggedInUser(req.sessionId, username)) {
        res.redirect("/login");
        return;
    }

    vector<webmail::Email> emails;
    if (!webmail_service.get_inbox(username, emails)) {
        res.status = "500 Internal Server Error";
        res.body = errorPage("Inbox Error", "Could not retrieve your inbox from KV storage.");
        return;
    }

    res.body = readHTMLfile("pages/inbox.html");
    string rows;
    if (emails.empty()) {
        rows = R"(<div class="empty-inbox">Your inbox is empty.</div>)";
    } else {
        for (const auto& email : emails) {
            rows += "<a href=\"/view_email?id=" + escapeHTML(email.id) + "\" class=\"email-row read\">";
            rows += "<div class=\"email-sender\">" + escapeHTML(email.sender) + "</div>";
            rows += "<div class=\"email-subject\">" + escapeHTML(email.subject) + "</div>";
            rows += "<div class=\"email-date\">" + escapeHTML(email.timestamp) + "</div>";
            rows += "</a>";
        }
    }
    findAndReplaceTemplate(res.body, "{{EMAIL_LIST}}", rows);
}

void handleGetViewEmail(const HttpRequest& req, HttpResponse& res) {
    string username;
    if (!getLoggedInUser(req.sessionId, username)) {
        res.redirect("/login");
        return;
    }

    map<string, string> params = parseFormData(req.query);
    auto idIt = params.find("id");
    if (idIt == params.end() || !validTokenKV(idIt->second, 64)) {
        res.status = "400 Bad Request";
        res.body = errorPage("Invalid Email", "The email ID is missing or invalid.");
        return;
    }

    webmail::Email email;
    if (!webmail_service.get_email(username, idIt->second, email)) {
        res.status = "404 Not Found";
        res.body = errorPage("Email Not Found", "This message does not exist in your inbox.");
        return;
    }

    res.body = readHTMLfile("pages/email_view.html");
    findAndReplaceTemplate(res.body, "{{EMAIL_ID}}", escapeHTML(email.id));
    findAndReplaceTemplate(res.body, "{{SUBJECT}}", escapeHTML(email.subject));
    findAndReplaceTemplate(res.body, "{{SENDER}}", escapeHTML(email.sender));
    findAndReplaceTemplate(res.body, "{{RECIPIENT}}", escapeHTML(email.recipient));
    findAndReplaceTemplate(res.body, "{{TIMESTAMP}}", escapeHTML(email.timestamp));
    findAndReplaceTemplate(res.body, "{{BODY}}", escapeHTML(email.body));
}

void handleGetCompose(const HttpRequest& req, HttpResponse& res) {
    string username;
    if (!getLoggedInUser(req.sessionId, username)) {
        res.redirect("/login");
        return;
    }
    res.body = readHTMLfile("pages/email_compose.html");
}

void handleGetAdmin(const HttpRequest& req, HttpResponse& res) {
    res.body = readHTMLfile("pages/admin_console.html");

    // read the static html file
    std::string html = readHTMLfile("pages/admin_console.html");

    // fetch live STATUS from the Coordinator (Port 9000)
    std::string statusData = sendAdminCommand(9000, "STATUS");
    std::string formattedStatus = "";
    std::vector<int> aliveBackendPorts;

    if(statusData.empty()) {
        formattedStatus = "<li class=\"node-item\"><div class=\"node-info\"><div class=\"status-dot\" style=\"background-color:#d32f2f; animation:none; box-shadow:none;\"></div>Coordinator Offline</div></li>";
    } 
    else {
        // parse coordinator's response and format into html list items
        std::istringstream ss(statusData);
        std::string line;

        while(std::getline(ss, line)) {
            if (line == "+OK" || line.empty()) continue;
            
            std::istringstream ssLine(line);
            std::string partition, role, address, state;

            if(ssLine >> partition >> role >> address >> state) {
                bool isAlive = (state == "ALIVE");
                std::string portStr = address.substr(address.find(':') + 1);

                // if node is alive, add it to our list to query for data dumps
                if(isAlive) {
                    int portNum = stoi(portStr);
                    if (std::find(aliveBackendPorts.begin(), aliveBackendPorts.end(), portNum) == aliveBackendPorts.end()) {
                        aliveBackendPorts.push_back(portNum);
                    }
                }
                
                std::string dotColor = isAlive ? "#4caf50" : "#d32f2f";
                std::string dotAnim = isAlive ? "animation: pulse 2s infinite; box-shadow: 0 0 8px #4caf50;" : "animation: none; box-shadow: none;";
                
                std::string controlBtn = isAlive ? 
                    "<form action=\"/api/admin/stop\" method=\"POST\"><input type=\"hidden\" name=\"node_port\" value=\"" + portStr + "\"><button type=\"submit\" class=\"btn-node btn-stop\">Kill Node</button></form>" 
                    : "<form action=\"/api/admin/start\" method=\"POST\"><input type=\"hidden\" name=\"node_port\" value=\"" + portStr + "\"><button type=\"submit\" class=\"btn-node btn-start\">Start Node</button></form>";

                formattedStatus += "<li class=\"node-item\"><div class=\"node-info\">"
                                    "<div class=\"status-dot\" style=\"background-color:" + dotColor + "; " + dotAnim + "\"></div>"
                                    "Partition [" + partition + "] " + role + " (" + address + ")</div>" + controlBtn + "</li>";
            }
        }
    }

    // fetch live DUMP from the first available Backend Node to show raw data
    std::string formattedDump = "";
    bool dataFetched = false;

    if(aliveBackendPorts.empty()) {
        formattedDump = "<tr><td colspan=\"3\">All Backend Nodes Offline - Cannot fetch data.</td></tr>";
    } 
    else {
        for(int port : aliveBackendPorts) {
            std::string dumpData = sendAdminCommand(port, "DUMP");
            if (dumpData.empty()) continue;

            std::istringstream ssDump(dumpData);
            std::string line;
            std::string currentRow = "";

            while(std::getline(ssDump, line)) {
                if(line == "+OK" || line.empty()) continue;
                
                if(line.find("ROW: ") == 0) {
                    currentRow = line.substr(5);
                }
                else if(line.find("  COL: ") == 0) {
                    std::string colData = line.substr(7);
                    formattedDump += "<tr><td style=\"font-weight: 600;\">" + currentRow + "</td>"
                                    "<td style=\"font-family: monospace; color: #666;\">" + colData + "</td>"
                                    "<td><span style=\"color:#2e7d32; font-size:12px; font-weight:bold;\">Node " + std::to_string(port) + "</span></td></tr>";
                }
            }
            
            dataFetched = true;
            break; 
        }
        
        if(!dataFetched) {
            formattedDump = "<tr><td colspan=\"3\">Could not fetch data from any active node.</td></tr>";
        }
    }

    // inject into HTML unconditionally
    size_t ulStart = html.find("<ul class=\"node-list\">", html.find("Backend Key-Value Store"));
    if(ulStart != std::string::npos) {
        size_t ul_end = html.find("</ul>", ulStart);
        html.replace(ulStart + 22, ul_end - (ulStart + 22), formattedStatus);
    }

    size_t tbodyStart = html.find("<tbody>");
    if(tbodyStart != std::string::npos) {
        size_t tbodyEnd = html.find("</tbody>", tbodyStart);
        html.replace(tbodyStart + 7, tbodyEnd - (tbodyStart + 7), formattedDump);
    }

    // inject live logs into the console box
    size_t consoleStart = html.find("<div class=\"console-box\">");
    if(consoleStart != std::string::npos) {
        size_t consoleEnd = html.find("</div>", consoleStart);
        std::string logsHtml = getLiveLogs() + "<span class=\"cursor\" style=\"animation: blink 1s step-end infinite;\">_</span>";
        html.replace(consoleStart + 25, consoleEnd - (consoleStart + 25), logsHtml); 
    }

    res.body = html;
}

void handleAdminStartNode(const HttpRequest& req, HttpResponse& res) {
    auto form = req.formParams;
    if(form.find("node_port") != form.end()) {
        int target_port = stoi(form["node_port"]);

        writeClusterLog("INFO", "Admin", "Initiated boot sequence for Node " + std::to_string(target_port));
        
        std::string cmd = "cd ../backend/kvStorage && ./kvstore -p " + std::to_string(target_port) + " -c nodes.conf -v &";
        
        int ret = system(cmd.c_str());
        if(ret != 0) std::cerr << "[ERROR] Failed to execute start command for node " << target_port << std::endl;
    }
    res.redirect("/admin");
}

void handleAdminStopNode(const HttpRequest& req, HttpResponse& res) {
    // extract the port from the URL encoded POST body
    auto form = req.formParams;
    if (form.find("node_port") != form.end()) {
        int target_port = stoi(form["node_port"]);

        writeClusterLog("WARN", "Admin", "Sent kill signal to Node " + std::to_string(target_port));
        sendAdminCommand(target_port, "SHUTDOWN");
    }

    // redirect the browser back to the Admin Console page
    res.redirect("/admin");
}

void handleGetLogout(const HttpRequest& req, HttpResponse& res) {
    if (!req.sessionId.empty()) {
        sendCommandKV("DELETE sessions " + req.sessionId);
    }
    res.redirect("/login", "session_id=; Max-Age=0; Path=/");
}


// POST handlers

void handlePostLogin(const HttpRequest& req, HttpResponse& res) {
    auto form = req.formParams;
    string user = form["username"];
    string pass = form["password"];

    string kvResp = sendCommandKV("GET users " + user);
    if (kvResp.find("+OK " + pass) != string::npos) {
        string sessionId = to_string(time(0)) + "_" + user;
        sendCommandKV("PUT sessions " + sessionId + " " + user);
        res.redirect("/dashboard", "session_id=" + sessionId + "; HttpOnly; Path=/");
    } else {
        res.redirect("/?error=invalid_credentials");
    }
}

void handlePostChangePassword(const HttpRequest& req, HttpResponse& res) {
    string activeUser = getSessionUser(req.headers);
    if (activeUser.empty()) {
        res.redirect("/login");
        return;
    }

    auto form = req.formParams;
    string oldPass = form["old_password"];
    string newPass = form["new_password"];
    string confirmPass = form["confirm_password"];

    if (oldPass.empty() || newPass.empty() || !validTokenKV(oldPass) || !validTokenKV(newPass)) {
        res.redirect("/account?error=invalid_input");
    } else if (!confirmPass.empty() && newPass != confirmPass) {
        res.redirect("/account?error=mismatch");
    } else {
        string cmd = "CPUT users " + activeUser + " " + oldPass + " " + newPass;
        string kvResp = sendCommandKV(cmd);
        if (kvResp.find("+OK") != string::npos) {
            res.redirect("/account?status=success");
        } else {
            res.redirect("/account?error=wrong_password");
        }
    }
}

void handlePostSignup(const HttpRequest& req, HttpResponse& res) {
    auto form = req.formParams;
    string user = form["username"];
    string pass = form["password"];

    string kvResp = sendCommandKV("CPUT users " + user + " [EMPTY] " + pass);
    if (kvResp.find("+OK") != string::npos) {
        res.redirect("/login");
    } else {
        res.redirect("/signup?error=user_exists");
    }
}

void handlePostSendEmail(const HttpRequest& req, HttpResponse& res) {
    string username;
    if (!getLoggedInUser(req.sessionId, username)) {
        res.status = "401 Unauthorized";
        res.body = errorPage("Authentication Required", "Please log in before sending email.");
        return;
    }

    auto form = req.formParams;
    string recipient = form["recipient"];
    string subject = form["subject"];
    string messageBody = form["body"];

    if (!validUsername(recipient) || subject.empty() || messageBody.empty() ||
        subject.size() > 200 || messageBody.size() > 500) {
        res.status = "400 Bad Request";
        res.body = errorPage("Invalid Message", "Enter an existing PennCloud username, a subject (maximum 200 bytes), and a message body (maximum 500 bytes).");
        return;
    }

    string emailId;
    if (webmail_service.send_email(username, recipient, subject, messageBody, emailId)) {
        res.redirect("/inbox?sent=1");
    } else {
        res.status = "400 Bad Request";
        res.body = errorPage("Send Failed", "The recipient may not exist, or the message could not be saved. Check the recipient username and try again.");
    }
}

void handlePostUpload(const HttpRequest& req, HttpResponse& res) {
    string activeUser = getSessionUser(req.headers);
    if (activeUser.empty()) {
        res.status = "401 Unauthorized";
        res.body = "<h1>401 Unauthorized</h1>";
        return;
    }

    size_t ctPos = req.headers.find("Content-Type: ");
    if (ctPos == string::npos) ctPos = req.headers.find("content-type: ");
    
    string reqContentType = "";
    if (ctPos != string::npos) {
        size_t ctEnd = req.headers.find("\r\n", ctPos);
        if (ctEnd == string::npos) ctEnd = req.headers.length();
        reqContentType = req.headers.substr(ctPos + 14, ctEnd - (ctPos + 14));
    }

    UploadedFile uploaded = parseMultipartFile(req.body, reqContentType);
    if (!uploaded.filename.empty() && !uploaded.file_data.empty()) {
        if (uploadFileChunks(activeUser, uploaded.filename, uploaded.file_data)) {
            res.redirect("/drive");
        } else {
            res.status = "500 Internal Server Error";
            res.body = "<h1>Failed to save file chunks to KV Store</h1>";
        }
    } else {
        res.status = "400 Bad Request";
        res.body = "<h1>Upload Failed - Missing file data</h1>";
    }
}

void handlePostDelete(const HttpRequest& req, HttpResponse& res) {
    string activeUser = getSessionUser(req.headers);
    if (activeUser.empty()) {
        res.status = "401 Unauthorized";
        return;
    }

    string filename = req.formParams.at("filename");
    if (!filename.empty()) {
        sendCommandKV("DELETE files:" + activeUser + " " + filename);

        if (filename.back() != '/') {
            string fileRow = "file:" + activeUser + ":" + filename;
            string chunkMeta = sendCommandKV("GET " + fileRow + " meta_chunks");
            if (chunkMeta.find("+OK ") != string::npos) {
                size_t totalChunks = stoul(chunkMeta.substr(4));
                for (size_t i = 0; i < totalChunks; ++i) {
                    sendCommandKV("DELETE " + fileRow + " c_" + to_string(i));
                }
                sendCommandKV("DELETE " + fileRow + " meta_chunks");
            }
        }

        string idxResp = sendCommandKV("GET files:" + activeUser + " __file_index__");
        if (idxResp.find("+OK ") != string::npos) {
            string currentIdx = idxResp.substr(4);
            while (!currentIdx.empty() && (currentIdx.back() == '\r' || currentIdx.back() == '\n')) currentIdx.pop_back();

            stringstream ss(currentIdx);
            string token, updatedIdx;
            while (getline(ss, token, ',')) {
                if (token != filename && !token.empty()) {
                    if (!updatedIdx.empty()) updatedIdx += ",";
                    updatedIdx += token;
                }
            }
            if (updatedIdx.empty()) {
                sendCommandKV("DELETE files:" + activeUser + " __file_index__");
            } else {
                sendCommandKV("PUT files:" + activeUser + " __file_index__ " + updatedIdx);
            }
        }
    }
    res.redirect("/drive");
}

void handlePostCreateFolder(const HttpRequest& req, HttpResponse& res) {
    string activeUser = getSessionUser(req.headers);
    if (activeUser.empty()) {
        res.status = "401 Unauthorized";
        return;
    }

    string folderName = req.formParams.at("folder_name");
    if (!folderName.empty()) {
        string dirKey = folderName + "/";
        sendCommandKV("PUT files:" + activeUser + " " + dirKey + " [DIR_MARKER]");

        string idxResp = sendCommandKV("GET files:" + activeUser + " __file_index__");
        string currentIdx = "";
        if (idxResp.find("+OK ") != string::npos) {
            currentIdx = idxResp.substr(4);
            while (!currentIdx.empty() && (currentIdx.back() == '\r' || currentIdx.back() == '\n')) currentIdx.pop_back();
        }
        if (currentIdx.empty() || currentIdx.find(dirKey) == string::npos) {
            if (!currentIdx.empty()) currentIdx += ",";
            currentIdx += dirKey;
            sendCommandKV("PUT files:" + activeUser + " __file_index__ " + currentIdx);
        }
    }
    res.redirect("/drive");
}


// dispatch tables

const unordered_map<string, RouteHandler> getRoutes = {
    {"/",                   handleGetLogin},
    {"/login",              handleGetLogin},
    {"/signup",             handleGetSignup},
    {"/dashboard",          handleGetDashboard},
    {"/drive",              handleGetDrive},
    {"/account",            handleGetAccount},
    {"/api/download",       handleGetDownload},
    {"/inbox",              handleGetInbox},
    {"/view_email",         handleGetViewEmail},
    {"/compose",            handleGetCompose},
    {"/admin",              handleGetAdmin},
    {"/logout",             handleGetLogout}
};

const unordered_map<string, RouteHandler> postRoutes = {
    {"/api/login",              handlePostLogin},
    {"/api/change_password",    handlePostChangePassword},
    {"/api/signup",             handlePostSignup},
    {"/api/send_email",         handlePostSendEmail},
    {"/api/upload",             handlePostUpload},
    {"/api/delete",             handlePostDelete},
    {"/api/admin/start",        handleAdminStartNode},
    {"/api/admin/stop",         handleAdminStopNode}
};


void handleClient(int client_sock) {
    // Stop waiting if a client sends no data for 5 seconds
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    char buffer[8192];
    string rawRequest = "";

    // Keep reading until we receive all HTTP headers
    while (rawRequest.find("\r\n\r\n") == string::npos) {
        int bytesRead = recv(client_sock, buffer, sizeof(buffer), 0);

        if (bytesRead <= 0) {
            close(client_sock);
            return;
        }

        rawRequest.append(buffer, bytesRead);

        // Prevent excessively large request headers
        if (rawRequest.size() > 16384) {
            close(client_sock);
            return;
        }
    }
    size_t headerEnd = rawRequest.find("\r\n\r\n");
    HttpRequest req;
    req.headers = rawRequest.substr(0, headerEnd);
    req.body = rawRequest.substr(headerEnd + 4);

    // Extract session ID from the Cookie header
    size_t cookiePos = req.headers.find("session_id=");
    if (cookiePos != string::npos) {
        size_t start = cookiePos + 11;
        size_t end = req.headers.find_first_of(";\r\n", start);
        req.sessionId = req.headers.substr(start, end - start);
    }

    // Extract Method and Path
    stringstream hdrStream(req.headers);
    string version;
    hdrStream >> req.method >> req.path >> version;

    // Check for query parameters (e.g., /?error=invalid)
    size_t qMark = req.path.find('?');
    if (qMark != string::npos) {
        req.query = req.path.substr(qMark + 1);
        req.path = req.path.substr(0, qMark);
    }

    // Read and validate request body
    if (req.method == "POST") {
        const size_t MAX_BODY_SIZE = 32 * 1024 * 1024; // 32MB
        size_t contentLength = 0;
        bool validLength = false;
        bool tooLarge = false;

        size_t clPos = req.headers.find("Content-Length: ");

        if (clPos != string::npos) {
            size_t start = clPos + 16;
            size_t end = req.headers.find("\r\n", start);
            string lengthStr = req.headers.substr(start, end - start);

            // Check that Content-Length contains only digits
            validLength = !lengthStr.empty();

            for (char c : lengthStr) {
                if (c < '0' || c > '9') {
                    validLength = false;
                    break;
                }

                size_t digit = c - '0';

                // Avoid overflow and reject oversized bodies
                if (contentLength > (MAX_BODY_SIZE - digit) / 10) {
                    tooLarge = true;
                    break;
                }

                contentLength = contentLength * 10 + digit;
            }
        }

        string errorStatus = "";

        if (tooLarge) {
            errorStatus = "413 Payload Too Large";
        } else if (!validLength || clPos == string::npos) {
            errorStatus = "400 Bad Request";
        } else {
            // Keep reading until the full POST body arrives
            while (req.body.size() < contentLength) {
                char temp[4096];
                size_t remaining = contentLength - req.body.size();
                size_t toRead = min(remaining, sizeof(temp));

                int bytes = recv(client_sock, temp, toRead, 0);

                if (bytes <= 0) {
                    errorStatus = "400 Bad Request";
                    break;
                }

                req.body.append(temp, bytes);
            }
        }

        // Reject invalid or incomplete requests
        if (!errorStatus.empty()) {
            string response = "HTTP/1.1 " + errorStatus + "\r\n"
                              "Content-Length: 0\r\n"
                              "Connection: close\r\n\r\n";

            size_t bytes_sent = 0;
            while(bytes_sent < response.length()) {
                ssize_t n = send(client_sock, response.c_str() + bytes_sent, response.length() - bytes_sent, 0);
                if (n <= 0) break; 
                bytes_sent += n;
            }
            close(client_sock);
            return;
        }

        // Use only the bytes specified by Content-Length
        req.body.resize(contentLength);

        // Parse form parameters for standard URL-encoded POST submissions
        if (req.headers.find("multipart/form-data") == string::npos) {
            req.formParams = parseFormData(req.body);
        }
    }

    // route dispatch
    HttpResponse res;
    bool handled = false;

    if(req.method == "GET") {
        auto it = getRoutes.find(req.path);
        if (it != getRoutes.end()) {
            it->second(req, res);
            handled = true;
        }
    }
    else if(req.method == "POST") {
        auto it = postRoutes.find(req.path);
        if (it != postRoutes.end()) {
            it->second(req, res);
            handled = true;
        }
    }

    if(!handled) {
        res.status = "404 Not Found";
        res.body = errorPage("404 Not Found", "The requested endpoint does not exist.");
    }


    // Construct and Send Raw HTTP Response
    string response = "HTTP/1.1 " + res.status + "\r\n" +
                      "Content-Type: " + res.contentType + "\r\n" +
                      "Content-Length: " + to_string(res.body.length()) + "\r\n" +
                      res.extraHeaders +
                      "Connection: close\r\n\r\n" +
                      res.body;

    send(client_sock, response.c_str(), response.length(), 0);
    close(client_sock);
}

