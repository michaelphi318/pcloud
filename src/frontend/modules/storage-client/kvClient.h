#ifndef KV_CLIENT_H
#define KV_CLIENT_H

#include <string>

// return the first responding KV node port from nodes.conf
int getTargetPort(const std::string& cmd);

// send all bytes even when TCP performs a partial write
bool socketSendAll(int sock, const std::string& data);

// read one KV protocol line, excluding its CRLF terminator
bool socketReadLine(int sock, std::string& line);

// send command to KV node on target port
std::string sendCommandKV(const std::string& cmd);

// get session user
std::string getSessionUser(const std::string& headers);

// resolve session_id to the authenticated PennCloud username
bool getLoggedInUser(const std::string& session_id, std::string& username);

// upload file to KV storage
bool uploadFileChunks(const std::string& user, const std::string& filename, const std::string& file_data);

// download file from KV storage
bool downloadFileChunks(const std::string& user, const std::string& filename, std::string& out_data);

#endif