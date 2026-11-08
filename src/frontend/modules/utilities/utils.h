#ifndef UTILS_H
#define UTILS_H

#include <string>
#include <map>
#include <vector>

struct UploadedFile {
    std::string filename;
    std::string file_data;
};

// read html file from disk into a C++ string and return the string
std::string readHTMLfile(const std::string& filepath);

// in-place find-and-replace to substitute all occurrences of a template tag (e.g. "{{USERNAME}}") inside an HTML string
void findAndReplaceTemplate(std::string& html, const std::string& placeholder, const std::string& data);

// escape untrusted values before inserting them into HTML text or attributes
std::string escapeHTML(const std::string& input);

// KV command tokens must not contain whitespace or command separators
bool validTokenKV(const std::string& value, size_t max_length = 128);

// return whether uesrname is valid
bool validUsername(const std::string& username);

// translates a percent-encoded URL string back into raw text
std::string decodePercentURL(std::string str);

// parses a URL-encoded form body or query string into a map
std::map<std::string, std::string> parseFormData(const std::string& body);

// parses an HTTP multipart/form-data request body to locate, extract, and return an uploaded file's name and raw payload
UploadedFile parseMultipartFile(const std::string& body, const std::string& content_type);

// encode raw binary or text bytes into Base64-encoded ASCII string
std::string base64Encode(const std::string& in);

// decodes Base64-encoded ASCII string back into its original raw binary or text bytes
std::string base64Decode(const std::string& in);

// generates HTML error document to display error messages to the user
std::string errorPage(const std::string& title, const std::string& message);

// any node or route handler can securely append a timestamped string to a shared text file
void writeClusterLog(const std::string& level, const std::string& node, const std::string& message);

#endif