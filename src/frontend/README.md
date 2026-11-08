# PennCloud Frontend Web Server

## Overview
The C++ frontend for the CIS 5050 PennCloud project. This component operates as a custom HTTP web server built entirely using raw POSIX sockets, strictly adhering to project constraints by avoiding external HTTP frameworks (such as `cpp-httplib`). It serves the HTML/CSS user interface, parses form submissions, and functions as the intermediary bridge to the Key-Value (KV) storage cluster and the Webmail service.

## Program Execution Flow
The system processes incoming web traffic through a highly concurrent, multi-threaded pipeline:

1. **Connection Acceptance:** The main process in `frontend.cc` binds to port 8000. Upon accepting an incoming TCP connection, it immediately spawns a detached `std::thread` to manage the client, allowing the main loop to resume listening.
2. **HTTP Parsing:** The detached thread executes `handleClient()` (located in `handlers.cc`). This function reads the raw socket stream byte-by-byte to securely extract HTTP headers. For `POST` requests, it parses the `Content-Length` header and continues reading until the exact payload body size is received, preventing TCP fragmentation issues.
3. **Route Dispatching:** The parsed `HttpRequest` object is matched against static dispatch tables (`getRoutes` and `postRoutes`). The request is then forwarded to the corresponding handler (e.g., `handlePostUpload`).
4. **Backend Communication:** Handlers requiring persistent data utilize the `kvClient.cc` module. This module determines the correct backend node via `nodes.conf`, establishes a TCP socket, completes the cluster handshake, and issues custom text protocol commands (`PUT`, `GET`, `CPUT`, `DELETE`).
5. **Response Generation:** The handler reads static HTML template files from disk, injects dynamic variables (such as active user sessions or file lists) using utilities in `utils.cc`, and writes a fully formed `200 OK` or `302 Found` HTTP response back to the client socket.

## Core Modules & Key Functions

The codebase is organized into specific domain modules:

* **`frontend.cc`** 
  * Main entry point and TCP `accept()` loop. 

* **`modules/route-handlers/handlers.cc`**
  * `handleClient(int client_sock)`: The primary HTTP parsing state machine.
  * Contains individual endpoint functions (e.g., `handleGetDashboard`, `handlePostSendEmail`) responsible for session validation and HTML rendering.

* **`modules/storage-client/kvClient.cc`**
  * `sendCommandKV(string cmd)`: Opens a short-lived TCP connection to a backend node, navigates the `+OK KVStorage ready` handshake, and returns the string response.
  * `uploadFileChunks()` / `downloadFileChunks()`: Splits large binary files into 1KB chunks. It encodes each chunk into Base64 to bypass text-protocol null-byte termination constraints, then orchestrates sequential `PUT` or `GET` operations.

* **`modules/utilities/utils.cc`**
  * `parseMultipartFile()`: Scans complex `multipart/form-data` request bodies to accurately isolate raw binary payloads and filenames from surrounding browser boundaries.
  * `findAndReplaceTemplate()`: Performs in-place string injection for HTML rendering.

## Compilation and Execution

The backend KV nodes must be running (typically on ports 8001-8003) prior to starting the frontend.

1. Build the server binary:
   ```bash
   make
   ```
2. Start the web server:
   ```bash
   ./frontend_server
   ```
3. Access the interface via a web browser:
   ```text
   http://localhost:8000
   ```

## Testing Guidelines

* **Authentication & Concurrency:** Execute the `/signup` route. The system utilizes `CPUT` to guarantee atomic account creation and prevent race conditions if duplicate usernames are submitted simultaneously. Logins with incorrect credentials should trigger dynamic error HTML injection.
* **Drive Uploads:** Upload a non-text binary file (e.g., `.png` or `.pdf`) via the `/drive` interface. Verification requires checking the frontend terminal logs to ensure the multipart form was parsed, and checking the backend node storage directories to confirm Base64 chunk files were successfully written to disk.
* **File Management:** Execute file deletion and renaming actions to verify the `__file_index__` tracker string correctly updates and persists in the KV store without corrupting existing entries.

## Future Enhancements (TODOs)

* **Session Expiry:** Sessions currently lack an expiration mechanism. A timestamp or TTL (Time-To-Live) flag must be added to the session tokens in the KV store to support automatic logout for inactive users.
* **Upload Limits:** The multipart parser currently restricts `POST` bodies to 16MB. Verification is required to ensure this threshold aligns with deployment environment memory constraints.
* **Static Asset Routing:** If CSS is eventually migrated out of the HTML `<style>` tags into distinct `.css` files, the routing logic must be updated to serve appropriate `text/css` MIME types.