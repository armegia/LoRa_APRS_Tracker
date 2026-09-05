/* Copyright (C) 2026 Antonio Megia
 *
 * This file is part of LoRa APRS Tracker.
 *
 * LoRa APRS Tracker is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * LoRa APRS Tracker is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with LoRa APRS Tracker. If not, see <https://www.gnu.org/licenses/>.
 */

#include "logger.h"

#include <cstdarg>
#include <cstdio>

#include <freertos/task.h>

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

void Logger::setSerial(Stream* serial) {
    lock();
    serial_ = serial;
    unlock();
}

void Logger::setDebugLevel(LoggerLevel level) {
    lock();
    // Defends against an invalid persisted/cast value (e.g. Config.logLevel never having been
    // validated, such as when SPIFFS fails before Configuration's own validation runs) so a
    // garbage threshold can't silently suppress every log level, including ERROR.
    level_ = LoggerLevel::isValidValue(level.getValue())
        ? level
        : LoggerLevel(LoggerLevel::LOGGER_LEVEL_INFO);
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
    const bool shouldFormat = (level <= level_) || syslogSet_;
    unlock();
    if (!shouldFormat) {
        return;
    }

    // Fixed-size stack buffer instead of a per-call heap allocation: this runs on every RX/TX
    // DEBUG trace in the loop() hot path, and an embedded target would rather truncate an
    // unusually long message than churn the heap on every packet.
    constexpr size_t kMessageBufferSize = 256;
    char message[kMessageBufferSize];

    va_list args;
    va_start(args, format);
    const int required = vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    if (required < 0) {
        writeRecord(LoggerLevel::LOGGER_LEVEL_ERROR, "Logger", "Failed to format log record");
        return;
    }

    writeRecord(level, module, message);
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
    if (level <= level_ && serial_ != nullptr) {
        serial_->write(reinterpret_cast<const uint8_t*>(record.c_str()), record.length());
    }
    if (syslogSet_ && level <= level_) {
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
