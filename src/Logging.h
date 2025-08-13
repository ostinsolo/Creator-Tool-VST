#pragma once
#include <juce_core/juce_core.h>
#include <fstream>
#include <ctime>
#include <chrono>
#include <memory>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <iostream>

// Asynchronous logger class
class AsyncLogger {
private:
    std::queue<std::string> logQueue_;
    std::timed_mutex queueMutex_;
    std::condition_variable_any cv_;
    std::atomic<bool> shouldStop_{false};
    std::thread logThread_;
    std::unique_ptr<std::ofstream> logFile_;
    std::string logFilePath_;
    std::atomic<bool> loggerHealthy_{true};
    
    // Private constructor for singleton
    AsyncLogger() {
        initializeLogFile();
        logThread_ = std::thread(&AsyncLogger::logWorker, this);
    }
    
    void initializeLogFile() {
        try {
            auto desktopDir = juce::File::getSpecialLocation(juce::File::userDesktopDirectory);
            auto logDir = desktopDir.getChildFile("CreatorTool_Logs");
            logDir.createDirectory();
            
            time_t now = time(nullptr);
            char* timeStr = ctime(&now);
            juce::String timestamp(timeStr);
            timestamp = timestamp.trim().replaceCharacters(" :", "__");
            
            auto logPath = logDir.getChildFile("CreatorTool_" + timestamp + ".log");
            logFilePath_ = logPath.getFullPathName().toStdString();
            
            logFile_ = std::make_unique<std::ofstream>(logFilePath_, std::ios::app);
            if (logFile_->is_open()) {
                *logFile_ << "=== Creator Tool Async Log Started at " << timeStr << " ===" << std::endl;
                logFile_->flush();
                loggerHealthy_.store(true);
            } else {
                loggerHealthy_.store(false);
                std::cout << "[LOGGER ERROR] Failed to open log file: " << logFilePath_ << std::endl;
            }
        } catch (const std::exception& e) {
            loggerHealthy_.store(false);
            std::cout << "[LOGGER ERROR] Exception in initializeLogFile: " << e.what() << std::endl;
        } catch (...) {
            loggerHealthy_.store(false);
            std::cout << "[LOGGER ERROR] Unknown exception in initializeLogFile" << std::endl;
        }
    }
    
    void logWorker() {
        try {
            while (!shouldStop_.load()) {
                std::unique_lock<std::timed_mutex> lock(queueMutex_);
                cv_.wait(lock, [this] { return !logQueue_.empty() || shouldStop_.load(); });
                while (!logQueue_.empty()) {
                    std::string message = logQueue_.front();
                    logQueue_.pop();
                    lock.unlock();
                    if (logFile_ && logFile_->is_open()) {
                        try {
                            time_t now = time(nullptr);
                            char* timeStr = ctime(&now);
                            std::string timestamp(timeStr);
                            if (!timestamp.empty() && timestamp.back() == '\n') {
                                timestamp.pop_back();
                            }
                            *logFile_ << "[" << timestamp << "] " << message << std::endl;
                            static int flushCounter = 0;
                            if (++flushCounter % 3 == 0) {
                                logFile_->flush();
                            }
                        } catch (const std::exception& e) {
                            std::cout << "[LOGGER ERROR] File write exception: " << e.what() << std::endl;
                            loggerHealthy_.store(false);
                        } catch (...) {
                            std::cout << "[LOGGER ERROR] Unknown file write exception" << std::endl;
                            loggerHealthy_.store(false);
                        }
                    }
                    lock.lock();
                }
            }
            if (logFile_ && logFile_->is_open()) {
                try {
                    logFile_->flush();
                    std::cout << "[LOGGER] Background thread shutting down gracefully" << std::endl;
                } catch (...) {
                    std::cout << "[LOGGER ERROR] Exception during final flush" << std::endl;
                }
            }
        } catch (const std::exception& e) {
            std::cout << "[LOGGER FATAL] Background thread exception: " << e.what() << std::endl;
            loggerHealthy_.store(false);
        } catch (...) {
            std::cout << "[LOGGER FATAL] Unknown background thread exception" << std::endl;
            loggerHealthy_.store(false);
        }
    }
    
public:
    static AsyncLogger& getInstance() {
        static AsyncLogger instance;
        return instance;
    }
    
    bool isHealthy() const {
        return loggerHealthy_.load();
    }
    
    void log(const std::string& message) {
        if (!loggerHealthy_.load()) {
            std::cout << "[FALLBACK LOG] " << message << std::endl;
            return;
        }
        std::unique_lock<std::timed_mutex> lock(queueMutex_, std::defer_lock);
        if (lock.try_lock_for(std::chrono::milliseconds(2))) {
            logQueue_.push(message);
            cv_.notify_one();
        } else {
           #ifdef JUCE_DEBUG
            std::cout << "[SKIPPED LOG] " << message << std::endl;
           #endif
        }
    }
    
    void logBlocking(const std::string& message, int timeoutMs = 10) {
        if (!loggerHealthy_.load()) {
            std::cout << "[FALLBACK LOG] " << message << std::endl;
            return;
        }
        std::unique_lock<std::timed_mutex> lock(queueMutex_, std::defer_lock);
        if (lock.try_lock_for(std::chrono::milliseconds(timeoutMs))) {
            logQueue_.push(message);
            cv_.notify_one();
        } else {
            std::cout << "[TIMEOUT LOG] " << message << std::endl;
        }
    }
    
    ~AsyncLogger() {
        shouldStop_.store(true);
        cv_.notify_all();
        if (logThread_.joinable()) {
            logThread_.join();
        }
    }
    
    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;
    AsyncLogger(AsyncLogger&&) = delete;
    AsyncLogger& operator=(AsyncLogger&&) = delete;
};

inline void LogMessage(const juce::String& message) {
    DBG(message);
    AsyncLogger::getInstance().logBlocking(message.toStdString(), 5);
}

inline void LogMessageFromAudioThread(const juce::String& message) {
    DBG(message);
    AsyncLogger::getInstance().log(message.toStdString());
}

inline void LogMessageBlocking(const juce::String& message) {
    DBG(message);
    AsyncLogger::getInstance().logBlocking(message.toStdString());
}
