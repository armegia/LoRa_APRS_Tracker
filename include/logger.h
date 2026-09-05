#ifndef TRACKER_LOGGER_H_
#define TRACKER_LOGGER_H_

#include <Arduino.h>
#include <WiFiUdp.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace logging {

class LoggerLevel {
public:
    enum Value : uint8_t {
        LOGGER_LEVEL_ERROR = 3,
        LOGGER_LEVEL_WARN  = 4,
        LOGGER_LEVEL_INFO  = 6,
        LOGGER_LEVEL_DEBUG = 7,
    };

    LoggerLevel() = default;
    constexpr LoggerLevel(Value value) : value_(value) {}

    constexpr bool operator<=(LoggerLevel other) const {
        return value_ <= other.value_;
    }

    const char* toString() const;
    const char* getLineColor() const;
    constexpr Value getValue() const {
        return value_;
    }

private:
    Value value_ = LOGGER_LEVEL_DEBUG;
};

// Formats each record before taking the mutex, then sends the complete record
// to Serial in one write. This prevents FreeRTOS tasks from interleaving the
// prefix, message, and newline of separate log records.
class Logger {
public:
    Logger();
    explicit Logger(LoggerLevel level);
    explicit Logger(Stream* serial);
    Logger(Stream* serial, LoggerLevel level);
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void begin();
    void setSerial(Stream* serial);
    void setDebugLevel(LoggerLevel level);

    void setSyslogServer(const String& server, unsigned int port, const String& hostname);
    void setSyslogServer(IPAddress ip, unsigned int port, const String& hostname);

    void log(LoggerLevel level, const String& module, const char* format, ...)
        __attribute__((format(printf, 4, 5)));

private:
    Stream* serial_;
    LoggerLevel level_;
    SemaphoreHandle_t mutex_;
    bool ready_;

    bool syslogSet_;
    WiFiUDP syslogUdp_;
    String syslogServer_;
    IPAddress syslogIp_;
    int syslogPort_;
    String syslogHostname_;

    void lock();
    void unlock();
    void writeRecord(LoggerLevel level, const String& module, const char* message);
    void syslogLog(LoggerLevel level, const String& module, const char* message);
};

} // namespace logging

#endif
