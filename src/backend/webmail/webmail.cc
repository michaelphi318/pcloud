#include "webmail.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cctype>
#include <ctime>
#include <sstream>

using namespace std;

namespace webmail {
namespace {

// Keep each command short enough for the KV server to read in one request.
const size_t MAX_COMMAND = 1800; // The KV server reads up to 2047 bytes per command.

// Check names before using them as KV row or column keys.
// Spaces and special characters would break the KV command format.
bool valid_name(const string& name) {
    if (name.empty() || name.size() > 64) return false;
    for (unsigned char c : name)
        if (!isalnum(c) && c != '_' && c != '-') return false;
    return true;
}

// Encode email fields before sending them to KV storage.
// The KV protocol separates values using spaces, so raw email text cannot be stored safely.
string encode(const string& text) {
    const string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    string result;
    unsigned int bits = 0;
    int count = -6;
    for (unsigned char c : text) {
        bits = (bits << 8) | c;
        count += 8;
        while (count >= 0) {
            result += chars[(bits >> count) & 63];
            count -= 6;
        }
    }
    if (count > -6) result += chars[((bits << 8) >> (count + 8)) & 63];
    return result;
}

// Turn encoded KV data back into normal text when reading an email.
// Return false if the stored text contains an invalid character.
bool decode(const string& text, string& result) {
    result.clear();
    const string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    unsigned int bits = 0;
    int count = -8;
    for (char c : text) {
        size_t value = chars.find(c);
        if (value == string::npos) return false;
        bits = (bits << 6) | static_cast<unsigned int>(value);
        count += 6;
        if (count >= 0) {
            result += static_cast<char>((bits >> count) & 255);
            count -= 8;
        }
    }
    return true;
}

// Put the email fields into one string for KV storage.
// Dots separate the encoded fields, including the previous email ID.
string pack_email(const Email& email, const string& previous) {
    return encode(email.sender) + "." + encode(email.subject) + "." +
           encode(email.timestamp) + "." + encode(email.body) + "." + encode(previous);
}

// Split a saved message into its fields and decode each one.
// The last field points to the next older email in the inbox.
bool unpack_email(const string& value, Email& email, string& previous) {
    vector<string> fields;
    stringstream stream(value);
    string field;
    while (getline(stream, field, '.')) fields.push_back(field);
    // getline drops a final empty field, so account for an empty previous ID.
    if (!value.empty() && value.back() == '.') fields.push_back("");
    return fields.size() == 5 && decode(fields[0], email.sender) &&
           decode(fields[1], email.subject) && decode(fields[2], email.timestamp) &&
           decode(fields[3], email.body) && decode(fields[4], previous);
}

// Read the KV server response until its ending \r\n.
// Return false if the connection closes before the full response arrives.
bool read_line(int sock, string& line) {
    line.clear();
    char c;
    while (line.size() < 4096) {
        if (recv(sock, &c, 1, 0) <= 0) return false;
        line += c;
        if (line.size() >= 2 && line.substr(line.size() - 2) == "\r\n") {
            line.resize(line.size() - 2);
            return true;
        }
    }
    return false;
}

// Keep sending until the complete KV command has gone over the socket.
// One send() call is not guaranteed to transmit every byte.
bool send_all(int sock, const string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = send(sock, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

// Record the time an email was sent, using UTC for consistent inbox dates.
string timestamp() {
    time_t current = time(nullptr);
    tm utc{};
    gmtime_r(&current, &utc);
    char buffer[32];
    strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

// Make a message ID from the current time and a local counter.
// The ID is used as the column name when saving and reading the message.
string new_email_id() {
    // A timestamp plus a counter keeps IDs distinct for this simple local demo.
    static unsigned long long counter = 0;
    auto micros = chrono::duration_cast<chrono::microseconds>(
        chrono::system_clock::now().time_since_epoch()).count();
    return to_string(micros) + "_" + to_string(counter++);
}

}

// Save the KV server ports that webmail will try to connect to.
Webmail::Webmail(vector<int> ports) : ports_(ports) {}

// Open a TCP connection to a KV node and send one command.
// Try the next port if connecting fails. Close the socket after the response.
// A response error is returned to the caller rather than blindly retrying a write.
bool Webmail::kv_command(const string& command, string& response) const {
    if (command.size() > MAX_COMMAND) return false;
    for (int port : ports_) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) continue;
        timeval timeout{2, 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close(sock);
            continue;
        }
        string hello;
        bool ok = read_line(sock, hello) && hello == "+OK KVStorage ready" &&
                  send_all(sock, command + "\r\n") && read_line(sock, response);
        close(sock);
        return ok;
    }
    return false;
}

// Read one KV cell. A missing cell is not the same as a network error:
// exists=false means the request worked but the key was not found.
bool Webmail::kv_get(const string& row, const string& column,
                     string& value, bool& exists) const {
    string response;
    if (!kv_command("GET " + row + " " + column, response)) return false;
    if (response == "-ERR Not Found") {
        exists = false;
        value.clear();
        return true;
    }
    if (response.rfind("+OK ", 0) != 0) return false;
    exists = true;
    value = response.substr(4);
    return true;
}

// Send an email to an existing PennCloud user.
// First check the recipient account, then save the message in that user's
// mailbox row and update the inbox index so it can be found later.
bool Webmail::send_email(const string& sender, const string& recipient,
                         const string& subject, const string& body, string& email_id) {
    email_id.clear();
    if (!valid_name(sender) || !valid_name(recipient) || subject.empty()) return false;
    string password;
    bool exists = false;
    if (!kv_get("users", recipient, password, exists) || !exists) return false;

    Email email;
    email.id = new_email_id();
    email.sender = sender;
    email.recipient = recipient;
    email.subject = subject;
    email.body = body;
    email.timestamp = timestamp();
    string row = "Mail_" + recipient;

    // Each mailbox has a "head" cell containing its newest email ID.
    // The saved message points to the old head. CPUT updates the head only
    // if it has not changed, so we can retry when another write got there first.
    for (int i = 0; i < 20; ++i) {
        string head;
        bool has_head = false;
        if (!kv_get(row, "head", head, has_head)) return false;
        string command = "PUT " + row + " msg_" + email.id + " " +
                         pack_email(email, has_head ? head : "");
        if (command.size() > MAX_COMMAND) return false;
        string response;
        if (!kv_command(command, response) || response != "+OK") return false;
        if (!kv_command("CPUT " + row + " head " +
                        (has_head ? head : "[EMPTY]") + " " + email.id, response)) return false;
        if (response == "+OK") {
            email_id = email.id;
            return true;
        }
        if (response != "-ERR CPUT Condition Failed") return false;
    }
    return false;
}

// Load a specific email from this user's mailbox row.
// Decode the saved fields and fill in the message ID and recipient.
bool Webmail::get_email(const string& username, const string& email_id, Email& email) {
    if (!valid_name(username) || !valid_name(email_id)) return false;
    string value, previous;
    bool exists = false;
    if (!kv_get("Mail_" + username, "msg_" + email_id, value, exists) || !exists) return false;
    if (!unpack_email(value, email, previous)) return false;
    email.id = email_id;
    email.recipient = username;
    return true;
}

// Build the inbox starting at the newest message ID ("head").
// Each saved message tells us which older email to load next.
// Stop at the end of the list or return false if stored data is missing.
bool Webmail::get_inbox(const string& username, vector<Email>& emails) {
    emails.clear();
    if (!valid_name(username)) return false;
    string current;
    bool exists = false;
    string row = "Mail_" + username;
    if (!kv_get(row, "head", current, exists)) return false;
    if (!exists) return true;
    for (int i = 0; i < 10000 && !current.empty(); ++i) {
        if (!valid_name(current)) return false;
        string value, previous;
        bool found = false;
        if (!kv_get(row, "msg_" + current, value, found) || !found) return false;
        Email email;
        if (!unpack_email(value, email, previous)) return false;
        email.id = current;
        email.recipient = username;
        emails.push_back(email);
        current = previous;
    }
    return current.empty();
}

}
