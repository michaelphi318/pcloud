#include <iostream>
#include <string>
#include <map>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <netinet/in.h>
#include <semaphore.h>
#include <pthread.h>
#include <sstream>
#include <sys/file.h>
#include <sys/stat.h>
#include <filesystem>

typedef struct {
    std::string ipPrimary;
    int portPrimary;
    std::vector<std::string> ipsSecondary;
    std::vector<int> portsSecondary;
} ReplicaGroup;

bool loadConfig(const std::string& filename);
std::map<std::string, ReplicaGroup> clusterTopology;

class KVStore;

void * worker(void * arg);
void runServer(KVStore& kvStore, int portNumber);
void parse(int argc, char *argv[], int * portNumber, std::string& configFile);

bool forwardRequest(const std::string& targetIP, int targetPort, const std::string& request, int client_fd);
void replicateToSecondary(const std::string& ip, int port, const std::string& request);

bool debug = false;


/**
 * class for KVStore
 * implements:
 *  1. PUT(r,c,v): Stores a value v in column c of row r.
 *  2. GET(r,c): Returns the value stored in column c of row r.
 *  3. CPUT(r,c,v1,v2): Stores value v2 in column c of row r, but only if the current value is v1.
 *  4. DELETE(r,c): Deletes the value in column c of row r.
 */
class KVStore {
    private:
        std::string storageDir;

        /**
         * deserialize from file into an unordered_map
         */
        std::unordered_map<std::string, std::vector<uint8_t>> deserializeRow(FILE * cfptr) const {
            std::unordered_map<std::string, std::vector<uint8_t>> rowMap;
            fseek(cfptr, 0, SEEK_SET);

            size_t colLen;

            // read until end of file
            while(fread(&colLen, sizeof(size_t), 1, cfptr) == 1) {

                // read into colName
                std::string colName(colLen, '\0');
                fread(&colName[0], sizeof(char), colLen, cfptr);

                size_t valLen;
                fread(&valLen, sizeof(size_t), 1, cfptr);
                
                // read into val
                std::vector<uint8_t> val(valLen);
                if(valLen > 0) fread(val.data(), sizeof(uint8_t), valLen, cfptr);

                // insert into map
                rowMap[colName] = val;
            }
            return rowMap;
        }

        /**
         * serialize unordered_map into chunk file
         */
        void serializeRow(FILE * cfptr, const std::unordered_map<std::string, std::vector<uint8_t>>& rowMap) {

            // loop through each col val pair in the map
            for(auto const& pair : rowMap) {
                std::string columnName = pair.first;
                std::vector<uint8_t> columnValue = pair.second;

                // save col name
                // write how many characters in col name, write the characters of the name one by one
                size_t nameLength = columnName.length();
                fwrite(&nameLength, sizeof(size_t), 1, cfptr);
                for(char c : columnName) fwrite(&c, sizeof(char), 1, cfptr);

                // write how many bytes in vector value, write the bytes one by one
                size_t valueLength = columnValue.size();
                fwrite(&valueLength, sizeof(size_t), 1, cfptr);
                for(uint8_t byte : columnValue) fwrite(&byte, sizeof(uint8_t), 1, cfptr);
            }

            // save into kernel buffer to prevent [application crash -> lose data]
            fflush(cfptr);
        }

    public:

        /**
         * class constructor
         * creates node's isolated directory
         * creates directory with read/write/search permissions
         */
        KVStore(int port) {
            storageDir = "data_" + std::to_string(port) + "/";
            mkdir(storageDir.c_str(), 0777);
        }

        /**
         * PUT(r,c,v): Stores a value v in column c of row r
         * if r or c are nullptr, return false
         * since put function wants to write, we dont want other threads to execute put, del, cput, get
         * so we block all other threads from reading/writing until put function unlocks
         */
        bool put(const char * r, const char * c, const std::vector<uint8_t>& v) {

            if(debug) printf("DEBUG - KVStore::put executing for row '%s', col '%s'\n", r, c);

            if(r == nullptr || c == nullptr) return false;

            std::string filepath = storageDir + r + ".chunk";

            FILE * cfptr = fopen(filepath.c_str(), "r+");
            if(cfptr == NULL) cfptr = fopen(filepath.c_str(), "w+");
            if(cfptr == NULL) return false;

            int fd = fileno(cfptr);

            // exclusive write lock
            if(flock(fd, LOCK_EX) == -1) {
                fclose(cfptr);
                return false;
            }

            // deserialize into unordered_map
            auto rowMap = deserializeRow(cfptr);

            // update the specific column with the new value
            rowMap[c] = v;

            // empty the file and seek to beginning before overwriting
            ftruncate(fd, 0);
            fseek(cfptr, 0, SEEK_SET);

            // serialize updated data back to disk
            serializeRow(cfptr, rowMap);
            
            close(fd);
            fclose(cfptr); 
            return true;
        }

        /**
         * GET(r,c): Returns the value stored in column c of row r
         * if r or c are nullptr, return empty vector {}
         * since get function wants to read the file, we block all writers but allow all other readers to read as well
         * multiple threads can have read lock concurrently
         */
        std::vector<uint8_t> get(const char * r, const char * c) const {

            if(debug) printf("DEBUG - KVStore::get executing for row '%s', col '%s'\n", r, c);

            if(r == nullptr || c == nullptr) return {};
            
            std::string filepath = storageDir + r + ".chunk";

            // open for read/write
            FILE * cfptr = fopen(filepath.c_str(), "r+");
            if(cfptr == NULL) return {};

            int fd = fileno(cfptr);
            // shared read lock (allows concurrent GET, blocks PUT/CPUT/DEL)
            if(flock(fd, LOCK_SH) == -1) {
                close(fd);
                return {};
            }

            std::vector<uint8_t> result;

            // deserialize into unordered_map
            auto rowMap = deserializeRow(cfptr);
            
            // search for the requested column
            auto colIterator = rowMap.find(c);
            if(colIterator != rowMap.end()) {
                // if found, assign it to result
                result = colIterator->second;
            }

            close(fd);
            fclose(cfptr);
            return result;
        }

        /**
         * CPUT(r,c,v1,v2): Stores value v2 in column c of row r, but only if the current value is v1
         * if r or c are nullptr, return false
         * since cput function wants to write to the file, we dont want other threads to execute put, del, cput, get
         * so we block all other threads from reading/writing until cput function unlocks 
         * we check if stored value at r,c is v1
         * if it is v1, then we can replace it with v2
         * if row or column does not exist, and v1 is empty, then we can safely insert v2 into the map
         */
        bool cput(const char * r, const char * c, const std::vector<uint8_t>& v1, const std::vector<uint8_t>& v2) {

            if(debug) printf("DEBUG - KVStore::cput executing for row '%s', col '%s'\n", r, c);

            if(r == nullptr || c == nullptr) return false;

            std::string filepath = storageDir + r + ".chunk";

            // open for read/write
            FILE * cfptr = fopen(filepath.c_str(), "r+");

            // if file doesnt exist only create it if v1 is empty
            if(cfptr == NULL) {
                if (v1.empty()) cfptr = fopen(filepath.c_str(), "w+");
                if (cfptr == NULL) return false;
            }

            int fd = fileno(cfptr);

            // exclusive write lock
            if(flock(fd, LOCK_EX) == -1) {
                fclose(cfptr);
                return false;
            }

            // deserialize into unordered_map
            auto rowMap = deserializeRow(cfptr);
            bool conditionMet = false;

            auto colIterator = rowMap.find(c);
            if(colIterator != rowMap.end()) {
                // col exists, check if it matches v1
                if(colIterator->second == v1) {
                    rowMap[c] = v2;
                    conditionMet = true;
                }
            }
            else {
                // col does not exist, insert if v1 is empty
                if(v1.empty()) {
                    rowMap[c] = v2;
                    conditionMet = true;
                }
            }

            if(conditionMet) {
                // empty the file and seek to beginning
                ftruncate(fd, 0);
                fseek(cfptr, 0, SEEK_SET);
                
                // serialize updated data
                serializeRow(cfptr, rowMap);
            }

            close(fd);
            fclose(cfptr);
            return conditionMet;
        }

        /**
         * DELETE(r,c): Deletes the value in column c of row r
         * if r or c are nullptr, return false
         * since del function wants to write to the hashmap, we dont want other threads to execute put, del, cput, get
         * so we block all other threads from reading/writing until del function unlocks 
         * if there is a value in r,c then we remove the column entry
         * if as a result of removing the column entry, the row becomes empty, we remove the row entry as well
         */
        bool del(const char * r, const char * c) {

            if(debug) printf("DEBUG - KVStore::del executing for row '%s', col '%s'\n", r, c);

            if(r == nullptr || c == nullptr) return false;

            std::string filepath = storageDir + r + ".chunk";

            // if file does not exist, nothing to delete
            FILE * cfptr = fopen(filepath.c_str(), "r+");
            if(cfptr == NULL) return false;

            int fd = fileno(cfptr);

            // exclusive write lock
            if(flock(fd, LOCK_EX) == -1) {
                fclose(cfptr);
                return false;
            }

            // deserialize into unordered_map

            auto rowMap = deserializeRow(cfptr);
            bool success = false;

            auto colIterator = rowMap.find(c);
            if(colIterator != rowMap.end()) {
                rowMap.erase(colIterator);
                success = true;

                // empty the file and seek to beginning
                ftruncate(fd, 0);
                fseek(cfptr, 0, SEEK_SET);
                
                if(rowMap.empty()) {
                    // if row is now empty, close and delete the physical file
                    close(fd);
                    fclose(cfptr);
                    unlink(filepath.c_str());
                    return true;
                }
                else {
                    // serialize remaining data
                    serializeRow(cfptr, rowMap);
                }
            }
            close(fd);
            fclose(cfptr);
            return success;
        }
};

/**
 * class for passing information to threads
 * primarily KVStore reference and client file descripto
 */
class ThreadData {
    private:
        KVStore * kvStore;
        int fd;
        int serverPort;

    public:
        ThreadData(KVStore * kvStore, int fd, int serverPort) {
            this->kvStore = kvStore;
            this->fd = fd;
            this->serverPort = serverPort;
        }

        KVStore * getKVStore() {
            return this->kvStore;
        }

        int getFd() {
            return this->fd;
        }

        int getServerPort() {
            return this->serverPort;
        }
};


int main(int argc, char * argv[]) {
    int portNumber = 8080;
    std::string configFile = "nodes.conf";
    parse(argc, argv, &portNumber, configFile);
    KVStore kvStore(portNumber);
    
    if(!loadConfig(configFile)) {
        fprintf(stderr, "Could not load cluster configuration from\n");
        return 1;
    }

    runServer(kvStore, portNumber);
    return 0;
}

/**
 * Parsing program arguments using getopt()
 */
void parse(int argc, char *argv[], int * portNumber, std::string& configFile) {
    int c;
    while((c = getopt(argc, argv, "p:c:v")) != -1) {
        switch(c) {
            case 'p':
                // update the portNumber from the default to the value inputted
                *portNumber= strtol(optarg, NULL, 10);
                if(*portNumber < 0) {
                    fprintf(stderr, "Option -%c requires a non-negative integer.\n", c);
                    exit(1);
                }
                break;
            
            case 'c':
                configFile = optarg;
                break;
            
            case 'v':
                debug = true;
                break;

            // documentation for getopt on sourceware
            case '?':
                if(optopt == 'p' || optopt == 'c') fprintf(stderr, "Option -%c requires an integer.\n", optopt);
                else fprintf (stderr, "Unknown option `-%c'.\n", optopt);
                exit(1);
            
            default:
            abort();
        }
    }
}

/**
 * opens socket to target storage node, forward exact command, send respond back to the original client
 */
bool forwardRequest(const std::string& targetIP, int targetPort, const std::string& request, int client_fd) {
    int proxy_fd = socket(PF_INET, SOCK_STREAM, 0);
    if (proxy_fd < 0) return false;

    struct sockaddr_in targetServaddr;
    bzero(&targetServaddr, sizeof(targetServaddr));
    targetServaddr.sin_family = AF_INET;
    targetServaddr.sin_port = htons(targetPort);
    inet_pton(AF_INET, targetIP.c_str(), &targetServaddr.sin_addr);

    if (connect(proxy_fd, (struct sockaddr *) &targetServaddr, sizeof(targetServaddr)) < 0) {
        close(proxy_fd);
        return false;
    }

    char buf[8192];
    std::string stream_buffer = "";
    size_t crlfPos = std::string::npos;

    // read target server's greeting until \r\n
    while ((crlfPos = stream_buffer.find("\r\n")) == std::string::npos) {
        ssize_t n = read(proxy_fd, buf, sizeof(buf));
        if (n <= 0) { close(proxy_fd); return false; }
        stream_buffer.append(buf, n);
    }
    // discard greeting
    stream_buffer.erase(0, crlfPos + 2); 

    // send the full request (guarantee all bytes are written)
    size_t sent = 0;
    while (sent < request.length()) {
        ssize_t n = write(proxy_fd, request.c_str() + sent, request.length() - sent);
        if (n <= 0) { close(proxy_fd); return false; }
        sent += n;
    }

    // read target server's response until \r\n
    while ((crlfPos = stream_buffer.find("\r\n")) == std::string::npos) {
        ssize_t n = read(proxy_fd, buf, sizeof(buf));
        // server closed connection prematurely
        if (n <= 0) break; 
        stream_buffer.append(buf, n);
    }

    // relay completed response back to client
    if (crlfPos != std::string::npos) {
        std::string full_resp = stream_buffer.substr(0, crlfPos + 2);
        sent = 0;
        while (sent < full_resp.length()) {
            ssize_t n = write(client_fd, full_resp.c_str() + sent, full_resp.length() - sent);
            if (n <= 0) break;
            sent += n;
        }
    }
    
    close(proxy_fd);
    return true;
}

void replicateToSecondary(const std::string& ip, int port, const std::string& request) {
    int sock = socket(PF_INET, SOCK_STREAM, 0);
    if(sock < 0) return;

    struct sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    // if the secondary is offline, we ignore it    
    if(connect(sock, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        close(sock);
        return;
    }

    char buf[1024];
    // drop the "+OK KVStorage ready" greeting
    read(sock, buf, sizeof(buf));

    // send the replication command
    std::string cmd = request + "\r\n";
    size_t sent = 0;
    while(sent < cmd.length()) {
        ssize_t n = write(sock, cmd.c_str() + sent, cmd.length() - sent);
        if(n <= 0) break;
        sent += n;
    }
    close(sock);
}

/**
 * thread worker function
 */
void * worker(void * arg) {
    
    // convert argument back to ThreadData and extract the vars
    ThreadData * td = (ThreadData *) arg;
    int client_fd = td->getFd();
    KVStore * kvStore = td->getKVStore();
    int serverPort = td->getServerPort();

    // free the memory allocated in main
    delete td;

    auto safeWrite = [&](const std::string& msg) {
        size_t sent = 0;
        while (sent < msg.length()) {
            ssize_t n = write(client_fd, msg.c_str() + sent, msg.length() - sent);
            if (n <= 0) break;
            sent += n;
        }
    };

    // 128kb
    char buffer[131072];
    std::string streamBuffer = "";

    while(true) {
        size_t crlfPos = streamBuffer.find("\r\n");

        // keep reading from the socket until a complete command arrives
        while (crlfPos == std::string::npos) {
            ssize_t bytesRead = read(client_fd, buffer, sizeof(buffer) - 1);
            
            if (bytesRead <= 0) {
                close(client_fd);
                return nullptr; 
            }

            // guarantee null termination
            buffer[bytesRead] = '\0';
            
            streamBuffer.append(buffer);
            crlfPos = streamBuffer.find("\r\n");

            if (streamBuffer.size() > 131072) {
                std::string response = "-ERR Command too long\r\n";
                safeWrite(response);
                close(client_fd);
                return nullptr;
            }
        }

        // convert buffer into a c++ string (extract request)
        std::string request = streamBuffer.substr(0, crlfPos);
        streamBuffer.erase(0, crlfPos + 2); 

        std::istringstream scanner(request);

        std::string command;
        std::string row;
        std::string col;
        
        // extract only command first
        if(scanner >> command) {
            
            // admin and coordinator commands
            
            if(strcasecmp(command.c_str(), "SHUTDOWN") == 0) {
                if(debug) printf("DEBUG - Received SHUTDOWN command. Exiting...\n");
                safeWrite("+OK Shutting down\r\n");
                close(client_fd);
                // immediately kills this backend process
                exit(0); 
            }

            else if(strcasecmp(command.c_str(), "DUMP") == 0) {
                std::string resp = "+OK\n";
                char lineBuf[256];
                std::string storageDir = "data_" + std::to_string(serverPort) + "/";
                
                if(std::filesystem::exists(storageDir)) {
                    for(const auto& entry : std::filesystem::directory_iterator(storageDir)) {
                        std::string filename = entry.path().filename().string();
                        
                        // only read chunk files
                        if(filename.length() > 6 && filename.substr(filename.length() - 6) == ".chunk") {
                            std::string row = filename.substr(0, filename.length() - 6);
                            snprintf(lineBuf, sizeof(lineBuf), "ROW: %s\n", row.c_str());
                            resp += lineBuf;
                            
                            // use GET to fetch column names
                            FILE * cfptr = fopen(entry.path().c_str(), "r");
                            if(cfptr) {
                                fseek(cfptr, 0, SEEK_SET);
                                size_t colLen;
                                while(fread(&colLen, sizeof(size_t), 1, cfptr) == 1) {
                                    std::string colName(colLen, '\0');
                                    fread(&colName[0], sizeof(char), colLen, cfptr);
                                    
                                    size_t valLen;
                                    fread(&valLen, sizeof(size_t), 1, cfptr);
                                    
                                    std::string valStr;
                                    if(valLen > 0) {
                                        valStr.resize(valLen);
                                        fread(&valStr[0], 1, valLen, cfptr);
                                    }

                                    // clean raw binary for html display
                                    std::string displayVal = valStr;
                                    if(displayVal.length() > 50) displayVal = displayVal.substr(0, 47) + "...";
                                    for (char& c : displayVal) {
                                        // replace non-printable chars
                                        if(c < 32 || c > 126) c = '.';
                                    }
                                    
                                    snprintf(lineBuf, sizeof(lineBuf), "  COL: %s | VAL: %s (%zu bytes)\n", colName.c_str(), displayVal.c_str(), valLen);
                                    resp += lineBuf;
                                }
                                fclose(cfptr);
                            }
                        }
                    }
                }
                safeWrite(resp);
            }

            else if(strcasecmp(command.c_str(), "UPDATE_GROUP") == 0) {
                // coordinator sent us a new topology because a primary died
                std::string keySecondary;
                std::string ipPrimary;
                std::string ipSecondary1;
                std::string ipSecondary2;
                int portPrimary;
                int portSecondary1;
                int portSecondary2;
                
                if (scanner >> keySecondary >> ipPrimary >> portPrimary >> ipSecondary1 >> portSecondary1 >> ipSecondary2 >> portSecondary2) {
                    ReplicaGroup rg;
                    rg.ipPrimary = ipPrimary; rg.portPrimary = portPrimary;
                    rg.ipsSecondary = {ipSecondary1, ipSecondary2}; rg.portsSecondary = {portSecondary1, portSecondary2};
                    
                    clusterTopology[keySecondary] = rg;
                    
                    if(debug) printf("DEBUG - Failover registered! Group '%s' new Primary is %d\n", keySecondary.c_str(), portPrimary);
                    safeWrite("+OK Topology Updated\r\n");
                } 
                else safeWrite("-ERR Invalid Update\r\n");
            }


            // standard kv commands

            else if(scanner >> row >> col) {
                // detect if this is an internal replication command from the primary
                bool isReplicaRequest = false;
                if(strcasecmp(command.c_str(), "REPLICATE") == 0) {
                    isReplicaRequest = true;
                    // shift the command
                    command = row;
                    // shift the row
                    row = col;
                    // read the actual column from the scanner
                    scanner >> col;
                }

                if(debug) printf("DEBUG - Worker parsed command: '%s', row: '%s', col: '%s'\n", command.c_str(), row.c_str(), col.c_str());

                // clusterTopology is empty means error in loading, continue
                if(clusterTopology.empty()) {
                    std::string response = "-ERR Cluster topology not loaded\r\n";
                    safeWrite(response);
                    continue;
                }

                // find which node owns this row - using upper_bound to find first key greater than the row, then doing --
                auto nodeIterator = clusterTopology.upper_bound(row);
                if(nodeIterator != clusterTopology.begin()) nodeIterator--;
                ReplicaGroup targetGroup = nodeIterator->second;

                if(debug) printf("DEBUG - Row '%s' mapped to Group Primary %s:%d (My Port: %d)\n", row.c_str(), targetGroup.ipPrimary.c_str(), targetGroup.portPrimary, serverPort);

                bool isPrimary = (targetGroup.portPrimary == serverPort);

                // if targetNode is not us, and we are not a secondary receiving a replica request, forward the request
                if(!isPrimary && !isReplicaRequest) {
                    std::string forwardReq = request + "\r\n";
                    if(!forwardRequest(targetGroup.ipPrimary, targetGroup.portPrimary, forwardReq, client_fd)) {
                        std::string response = "-ERR Node Unavailable\r\n";
                        safeWrite(response);
                    }
                    continue;
                }

                if(strcasecmp(command.c_str(), "PUT") == 0) {

                    std::string strVal;
                    // skip leading whitespace then extract value
                    scanner >> std::ws;
                    std::getline(scanner, strVal);
                    
                    // remove trailing \r if present
                    if(!strVal.empty() && strVal.back() == '\r') strVal.pop_back();

                    std::vector<uint8_t> val(strVal.begin(), strVal.end());

                    // execute command on KVStore
                    bool success = kvStore->put(row.c_str(), col.c_str(), val);

                    // mirror successful write to all secondaries
                    if(isPrimary && success) {
                        std::string replicateCmd = "REPLICATE " + request;
                        for(int i = 0; i < targetGroup.portsSecondary.size(); i++) {
                            replicateToSecondary(targetGroup.ipsSecondary[i], targetGroup.portsSecondary[i], replicateCmd);
                        }
                    }

                    // respond to server
                    std::string response = success ? "+OK\r\n" : "-ERR PUT Failed\r\n";
                    safeWrite(response);
                }

                else if(strcasecmp(command.c_str(), "GET") == 0) {

                    // get value with given row col
                    std::vector<uint8_t> val = kvStore->get(row.c_str(), col.c_str());

                    // if val is empty, respond with error
                    if(val.empty()) {
                        std::string response = "-ERR Not Found\r\n";
                        safeWrite(response);
                    }
                    else {
                        std::string strVal(val.begin(), val.end());
                        std::string response = "+OK " + strVal + "\r\n";
                        safeWrite(response);
                    }
                }

                else if(strcasecmp(command.c_str(), "CPUT") == 0) {
                    std::string strV1;
                    std::string strV2;

                    // extract v1, skip leading whitespace then extract v2
                    scanner >> strV1;
                    scanner >> std::ws;
                    std::getline(scanner, strV2);

                    // remove trailing \r if present
                    if(!strV2.empty() && strV2.back() == '\r') strV2.pop_back();

                    std::vector<uint8_t> v1;
                    std::vector<uint8_t> v2;

                    // check if v1 is empty using placeholder
                    if(strV1 != "[EMPTY]") v1.assign(strV1.begin(), strV1.end());
                    v2.assign(strV2.begin(), strV2.end());

                    bool success = kvStore->cput(row.c_str(), col.c_str(), v1, v2);

                    // mirror successful write to all secondaries
                    if(isPrimary && success) {
                        std::string replicateCmd = "REPLICATE " + request;
                        for(int i = 0; i < targetGroup.portsSecondary.size(); i++) {
                            replicateToSecondary(targetGroup.ipsSecondary[i], targetGroup.portsSecondary[i], replicateCmd);
                        }
                    }

                    std::string response = success ? "+OK\r\n" : "-ERR CPUT Condition Failed\r\n";
                    safeWrite(response);
                }

                else if(strcasecmp(command.c_str(), "DELETE") == 0) {
                    // execute command, then respond
                    bool success = kvStore->del(row.c_str(), col.c_str());

                    // mirror successful write to all secondaries
                    if(isPrimary && success) {
                        std::string replicateCmd = "REPLICATE " + request;
                        for(int i = 0; i < targetGroup.portsSecondary.size(); i++) {
                            replicateToSecondary(targetGroup.ipsSecondary[i], targetGroup.portsSecondary[i], replicateCmd);
                        }
                    }

                    std::string response = success ? "+OK\r\n" : "-ERR Not Found\r\n";
                    safeWrite(response);
                }

                else {
                    std::string response = "-ERR Unknown Command\r\n";
                    safeWrite(response);
                }
            }
            else {
                std::string response = "-ERR Malformed Request\r\n";
                safeWrite(response);
            }
        }
        else {
            std::string response = "-ERR Malformed Request\r\n";
            safeWrite(response);
        }
    }
    
    close(client_fd);
    return nullptr;
}

/**
 * create socket
 * connect to client
 */
void runServer(KVStore& kvStore, int portNumber) {
    int server_fd = socket(PF_INET, SOCK_STREAM, 0);
    
    struct sockaddr_in servaddr;
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(portNumber);
    servaddr.sin_addr.s_addr = htons(INADDR_ANY);

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    int success = bind(server_fd, (struct sockaddr *) &servaddr, sizeof(servaddr));

    if(success != 0) {
        fprintf(stderr, "Bind failed\n");
        exit(1);
    }

    listen(server_fd, 100);
    printf("KV Server listening on port %d...\n", portNumber);
    
    while(true) {
        struct sockaddr_in clientaddr;
        socklen_t clientaddrlen = sizeof(clientaddr);
        int client_fd = accept(server_fd, (struct sockaddr *) &clientaddr, &clientaddrlen);

        if(debug) printf("DEBUG - Accepted new connection (FD: %d) on port %d\n", client_fd, portNumber);

        char serverOk[] = "+OK KVStorage ready\r\n";
        write(client_fd, serverOk, strlen(serverOk));

        // pass multiple variables to thread using ThreadData
        ThreadData * td = new ThreadData(&kvStore, client_fd, portNumber);
        
        pthread_t thread;
        pthread_create(&thread, NULL, worker, td);
        pthread_detach(thread);
    }
    
    close(server_fd);
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
            
            if(debug) printf("DEBUG - Loaded Group '%s': Primary %d, Secs %d, %d\n", startKey.c_str(), portPrimary, portSecondary1, portSecondary2);
        }
    }
    fclose(cfptr);
    return true;
}

