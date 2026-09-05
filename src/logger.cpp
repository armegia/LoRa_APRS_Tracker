#include "logger.h"

#include <cstdarg>
#include <cstdio>
#include <memory>
#include <new>

#include <freertos/task.h>

namespace {

} // namespace

namespace logging {

const char* LoggerLevel::toString() const {
    switch (value_) {
        case LOGGER_LEVEL_ERROR: return "ERROR";
        case LOGGER_LEVEL_WARN:  return "WARN";
        case LOGGER_LEVEL_INFO:  return "INFO";
        case LOGGER_LEVEL_DEBUG: return "DEBUG";
        default:                 return "UNKNOWN";
    }
}

const char* LoggerLevel::getLineColor() const {
    return "";
}

Logger::Logger()
    : Logger(&Serial, LoggerLevel::LOGGER_LEVEL_DEBUG) {}

Logger::Logger(LoggerLevel level)
    : Logger(&Serial, level) {}

Logger::Logger(Stream* serial)
    : Logger(serial, LoggerLevel::LOGGER_LEVEL_DEBUG) {}

Logger::Logger(Stream* serial, LoggerLevel level)
    : serial_(serial),
      level_(level),
      mutex_(xSemaphoreCreateMutex()),
      ready_(false),
      syslogSet_(false),
      syslogIp_(INADDR_NONE),
      syslogPort_(0) {}

Logger::~Logger() {
    if (mutex_ != nullptr) {
        vSemaphoreDelete(mutex_);
    }
}

void Logger::lock() {
    if (mutex_ != nullptr) {
        xSemaphoreTake(mutex_, portMAX_DELAY);
    }
}

void Logger::unlock() {
    if (mutex_ != nullptr) {
        xSemaphoreGive(mutex_);
    }
}

void Logger::begin() {
    lock();
    ready_ = true;
    unlock();
}

void Logger::setSerial(Stream* serial) {
    lock();
    serial_ = serial;
    unlock();
}

void Logger::setDebugLevel(LoggerLevel level) {
    lock();
    level_ = level;
    unlock();
}

void Logger::setSyslogServer(const String& server, unsigned int port, const String& hostname) {
    lock();
    syslogServer_ = server;
    syslogIp_ = IPAddress(INADDR_NONE);
    syslogPort_ = static_cast<int>(port);
    syslogHostname_ = hostname;
    syslogSet_ = true;
    unlock();
}

void Logger::setSyslogServer(IPAddress ip, unsigned int port, const String& hostname) {
    lock();
    syslogIp_ = ip;
    syslogServer_ = "";
    syslogPort_ = static_cast<int>(port);
    syslogHostname_ = hostname;
    syslogSet_ = true;
    unlock();
}

void Logger::log(LoggerLevel level, const String& module, const char* format, ...) {
    lock();
    const bool shouldFormat = (ready_ && level <= level_) || syslogSet_;
    unlock();
    if (!shouldFormat) {
        return;
    }

    va_list args;
    va_start(args, format);

    va_list sizeArgs;
    va_copy(sizeArgs, args);
    const int required = vsnprintf(nullptr, 0, format, sizeArgs);
    va_end(sizeArgs);

    if (required < 0) {
        va_end(args);
        writeRecord(LoggerLevel::LOGGER_LEVEL_ERROR, "Logger", "Failed to format log record");
        return;
    }

    std::unique_ptr<char[]> message(new (std::nothrow) char[static_cast<size_t>(required) + 1]);
    if (!message) {
        va_end(args);
        writeRecord(LoggerLevel::LOGGER_LEVEL_ERROR, "Logger", "Out of memory formatting log record");
        return;
    }

    va_list writeArgs;
    va_copy(writeArgs, args);
    vsnprintf(message.get(), static_cast<size_t>(required) + 1, format, writeArgs);
    va_end(writeArgs);
    va_end(args);

    writeRecord(level, module, message.get());
}

void Logger::writeRecord(LoggerLevel level, const String& module, const char* message) {
    const char* taskName = pcTaskGetName(nullptr);

    String record;
    record.reserve(module.length() + strlen(message) + 80);
    record += '[';
    record += level.toString();
    record += "][";
    record += module;
    record += "][";
    record += millis();
    record += "ms][";
    record += taskName != nullptr ? taskName : "no-task";
    record += "/C";
    record += xPortGetCoreID();
    record += "] ";
    record += message;
    record += "\r\n";

    lock();
    if (ready_ && level <= level_ && serial_ != nullptr) {
        serial_->write(reinterpret_cast<const uint8_t*>(record.c_str()), record.length());
    }
    if (syslogSet_) {
        syslogLog(level, module, message);
    }
    unlock();
}

void Logger::syslogLog(LoggerLevel level, const String& module, const char* message) {
    const int result = syslogIp_ == INADDR_NONE
        ? syslogUdp_.beginPacket(syslogServer_.c_str(), syslogPort_)
        : syslogUdp_.beginPacket(syslogIp_, syslogPort_);

    if (result != 1) {
        return;
    }

    constexpr int LOG_KERN = 0 << 3;
    syslogUdp_.print('<');
    syslogUdp_.print(LOG_KERN | level.getValue());
    syslogUdp_.print(">1 - ");
    syslogUdp_.print(syslogHostname_);
    syslogUdp_.print(' ');
    syslogUdp_.print(module);
    syslogUdp_.print(" - - - \xEF\xBB\xBF");
    syslogUdp_.print(message);
    syslogUdp_.endPacket();
}

} // namespace logging
