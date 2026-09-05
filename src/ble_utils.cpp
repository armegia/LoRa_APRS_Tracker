/* Copyright (C) 2025 Ricardo Guzman - CA2RXU
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

#include <NimBLEDevice.h>
#include <atomic>
#include "configuration.h"
#include "lora_utils.h"
#include "kiss_utils.h"
#include "ble_utils.h"
#include "display.h"
#include "logger.h"

#define BLE_CHUNK_SIZE  512
#define MAX_KISS_BUFFER 1024
#define BLE_PAIRING_PIN_MIN 100000UL
#define BLE_PAIRING_PIN_RANGE 900000UL
#define NIMBLE_PASSKEY_CALLBACK_SENTINEL 123456UL
#define BLE_PAIRING_DISPLAY_TIMEOUT_MS 60000UL
#define BLE_PAIRING_DISPLAY_REFRESH_MS 1000UL


// APPLE - APRS.fi app
#define SERVICE_UUID_0            "00000001-ba2a-46c9-ae49-01b0961f68bb"
#define CHARACTERISTIC_UUID_TX_0  "00000003-ba2a-46c9-ae49-01b0961f68bb"
#define CHARACTERISTIC_UUID_RX_0  "00000002-ba2a-46c9-ae49-01b0961f68bb"

// ANDROID - BLE Terminal app (Serial Bluetooth Terminal from Playstore)
#define SERVICE_UUID_1            "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX_1  "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX_1  "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

BLEServer               *pServer;
BLECharacteristic       *pCharacteristicTx;
BLECharacteristic       *pCharacteristicRx;

extern Configuration    Config;
extern logging::Logger  logger;
extern bool             bluetoothConnected;
extern bool             bluetoothActive;
extern bool             displayState;
extern uint32_t         displayTime;

bool    shouldSendBLEtoLoRa     = false;
String  BLEToLoRaPacket         = "";
String  kissSerialBuffer        = "";

// NimBLE callbacks run in the host task. Only publish plain values from there;
// all display and String work stays in the Arduino loop task.
static std::atomic<uint32_t> pairingPasskey {BLE_PAIRING_PIN_MIN};
static std::atomic<uint32_t> pairingDisplayPasskey {0};

static uint32_t generatePairingPasskey() {
    return BLE_PAIRING_PIN_MIN + (esp_random() % BLE_PAIRING_PIN_RANGE);
}

static void preparePairingPasskey() {
    const uint32_t passkey = generatePairingPasskey();
    pairingPasskey.store(passkey, std::memory_order_release);
    pairingDisplayPasskey.store(0, std::memory_order_release);
    // NimBLE-Arduino 1.4.1 calls onPassKeyRequest() for DISPLAY_ONLY only when
    // its configured passkey still equals the library's 123456 sentinel. The
    // callback below returns the actual random value used by this connection.
    NimBLEDevice::setSecurityPasskey(NIMBLE_PASSKEY_CALLBACK_SENTINEL);
}

static void publishPairingPasskey() {
    const uint32_t passkey = pairingPasskey.load(std::memory_order_acquire);
    pairingDisplayPasskey.store(passkey, std::memory_order_release);
    logger.log(logging::LoggerLevel::LOGGER_LEVEL_INFO, "BLE Security",
               "Pairing passkey requested; see tracker display");
    logger.log(logging::LoggerLevel::LOGGER_LEVEL_DEBUG, "BLE Security",
               "Pairing passkey: %06lu", static_cast<unsigned long>(passkey));
}

static void clearPairingDisplay() {
    pairingDisplayPasskey.store(0, std::memory_order_release);
}

static int logBleGapSecurityEvent(ble_gap_event* event, void* arg) {
    (void)arg;

    switch (event->type) {
        case BLE_GAP_EVENT_ENC_CHANGE: {
            const int status = event->enc_change.status;
            logger.log(status == 0
                           ? logging::LoggerLevel::LOGGER_LEVEL_INFO
                           : logging::LoggerLevel::LOGGER_LEVEL_ERROR,
                       "BLE Security",
                       "Encryption change: connection=%u status=%d (%s)",
                       event->enc_change.conn_handle, status,
                       NimBLEUtils::returnCodeToString(status));
            break;
        }

        case BLE_GAP_EVENT_PASSKEY_ACTION:
            logger.log(logging::LoggerLevel::LOGGER_LEVEL_DEBUG, "BLE Security",
                       "Pairing I/O action: connection=%u action=%u",
                       event->passkey.conn_handle, event->passkey.params.action);
            break;

        case BLE_GAP_EVENT_REPEAT_PAIRING:
            logger.log(logging::LoggerLevel::LOGGER_LEVEL_WARN, "BLE Security",
                       "Peer requested repeat pairing: connection=%u",
                       event->repeat_pairing.conn_handle);
            break;

        default:
            break;
    }

    return 0;
}


class MyServerCallbacks : public NimBLEServerCallbacks {
    uint32_t onPassKeyRequest() override {
        // This callback is the only reliable place in NimBLE-Arduino 1.4.1 to
        // publish the display-only PIN and return the exact value it injects.
        const uint32_t passkey = pairingPasskey.load(std::memory_order_acquire);
        publishPairingPasskey();
        return passkey;
    }

    void onConnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) override {
        // The pairing-PIN state below (pairingPasskey/pairingDisplayPasskey) is a single shared
        // value with no per-connection keying, and the rest of this file already assumes a
        // single BLE peer (bluetoothConnected, shouldSendBLEtoLoRa, kissSerialBuffer are all
        // single-connection globals). NimBLE-Arduino 1.4.1 has no runtime max-connections
        // setter (that's a 2.x addition), so enforce single-connection at the application layer:
        // reject anything beyond the first.
        if (pServer->getConnectedCount() > 1) {
            logger.log(logging::LoggerLevel::LOGGER_LEVEL_WARN, "BLE",
                       "Rejecting extra connection: only one BLE peer is supported at a time");
            pServer->disconnect(desc->conn_handle);
            return;
        }

        bluetoothConnected = true;
        // peer_id_addr is the resolved identity address, which for a first-time connection
        // using a private/random address may not be resolved yet at this point; peer_ota_addr
        // is the address actually used over the air and is always valid here, so log both.
        const std::string peerIdAddress = NimBLEAddress(desc->peer_id_addr).toString();
        const std::string peerOtaAddress = NimBLEAddress(desc->peer_ota_addr).toString();
        logger.log(logging::LoggerLevel::LOGGER_LEVEL_INFO, "BLE",
                   "Client connected: id=%s ota=%s (encrypted=%u authenticated=%u bonded=%u)",
                   peerIdAddress.c_str(), peerOtaAddress.c_str(), desc->sec_state.encrypted,
                   desc->sec_state.authenticated, desc->sec_state.bonded);

        preparePairingPasskey();
        const int securityStatus = NimBLEDevice::startSecurity(desc->conn_handle);
        logger.log(securityStatus == 0
                       ? logging::LoggerLevel::LOGGER_LEVEL_DEBUG
                       : logging::LoggerLevel::LOGGER_LEVEL_ERROR,
                   "BLE Security", "Security request on connection=%u: status=%d (%s)",
                   desc->conn_handle, securityStatus,
                   NimBLEUtils::returnCodeToString(securityStatus));
    }

    void onDisconnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) override {
        bluetoothConnected = false;
        clearPairingDisplay();
        const std::string peerAddress = NimBLEAddress(desc->peer_id_addr).toString();
        logger.log(logging::LoggerLevel::LOGGER_LEVEL_INFO, "BLE",
                   "Client disconnected: %s; restarting advertising", peerAddress.c_str());
        pServer->startAdvertising();
    }

    void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
        clearPairingDisplay();
        const std::string peerAddress = NimBLEAddress(desc->peer_id_addr).toString();
        const logging::LoggerLevel level = desc->sec_state.encrypted && desc->sec_state.bonded
            ? logging::LoggerLevel::LOGGER_LEVEL_INFO
            : logging::LoggerLevel::LOGGER_LEVEL_WARN;

        logger.log(level, "BLE Security",
                   "Security complete for %s (encrypted=%u authenticated=%u bonded=%u keySize=%u)",
                   peerAddress.c_str(), desc->sec_state.encrypted,
                   desc->sec_state.authenticated, desc->sec_state.bonded,
                   desc->sec_state.key_size);
        logger.log(logging::LoggerLevel::LOGGER_LEVEL_DEBUG, "BLE Security",
                   "Stored bond count at authentication callback: %d (NVS persistence may complete afterward)",
                   NimBLEDevice::getNumBonds());
    }
};

class MyCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *pCharacteristic) {
        if (Config.bluetooth.useKISS) {   // KISS (AX.25)
            std::string receivedData = pCharacteristic->getValue();

            for (uint8_t c : receivedData) {                                                // save all received data from buffer
                kissSerialBuffer += (char)c;
            }
            if (kissSerialBuffer.length() > MAX_KISS_BUFFER) {                              // buffer overflow protection
                kissSerialBuffer = "";
                return;
            }

            int maxIterations = 10;                                                        // infinite loop protection
            while (maxIterations-- > 0) {

                if (kissSerialBuffer.length() == 0) break;                                  // empty buffer protection

                int fendIndex = -1;
                if (kissSerialBuffer.charAt(0) == (char)KissChar::FEND) {                   // starts with FEND???
                    for (int i = 1; i < kissSerialBuffer.length(); i++) {                   // look for next FEND
                        if (kissSerialBuffer.charAt(i) == (char)KissChar::FEND) {
                            fendIndex = i;
                            break;
                        }
                    }
                } else {
                    int firstFendIndex = kissSerialBuffer.indexOf((char)KissChar::FEND);    // find first FEND byte to discard leading corrupted bytes
                    if (firstFendIndex != -1) {
                        kissSerialBuffer.remove(0, firstFendIndex);                         // delete corrupted data before FEND
                    } else {
                        kissSerialBuffer = "";                                              // if no FEND found, delete all
                        break;
                    }
                    continue;
                }

                if (fendIndex == -1) {                                                      // exit: no FEND byte to process the kissSerialBuffer (yet)
                    break;
                }

                String frame = kissSerialBuffer.substring(0, fendIndex + 1);                // extract full frame (With FEND at start and end)
                kissSerialBuffer.remove(0, fendIndex + 1);

                if (frame.length() >= 4) {                                                  // FEND | CMD | DATA | FEND
                    bool isDataFrame    = false;
                    BLEToLoRaPacket     = KISS_Utils::decodeKISS(frame, isDataFrame);
                    if (isDataFrame) shouldSendBLEtoLoRa = true;
                }
            }
        } else {                            // TNC2
            std::string receivedData = pCharacteristic->getValue();
            String receivedString = "";
            for (int i = 0; i < receivedData.length(); i++) receivedString += receivedData[i];
            BLEToLoRaPacket = receivedString;
            shouldSendBLEtoLoRa = true;
        }
    }
};

namespace BLE_Utils {

    void stop() {
        clearPairingDisplay();
        BLEDevice::deinit();
    }

    void setup() {
        String BLEid = Config.bluetooth.deviceName;
        BLEDevice::init(BLEid.c_str());
        // NimBLE-Arduino 1.4.1 Secure Connections pairing stalled with the tested NA7Q
        // APRSdroid client. Authenticated legacy passkey pairing plus bidirectional ENC+ID
        // key distribution produced a persistent bond; keep this compatibility mode isolated
        // so a future NimBLE 2.x migration can retest Secure Connections independently.
        NimBLEDevice::setSecurityAuth(true, true, false);
        preparePairingPasskey();
        NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
        NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
        NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
        NimBLEDevice::setCustomGapHandler(logBleGapSecurityEvent);

        if (Config.bluetooth.bondResetPending) {
            const int previousBonds = NimBLEDevice::getNumBonds();
            NimBLEDevice::deleteAllBonds();
            Config.bluetooth.bondResetPending = false;
            logger.log(logging::LoggerLevel::LOGGER_LEVEL_WARN, "BLE Security",
                       "Cleared %d stored bond(s)", previousBonds);
            if (!Config.writeFile()) {
                logger.log(logging::LoggerLevel::LOGGER_LEVEL_ERROR, "BLE Security",
                           "Could not clear bond-reset flag; bonds were already deleted");
            }
        }

        const int storedBonds = NimBLEDevice::getNumBonds();
        logger.log(logging::LoggerLevel::LOGGER_LEVEL_INFO, "BLE Security",
                    "Legacy bonding enabled with a per-connection random PIN; stored bonds: %d",
                    storedBonds);
        for (int index = 0; index < storedBonds; ++index) {
            const std::string bondedAddress = NimBLEDevice::getBondedAddress(index).toString();
            logger.log(logging::LoggerLevel::LOGGER_LEVEL_DEBUG, "BLE Security",
                       "Stored bond %d: %s", index + 1, bondedAddress.c_str());
        }

        pServer = BLEDevice::createServer();
        pServer->setCallbacks(new MyServerCallbacks());

        BLEService *pService = nullptr;

        //  KISS (AX.25) or TNC2
        bool useKISS = Config.bluetooth.useKISS;
        pService = pServer->createService(useKISS ? SERVICE_UUID_0 : SERVICE_UUID_1);
        pCharacteristicTx = pService->createCharacteristic(useKISS ? CHARACTERISTIC_UUID_TX_0 : CHARACTERISTIC_UUID_TX_1,
                                                            NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY |
                                                            NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN);
        pCharacteristicRx = pService->createCharacteristic(useKISS ? CHARACTERISTIC_UUID_RX_0 : CHARACTERISTIC_UUID_RX_1,
                                                            NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR |
                                                            NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN);

        if (pService != nullptr) {
            pCharacteristicRx->setCallbacks(new MyCallbacks());
            pService->start();

            BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
            pAdvertising->addServiceUUID(useKISS ? SERVICE_UUID_0 : SERVICE_UUID_1);

            pServer->getAdvertising()->setScanResponse(true);
            pServer->getAdvertising()->setMinPreferred(0x06);
            pServer->getAdvertising()->setMaxPreferred(0x0C);
            pAdvertising->start();
            logger.log(logging::LoggerLevel::LOGGER_LEVEL_DEBUG, "BLE", "%s", "Waiting for BLE central to connect...");
        } else {
            logger.log(logging::LoggerLevel::LOGGER_LEVEL_ERROR, "BLE", "Failed to create BLE service");
        }
    }

    bool handlePairingDisplay() {
        static uint32_t displayedPasskey = 0;
        static uint32_t displayStartedAt = 0;
        static uint32_t lastDisplayAt = 0;

        const uint32_t passkey = pairingDisplayPasskey.load(std::memory_order_acquire);
        if (passkey == 0) {
            displayedPasskey = 0;
            displayStartedAt = 0;
            lastDisplayAt = 0;
            return false;
        }

        const uint32_t now = millis();
        const bool isNewPasskey = passkey != displayedPasskey;
        if (isNewPasskey) {
            displayedPasskey = passkey;
            displayStartedAt = now;
        }

        if (now - displayStartedAt >= BLE_PAIRING_DISPLAY_TIMEOUT_MS) {
            uint32_t expectedPasskey = passkey;
            // If this CAS fails, a new passkey was published concurrently (a fresh pairing
            // attempt) between the load above and here; that new value is now current, so don't
            // log a stale timeout or reset the display bookkeeping out from under it. The next
            // call will see it as a new passkey and start its own display/timeout cycle.
            if (pairingDisplayPasskey.compare_exchange_strong(
                    expectedPasskey, 0, std::memory_order_acq_rel)) {
                logger.log(logging::LoggerLevel::LOGGER_LEVEL_WARN, "BLE Security",
                           "Pairing PIN display timed out");
                displayedPasskey = 0;
                displayStartedAt = 0;
                lastDisplayAt = 0;
            }
            return false;
        }

        if (!displayState) {
            displayToggle(true);
            displayState = true;
        }
        displayTime = now;

        if (isNewPasskey || now - lastDisplayAt >= BLE_PAIRING_DISPLAY_REFRESH_MS) {
            char formattedPasskey[7];
            snprintf(formattedPasskey, sizeof(formattedPasskey), "%06lu",
                     static_cast<unsigned long>(passkey));
            displayShow("BLE PAIR", "Enter PIN on phone", "PIN: " + String(formattedPasskey));
            lastDisplayAt = now;
        }

        return true;
    }

    void sendToLoRa() {
        if (!shouldSendBLEtoLoRa) return;

        logger.log(logging::LoggerLevel::LOGGER_LEVEL_DEBUG, "BLE Tx", "%s", BLEToLoRaPacket.c_str());
        displayShow("BLE Tx >>", "", BLEToLoRaPacket, 1000);
        LoRa_Utils::sendNewPacket(BLEToLoRaPacket);
        BLEToLoRaPacket = "";
        shouldSendBLEtoLoRa = false;
    }

    void txBLE(uint8_t p) {
        pCharacteristicTx->setValue(&p,1);
        pCharacteristicTx->notify();
        delay(3);
    }

    void txToPhoneOverBLE(const String& frame) {
        if (Config.bluetooth.useKISS) {   // KISS (AX.25)
            const String kissEncodedFrame = KISS_Utils::encodeKISS(frame);

            const char* t   = kissEncodedFrame.c_str();
            int length      = kissEncodedFrame.length();
            for (int i = 0; i < length; i += BLE_CHUNK_SIZE) {
                int chunkSize = (length - i < BLE_CHUNK_SIZE) ? (length - i) : BLE_CHUNK_SIZE;

                uint8_t* chunk = new uint8_t[chunkSize];
                memcpy(chunk, t + i, chunkSize);

                pCharacteristicTx->setValue(chunk, chunkSize);
                pCharacteristicTx->notify();
                delete[] chunk;
                delay(200);
            }
        } else {        // TNC2
            for (int n = 0; n < frame.length(); n++) txBLE(frame[n]);
            txBLE('\n');
        }
    }

    void sendToPhone(const String& packet) {
        if (!packet.isEmpty() && bluetoothConnected) {
            logger.log(logging::LoggerLevel::LOGGER_LEVEL_DEBUG, "BLE Rx", "%s", packet.c_str());
            String receivedPacketString = "";
            for (int i = 0; i < packet.length(); i++) receivedPacketString += packet[i];
            txToPhoneOverBLE(receivedPacketString);
        }
    }

}
