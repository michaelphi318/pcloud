#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <stdbool.h>

int main(int argc, char *argv[]) {
    char * ip = "127.0.0.1";
    int port = 8001;

    if(argc >= 2) port = atoi(argv[1]);
    if(argc >= 3) ip = argv[2];

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if(sock < 0) {
        fprintf(stderr, "Socket creation failed\n");
        return 1;
    }

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(port);

    if(inet_pton(AF_INET, ip, &servaddr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid IP address\n");
        close(sock);
        return 1;
    }

    if(connect(sock, (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0) {
        fprintf(stderr, "Connection to node failed\n");
        close(sock);
        return 1;
    }

    printf("Connected to node at %s:%d\n", ip, port);

    // initial greeting from the server
    char buffer[2048];
    ssize_t bytes = recv(sock, buffer, sizeof(buffer) - 1, 0);
    if(bytes > 0) {
        buffer[bytes] = '\0';
        printf("%s", buffer);
    }

    printf("Type commands directly\n\n");
    
    printf("Test: PUT Apple flavor sweet\t\t\tExpect: +OK\n");
    printf("Test: GET Apple flavor\t\t\t\tExpect: +OK sweet\n\n");

    printf("Test: PUT Zebra color black_and_white\t\tExpect: +OK\n");
    printf("Test: GET Zebra color\t\t\t\tExpect: +OK black_and_white\n\n");

    printf("Test: PUT Mango price 5\t\t\t\tExpect: +OK\n");
    printf("Test: CPUT Mango price 4 10\t\t\tExpect: -ERR CPUT Condition Failed\n");
    printf("Test: CPUT Mango price 5 10\t\t\tExpect: +OK\n");
    printf("Test: GET Mango price\t\t\t\tExpect: +OK 10\n\n");

    printf("Test: CPUT NewItem count [EMPTY] 100\t\tExpect: +OK\n");
    printf("Test: GET NewItem count\t\t\t\tExpect: +OK 100\n\n");

    printf("Test: PUT Apple to_delete true\t\t\tExpect: +OK\n");
    printf("Test: DELETE Apple to_delete\t\t\tExpect: +OK\n");
    printf("Test: GET Apple to_delete\t\t\tExpect: -ERR Not Found\n\n");

    printf("Test: INVALIDCMD row col val\t\t\tExpect: -ERR Unknown Command\n");

    printf("quit\n");

    printf("\n\n");


    char inputLine[2048];
    while(true) {
        printf("kvstore> ");
        fflush(stdout);

        if(!fgets(inputLine, sizeof(inputLine), stdin)) break;

        // remove trailing \r or \n
        size_t len = strlen(inputLine);
        while(len > 0 && (inputLine[len - 1] == '\n' || inputLine[len - 1] == '\r')) inputLine[--len] = '\0';

        if(len == 0) continue;

        if(strcmp(inputLine, "quit") == 0 || strcmp(inputLine, "exit") == 0) break;

        // append proper \r\n protocol terminator
        char send_buf[2048 + 2];
        int written = snprintf(send_buf, sizeof(send_buf), "%s\r\n", inputLine);

        if(send(sock, send_buf, written, 0) < 0) {
            fprintf(stderr, "Failed to send command\n");
            break;
        }

        // rcv response
        bytes = recv(sock, buffer, sizeof(buffer) - 1, 0);
        if(bytes <= 0) {
            printf("Server closed connection or connection lost\n");
            break;
        }

        buffer[bytes] = '\0';
        printf("%s", buffer);
    }

    close(sock);
    printf("Connection closed\n");
    return 0;
}