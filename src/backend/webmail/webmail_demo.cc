#include "webmail.h"
#include <iostream>
#include <string>
#include <vector>
using namespace std;

// Small terminal program for trying webmail without the website.
// argv[1] is the command (send, inbox, or read)
int main(int argc, char* argv[]) {
    // Show the available commands when too few arguments are given.
    if (argc < 3) {
        cerr << "Usage:\n  ./webmail_demo send <sender> <recipient> <subject> <body>\n"
                "  ./webmail_demo inbox <username>\n  ./webmail_demo read <username> <email_id>\n";
        return 1;
    }
    // Use the webmail functions to communicate with the KV servers.
    webmail::Webmail mail;
    string operation = argv[1];
    // Send an email using sender, recipient, subject, and body.
    // The function fills "id" with the new email ID on success.
    if (operation == "send" && argc == 6) {
        string id;
        if (!mail.send_email(argv[2], argv[3], argv[4], argv[5], id)) {
            cerr << "Email send failed (check recipient exists and KV nodes are running)\n";
            return 1;
        }
        cout << "Sent email ID: " << id << '\n';
    // List messages in one user's inbox, newest first.
    // Print each message's ID so it can be used with the read command.
    } else if (operation == "inbox" && argc == 3) {
        vector<webmail::Email> emails;
        if (!mail.get_inbox(argv[2], emails)) {
            cerr << "Could not load inbox\n";
            return 1;
        }
        for (const auto& e : emails) {
            cout << e.id << " | " << e.sender << " | " << e.timestamp << " | " << e.subject << '\n';
        }
        cout << "Total emails: " << emails.size() << '\n';
    // Retrieve one email by its ID and print its header and body.
    } else if (operation == "read" && argc == 4) {
        webmail::Email e;
        if (!mail.get_email(argv[2], argv[3], e)) {
            cerr << "Email not found\n";
            return 1;
        }
        cout << "From: " << e.sender << "\nTo: " << e.recipient
             << "\nDate: " << e.timestamp << "\nSubject: " << e.subject
             << "\n\n" << e.body << '\n';
    } else {
        // The command name or number of arguments was incorrect.
        cerr << "Invalid arguments\n";
        return 1;
    }
    // Return 0 when the requested operation finishes successfully.
    return 0;
}
