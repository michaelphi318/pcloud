#include <iostream>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include "webmail.h"
#include "handlers.h"

// Your code here

using namespace std;

webmail::Webmail webmail_service({8001, 8002, 8003});

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
            thread(handleClient, client_sock).detach();
        }
    }

    close(server_sock);
    return 0;
}