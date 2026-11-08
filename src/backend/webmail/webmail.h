#ifndef PENNCLOUD_WEBMAIL_H
#define PENNCLOUD_WEBMAIL_H

#include <string>
#include <vector>

namespace webmail {

struct Email {
    std::string id;
    std::string sender;
    std::string recipient;
    std::string subject;
    std::string timestamp;
    std::string body;
};

// These methods talk to the existing KV storage servers, not the HTML frontend.
// The KV nodes must be running on the supplied ports (defaults: 8001-8003).
class Webmail {
public:
    explicit Webmail(std::vector<int> ports = {8001, 8002, 8003});

    // Save an email for an existing PennCloud recipient.
    bool send_email(const std::string& sender, const std::string& recipient,
                    const std::string& subject, const std::string& body,
                    std::string& email_id);

    // List inbox messages newest first. Returns false on backend errors.
    bool get_inbox(const std::string& username, std::vector<Email>& emails);

    // Read one email, scoped to the recipient's inbox.
    bool get_email(const std::string& username, const std::string& email_id,
                   Email& email);

private:
    std::vector<int> ports_;
    bool kv_command(const std::string& command, std::string& response) const;
    bool kv_get(const std::string& row, const std::string& column,
                std::string& value, bool& exists) const;
};

} // namespace webmail
#endif
