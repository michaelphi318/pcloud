# PennCloud Webmail (Demo I)

This C++ module sends emails between PennCloud users and stores them in the KV server.
It supports sending an email, listing an inbox, and reading a message.

## Build

Run `make` inside `src/backend/webmail` while the KV servers are available on ports 8001, 8002 and 8003.

## Terminal test

```sh
./webmail_demo send alice bob "Hello" "Hi Bob"
./webmail_demo inbox bob
./webmail_demo read bob EMAIL_ID
```

Both users must already have accounts. Use the email ID shown by `inbox` for `read`.

## C++ interface

Include `webmail.h` and link `webmail.cc` to the calling C++ program.

```cpp
webmail::Webmail mail;
std::string id;
mail.send_email("alice", "bob", "Hello", "Hi Bob", id);
std::vector<webmail::Email> messages;
mail.get_inbox("bob", messages);
webmail::Email message;
mail.get_email("bob", id, message);
```

Each user's messages are stored in `Mail_<username>`. The `head` entry points to the newest email, and each `msg_<id>` entry stores the email plus the previous email ID. Text is encoded so it can be sent through the KV text protocol.

Messages must fit within the KV command size limit (about 1,800 bytes after encoding)