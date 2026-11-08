#include <iostream>
#include <string>
#include <map>
#include <vector>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sstream>
#include <fstream>
#include <string.h>
#include <chrono>
#include <iomanip>

typedef struct {
    std::string ipPrimary;
    int portPrimary;
    std::vector<std::string> ipsSecondary;
    std::vector<int> portsSecondary;
} ReplicaGroup;

std::map<std::string, ReplicaGroup> clusterTopology;

bool loadConfig(const std::string& filename);
bool pingNode(const std::string& ip, int port);
void pushTopologyUpdate(const std::string& ip, int port, const std::string& startKey, const ReplicaGroup& rg);

void * heartbeatMonitor(void * arg);
void * handleClient(void * arg);

void writeClusterLog(const std::string& level, const std::string& node, const std::string& message);

// listens on 9000
int main(int argc, char * argv[]) {
    if(!loadConfig("nodes.conf")) {
        fprintf(stderr, "Failed to load nodes.conf\n");
        return 1;
    }

    pthread_t threadHeartbeat;
    pthread_create(&threadHeartbeat, NULL, heartbeatMonitor, NULL);
    pthread_detach(threadHeartbeat);

    int server_fd = socket(PF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9000);
    addr.sin_addr.s_addr = htons(INADDR_ANY);

    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_fd, 50);

    printf("Backend Coordinator listening on port 9000...\n");

    while(true) {
        int client_fd = accept(server_fd, NULL, NULL);
        if(client_fd >= 0) {

            int * fdPtr = new int(client_fd);

            pthread_t threadHandleClient;
            pthread_create(&threadHandleClient, NULL, handleClient, fdPtr);
            pthread_detach(threadHandleClient);
        }
    }
    return 0;
}

/**
 * parses cluster configuration file and populates the topology map
 */
bool loadConfig(const std::string& filename) {
    FILE * cfptr = fopen(filename.c_str(), "r");

    if(cfptr == NULL) return false;

    // 128kb
    char buffer[131072];

    // fgets reads max sizeof(buffer)-1 chars or until \n

    while(fgets(buffer, sizeof(buffer), cfptr)) {

        std::string line(buffer);

        // fgets includes \n, so remove it for clean parsing
        if(!line.empty() && line.back() == '\n') line.pop_back();
        if(!line.empty() && line.back() == '\r') line.pop_back();

        // skip empty lines/comments
        if(line.empty() || line[0] == '#') continue;

        std::istringstream scanner(line);
        std::string startKey;
        std::string ipPrimary;
        std::string ipSecondary1;
        std::string ipSecondary2;
        int portPrimary;
        int portSecondary1;
        int portSecondary2;

        if(scanner >> startKey >> ipPrimary >> portPrimary >> ipSecondary1 >> portSecondary1 >> ipSecondary2 >> portSecondary2) {
            ReplicaGroup rg;
            rg.ipPrimary = ipPrimary;
            rg.portPrimary = portPrimary;
            rg.ipsSecondary = {ipSecondary1, ipSecondary2};
            rg.portsSecondary = {portSecondary1, portSecondary2};
            clusterTopology[startKey] = rg;
            
            printf("Loaded Group '%s' -> Primary: %d\n", startKey.c_str(), portPrimary);
        }
    }
    fclose(cfptr);
    return true;
}

/**
 * helper func to ping node (check if TCP connection is accepted)
 */
bool pingNode(const std::string& ip, int port) {
    int sock = socket(PF_INET, SOCK_STREAM, 0);
    if(sock < 0) return false;

    struct sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    bool alive = (connect(sock, (struct sockaddr *) &addr, sizeof(addr)) == 0);
    close(sock);
    return alive;
}

/**
 * push new topology to a backend node so it knows it was promoted
 */
void pushTopologyUpdate(const std::string& ip, int port, const std::string& startKey, const ReplicaGroup& rg) {
    int sock = socket(PF_INET, SOCK_STREAM, 0);
    if(sock < 0) return;

    struct sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    if(connect(sock, (struct sockaddr *) &addr, sizeof(addr)) == 0) {
        char buf[1024];

        // drop greeting +OK
        read(sock, buf, sizeof(buf));

        char cmdBuf[1024];
        snprintf(cmdBuf, sizeof(cmdBuf), "UPDATE_GROUP %s %s %d %s %d %s %d\r\n", 
                startKey.c_str(), 
                rg.ipPrimary.c_str(), rg.portPrimary, 
                rg.ipsSecondary[0].c_str(), rg.portsSecondary[0], 
                rg.ipsSecondary[1].c_str(), rg.portsSecondary[1]);
        
        write(sock, cmdBuf, strlen(cmdBuf));
    }
    close(sock);
}

/**
 * worker thread that monitors primary nodes and executes failovers
 */
void * heartbeatMonitor(void * arg) {
    while(true) {
        // check every 3 seconds
        sleep(3);

        for(auto& pair : clusterTopology) {
            std::string startKey = pair.first;
            ReplicaGroup& rg = pair.second;

            if(!pingNode(rg.ipPrimary, rg.portPrimary)) {
                writeClusterLog("ERROR", "Coordinator", "Primary " + std::to_string(rg.portPrimary) + " heartbeat timeout. Initiating failover.");
                printf("[WARNING] Primary %d is DOWN for partition '%s'. Initiating Failover...\n", rg.portPrimary, startKey.c_str());
                
                // find an alive secondary to promote
                for(int i = 0; i < rg.portsSecondary.size(); i++) {
                    if(pingNode(rg.ipsSecondary[i], rg.portsSecondary[i])) {
                        
                        // swap dead primary with alive secondary
                        std::string oldPrimaryIp = rg.ipPrimary;
                        int oldPrimaryPort = rg.portPrimary;

                        rg.ipPrimary = rg.ipsSecondary[i];
                        rg.portPrimary = rg.portsSecondary[i];

                        rg.ipsSecondary[i] = oldPrimaryIp;
                        rg.portsSecondary[i] = oldPrimaryPort;

                        printf("[SUCCESS] Promoted Node %d to Primary for partition '%s'.\n", rg.portPrimary, startKey.c_str());
                        writeClusterLog("INFO", "Coordinator", "Promoted Node " + std::to_string(rg.portPrimary) + " to Primary for partition '" + startKey + "'.");

                        // update new topology to all alive nodes in this group
                        pushTopologyUpdate(rg.ipPrimary, rg.portPrimary, startKey, rg);
                        pushTopologyUpdate(rg.ipsSecondary[0], rg.portsSecondary[0], startKey, rg);
                        pushTopologyUpdate(rg.ipsSecondary[1], rg.portsSecondary[1], startKey, rg);
                        
                        // failover complete
                        break;
                    }
                }
            }
        }
    }
}

/**
 * handles frontend LOOKUP requests
 */
void * handleClient(void * arg) {
    // extract the file descriptor
    int client_fd = *((int *) arg);
    delete (int *) arg;

    char buf[1024];
    ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
    if(n > 0) {
        buf[n] = '\0';
        std::istringstream scanner(buf);
        std::string command;
        std::string rowKey;

        scanner >> command;
        
        if(strcasecmp(command.c_str(), "LOOKUP") == 0) {
            scanner >> rowKey;
            auto iterator = clusterTopology.upper_bound(rowKey);
            if(iterator != clusterTopology.begin()) iterator--;
            
            ReplicaGroup& rg = iterator->second;
            std::string resp = "+OK " + rg.ipPrimary + " " + std::to_string(rg.portPrimary) + "\r\n";
            write(client_fd, resp.c_str(), resp.length());
        }

        else if(strcasecmp(command.c_str(), "STATUS") == 0) {
            // build a status report of the entire cluster
            std::string resp = "+OK\n";
            char lineBuf[256];
            
            for(auto& pair : clusterTopology) {
                std::string sKey = pair.first;
                ReplicaGroup& rg = pair.second;
                
                // Primary
                bool primaryAlive = pingNode(rg.ipPrimary, rg.portPrimary);
                snprintf(lineBuf, sizeof(lineBuf), "%s PRIMARY %s:%d %s\n", sKey.c_str(), rg.ipPrimary.c_str(), rg.portPrimary, primaryAlive ? "ALIVE" : "DEAD");
                resp += lineBuf;
                
                // Secondaries
                for(int i = 0; i < rg.portsSecondary.size(); i++) {
                    bool secondaryAlive = pingNode(rg.ipsSecondary[i], rg.portsSecondary[i]);
                    snprintf(lineBuf, sizeof(lineBuf), "%s SECONDARY %s:%d %s\n", sKey.c_str(), rg.ipsSecondary[i].c_str(), rg.portsSecondary[i], secondaryAlive ? "ALIVE" : "DEAD");
                    resp += lineBuf;
                }
            }
            write(client_fd, resp.c_str(), resp.length());
        }
    }
    close(client_fd);
    return nullptr;
}

/**
 * write clust log file in this folder
 */
void writeClusterLog(const std::string& level, const std::string& node, const std::string& message) {
    std::ofstream logfile("cluster.log", std::ios_base::app);
    if (!logfile.is_open()) return;

    auto now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    std::tm* local_tm = std::localtime(&now_c);

    logfile << "[" << std::put_time(local_tm, "%H:%M:%S") << "] " 
            << level << ": " << node << " " << message << std::endl;
}