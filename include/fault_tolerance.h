#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace penncloud {
    //sparse table
using Tablet = std::unordered_map<
    std::string, std::unordered_map<std::string, std::string>>;

class PersistenceManager {
public:
    using CheckpointErrorHandler = std::function<void(const std::string&)>;
    PersistenceManager(Tablet& tablet, std::mutex& tablet_mutex,
                       const std::string& data_directory);
    ~PersistenceManager();

    PersistenceManager(const PersistenceManager&) = delete;
    PersistenceManager& operator=(const PersistenceManager&) = delete;
    bool recover(std::string* error);

    bool put(const std::string& row, const std::string& col,
             const std::string& value, std::string* error);
    bool cput(const std::string& row, const std::string& col,
              const std::string& expected, const std::string& replacement,
              bool* swapped, std::string* error);

    bool erase(const std::string& row, const std::string& col,
               std::string* error);

    bool checkpoint(std::string* error);

    bool startPeriodicCheckpointing(
        std::chrono::milliseconds interval,
        CheckpointErrorHandler on_error,
        std::string* error);
    void stopPeriodicCheckpointing();

    uint64_t lastSequence() const;

private:
    bool ensureRecovered(std::string* error) const;
    void checkpointLoop(std::chrono::milliseconds interval);

    Tablet& tablet_;
    std::mutex& tablet_mutex;
    std::string data_directory;
    std::string checkpoint_path;

    class WALLogger;
    WALLogger* wal_;

    uint64_t last_sequence_ = 0; // tablet_mutex lock this
    bool recovered_ = false;    

    mutable std::mutex checkpoint_control_mutex;
    std::condition_variable checkpoint_cv;
    bool stop_checkpoint_thread = false;
    std::thread checkpoint_thread;
    CheckpointErrorHandler on_checkpoint_error;
};

} // namespace penncloud
