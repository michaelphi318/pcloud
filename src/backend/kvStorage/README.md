# Distributed KVStorage System Documentation

---

## Table of Contents
1. [System Architecture Overview](#system-architecture-overview)
2. [Compilation & Building](#compilation--building)
3. [Cluster Configuration (`nodes.conf`)](#cluster-configuration-nodesconf)
4. [Running the Storage Nodes](#running-the-storage-nodes)
5. [Text-Based Protocol Specification](#text-based-protocol-specification)
   - [Initial Connection Greeting](#initial-connection-greeting)
   - [PUT](#1-put)
   - [GET](#2-get)
   - [CPUT (Conditional Put)](#3-cput-conditional-put)
   - [DELETE](#4-delete)
   - [Error Responses](#error-responses)
6. [Automated Integration Testing](#automated-integration-testing)
7. [Data Persistence & Clean Up](#data-persistence--clean-up)

---

## System Architecture Overview

The system is a multi-threaded, persistent distributed key-value store:
- **Data Model:** Two-dimensional key space indexed by `(row, column) -> value`.
- **Partitioning:** Lexicographical partitioning using node `StartKey` definitions.
- **Proxy Routing:** Clients can connect to any live node in the cluster. If a node receives a request for a row outside its assigned key range, it automatically forwards the request via TCP to the responsible node and streams the response back to the client.
- **Concurrency & Storage:** Each row is persisted to an independent chunk file (`<row>.chunk`) using serialization with length as prefix. File locking via `flock` guarantees read write mutual exclusion:
  - `GET` holds a shared read lock (`LOCK_SH`).
  - `PUT`, `CPUT`, `DELETE` acquire exclusive write lock (`LOCK_EX`).

---

## Compilation & Building

To compile the node binary and tests:
```bash
make
```

To clean previous binaries, temporary files, and on-disk chunk databases:
```bash
make clean
```

---

## Cluster Configuration (`nodes.conf`)

The cluster topology is defined in a text configuration file (`nodes.conf`). Each line specifies a node's IP address, listening port, and the starting row key for its lexicographical range.

### Format
```text
# IP          Port    StartKey
127.0.0.1     8001    A
127.0.0.1     8002    M
127.0.0.1     8003    Z
```

### How Range Partitioning Works
- **Node 8001 (`A`)**: Responsible for any row key from `"A"` up to (but not including) `"M"` (e.g., `Apple`, `Banana`, `Cat`).
- **Node 8002 (`M`)**: Responsible for row keys from `"M"` up to (but not including) `"Z"` (e.g., `Mango`, `Orange`, `Watermelon`).
- **Node 8003 (`Z`)**: Responsible for row keys starting with `"Z"` and beyond (e.g., `Zebra`, `Zoo`).

### Customizing the Topology
- **Adding a Node:** Add an entry with the target IP, port, and key boundary.
- **Changing Boundaries:** Adjust the `StartKey` token. Make sure keys are ordered logically.
- **Comments & Formatting:** Lines starting with `#` and empty lines are automatically skipped. (`\n`) and (`\r\n`) line endings are supported.

---

## Running the Storage Nodes

Each node must be started as a standalone process.

### Command Line Arguments
```bash
./kvstore -p <port> -c <config_file> [-v]
```
- `-p <port>`: The TCP port this node will bind to (default: `8080`).
- `-c <path>`: Path to the cluster config file (default: `nodes.conf`).
- `-v`: *(Optional)* Verbose debug mode.

### Starting a 3-Node Cluster Locally
Open three terminal windows and run:

**Terminal 1 (Node 1):**
```bash
./kvstore -p 8001 -c nodes.conf -v
```

**Terminal 2 (Node 2):**
```bash
./kvstore -p 8002 -c nodes.conf -v
```

**Terminal 3 (Node 3):**
```bash
./kvstore -p 8003 -c nodes.conf -v
```

---

## Text-Based Protocol Specification

All communication occurs over TCP connections. Every client command must end with a carriage return and newline: `\r\n`.

### Initial Connection Greeting
Upon connecting, the node sends:
```text
+OK KVStorage ready\r\n
```

---

### 1. PUT
Stores or overwrites a value in column `col` of row `row`.

* **Syntax:**
  ```text
  PUT <row> <col> <value>\r\n
  ```
* **Success Response:**
  ```text
  +OK\r\n
  ```
* **Example:**
  ```text
  Client:  PUT Apple color red\r\n
  Server:  +OK\r\n
  ```

---

### 2. GET
Retrieves the binary/text value stored in column `col` of row `row`.

* **Syntax:**
  ```text
  GET <row> <col>\r\n
  ```
* **Success Response:**
  ```text
  +OK <value>\r\n
  ```
* **Failure Response (Row or column not present):**
  ```text
  -ERR Not Found\r\n
  ```
* **Examples:**
  ```text
  Client:  GET Apple color\r\n
  Server:  +OK red\r\n

  Client:  GET Apple shape\r\n
  Server:  -ERR Not Found\r\n
  ```

---

### 3. CPUT (Conditional Put)
Atomically sets column `col` in row `row` to `v2` only if its current value is equal to `v1`. If `v1` is specified as `[EMPTY]`, the operation succeeds only if the column does not currently exist.

* **Syntax:**
  ```text
  CPUT <row> <col> <v1> <v2>\r\n
  ```
* **Success Response:**
  ```text
  +OK\r\n
  ```
* **Failure Response (Condition mismatch):**
  ```text
  -ERR CPUT Condition Failed\r\n
  ```
* **Examples:**

  *Conditional Insert (only if not existing):*
  ```text
  Client:  CPUT Item inventory [EMPTY] 50\r\n
  Server:  +OK\r\n
  ```

  *Conditional Update with Mismatched Prior Value:*
  ```text
  Client:  CPUT Item inventory 30 75\r\n
  Server:  -ERR CPUT Condition Failed\r\n
  ```

  *Conditional Update with Matching Prior Value:*
  ```text
  Client:  CPUT Item inventory 50 75\r\n
  Server:  +OK\r\n
  ```

---

### 4. DELETE
Removes column `col` from row `row`. If the deleted column was the last remaining column in the row, the row chunk file is removed from disk.

* **Syntax:**
  ```text
  DELETE <row> <col>\r\n
  ```
* **Success Response:**
  ```text
  +OK\r\n
  ```
* **Failure Response (Column not found):**
  ```text
  -ERR Not Found\r\n
  ```
* **Example:**
  ```text
  Client:  DELETE Apple color\r\n
  Server:  +OK\r\n
  ```

---

### Error Responses

| Error String | Reason |
| :--- | :--- |
| `-ERR Not Found` | The specified row or column does not exist on `GET` or `DELETE`. |
| `-ERR CPUT Condition Failed` | The existing value does not match `v1` in a `CPUT` operation. |
| `-ERR Node Unavailable` | The row routes to a peer node that is offline or unreachable. |
| `-ERR Unknown Command` | Command verb is not recognized (allowed: `PUT`, `GET`, `CPUT`, `DELETE`). |
| `-ERR Malformed Request` | The line does not contain the required number of parameters. |
| `-ERR Cluster topology not loaded` | The server failed to read or locate its configuration file. |

---

## Interactive Client & Manual Testing

A manual test file is available to connect and test commands from your terminal.

### 1. Compile the Client
```bash
make
```

### 2. Connect to a Node
```bash
# Connects by default to 127.0.0.1:8001
./test

# Or specify a custom port / IP
./test 8002 127.0.0.1
```

### 3. Run Commands Provided in STDOUT
```bash
# For e.g.
PUT Apple flavor sweet
```

---

## Data Persistence & Clean Up

- Each node stores data inside its own port directory: `data_<port>/` (e.g., `data_8001/`).
- Each row is stored as `data_<port>/<row>.chunk`.
- When deleting all columns in a row, the node unlinks and deletes the `.chunk` file automatically.
- To completely reset storage between test sessions, run:
  ```bash
  make clean
  ```