#include "web.h"

#include <Arduino.h>
#include <ArduinoJson.h>

#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <FS.h>
#include <LittleFS.h>
#include <SPIFFSEditor.h>
#include <WiFi.h>
#include <WiFiManager.h>  // https://github.com/tzapu/WiFiManager/tree/feature_asyncwebserver

#include "commstructs.h"
#include "newproto.h"
#include "settings.h"
#include "tag_db.h"

extern uint8_t data_to_send[];

const char *http_username = "admin";
const char *http_password = "admin";
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

SemaphoreHandle_t wsMutex;
TaskHandle_t websocketUpdater;

uint64_t swap64(uint64_t x) {
    uint64_t byte1 = x & 0xff00000000000000;
    uint64_t byte2 = x & 0x00ff000000000000;
    uint64_t byte3 = x & 0x0000ff0000000000;
    uint64_t byte4 = x & 0x000000ff00000000;
    uint64_t byte5 = x & 0x00000000ff000000;
    uint64_t byte6 = x & 0x0000000000ff0000;
    uint64_t byte7 = x & 0x000000000000ff00;
    uint64_t byte8 = x & 0x00000000000000ff;

    return (uint64_t)(byte1 >> 56 | byte2 >> 40 | byte3 >> 24 | byte4 >> 8 |
                      byte5 << 8 | byte6 << 24 | byte7 << 40 | byte8 << 56);
}

void webSocketSendProcess(void *parameter) {
    uint32_t ulNotificationValue;
    Serial.print("websocket thread started\n");
    websocketUpdater = xTaskGetCurrentTaskHandle();
    wsMutex = xSemaphoreCreateMutex();
    while (true) {
        ulNotificationValue = ulTaskNotifyTake(pdTRUE, 1000 / portTICK_RATE_MS);
        if (ulNotificationValue == 0) {  // timeout, so every 1s
            ws.cleanupClients();
        } else {
            // if (ws.count())
            //  sendStatus(STATUS_WIFI_ACTIVITY);
            DynamicJsonDocument doc(1500);
            if (ulNotificationValue & 2) {  // WS_SEND_MODE_STATUS) {
            }
            /*
                JsonArray statusframes = doc.createNestedArray("frames");
            }*/
            size_t len = measureJson(doc);
            xSemaphoreTake(wsMutex, portMAX_DELAY);
            auto buffer = std::make_shared<std::vector<uint8_t>>(len);
            serializeJson(doc, buffer->data(), len);
            // ws.textAll((char*)buffer->data());
            xSemaphoreGive(wsMutex);
        }
    }
}

void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
    switch (type) {
        case WS_EVT_CONNECT:
            // client connected
            ets_printf("ws[%s][%u] connect\n", server->url(), client->id());
            xTaskNotify(websocketUpdater, 2, eSetBits);
            // client->ping();
            break;
        case WS_EVT_DISCONNECT:
            // client disconnected
            ets_printf("ws[%s][%u] disconnect: %u\n", server->url(), client->id());
            break;
        case WS_EVT_ERROR:
            // error was received from the other end
            ets_printf("WS Error received :(\n\n");
            // ets_printf("ws[%s][%u] error(%u): %s\n", server->url(), client->id(), *((uint16_t *)arg), (char *)data);
            break;
        case WS_EVT_PONG:
            // pong message was received (in response to a ping request maybe)
            ets_printf("ws[%s][%u] pong[%u]: %s\n", server->url(), client->id(), len, (len) ? (char *)data : "");
            break;
        case WS_EVT_DATA:
            // data packet
            AwsFrameInfo *info = (AwsFrameInfo *)arg;
            if (info->final && info->index == 0 && info->len == len) {
                // the whole message is in a single frame and we got all of it's data
                ets_printf("ws[%s][%u] %s-message[%llu]: ", server->url(), client->id(), (info->opcode == WS_TEXT) ? "text" : "binary", info->len);
                if (info->opcode == WS_TEXT) {
                    data[len] = 0;
                    ets_printf("%s\n", (char *)data);
                } else {
                    for (size_t i = 0; i < info->len; i++) {
                        ets_printf("%02x ", data[i]);
                    }
                    ets_printf("\n");
                }
                if (info->opcode == WS_TEXT)
                    client->text("{\"status\":\"received\"}");
                else
                    client->binary("{\"status\":\"received\"}");
            } else {
                // message is comprised of multiple frames or the frame is split into multiple packets
                if (info->index == 0) {
                    if (info->num == 0)
                        ets_printf("ws[%s][%u] %s-message start\n", server->url(), client->id(), (info->message_opcode == WS_TEXT) ? "text" : "binary");
                    ets_printf("ws[%s][%u] frame[%u] start[%llu]\n", server->url(), client->id(), info->num, info->len);
                }

                ets_printf("ws[%s][%u] frame[%u] %s[%llu - %llu]: ", server->url(), client->id(), info->num, (info->message_opcode == WS_TEXT) ? "text" : "binary", info->index, info->index + len);
                if (info->message_opcode == WS_TEXT) {
                    data[len] = 0;
                    ets_printf("%s\n", (char *)data);
                } else {
                    for (size_t i = 0; i < len; i++) {
                        ets_printf("%02x ", data[i]);
                    }
                    ets_printf("\n");
                }

                if ((info->index + len) == info->len) {
                    ets_printf("ws[%s][%u] frame[%u] end[%llu]\n", server->url(), client->id(), info->num, info->len);
                    if (info->final) {
                        ets_printf("ws[%s][%u] %s-message end\n", server->url(), client->id(), (info->message_opcode == WS_TEXT) ? "text" : "binary");
                        if (info->message_opcode == WS_TEXT)
                            client->text("{\"status\":\"received\"}");
                        else
                            client->binary("{\"status\":\"received\"}");
                    }
                }
            }
            break;
    }
}

void wsLog(String text) {
    StaticJsonDocument<500> doc;
    doc["logMsg"] = text;
    xSemaphoreTake(wsMutex, portMAX_DELAY);
    ws.textAll(doc.as<String>());
    xSemaphoreGive(wsMutex);
}

void wsErr(String text) {
    StaticJsonDocument<500> doc;
    doc["errMsg"] = text;
    xSemaphoreTake(wsMutex, portMAX_DELAY);
    ws.textAll(doc.as<String>());
    xSemaphoreGive(wsMutex);
}

void wsSendSysteminfo() {
    DynamicJsonDocument doc(250);
    JsonObject sys = doc.createNestedObject("sys");
    time_t now;
    time(&now);
    sys["currtime"] = now;
    sys["heap"] = ESP.getFreeHeap();
    sys["recordcount"] = tagDB.size();
    sys["dbsize"] = tagDB.size() * sizeof(tagRecord);
    sys["littlefsfree"] = LittleFS.totalBytes() - LittleFS.usedBytes();

    xSemaphoreTake(wsMutex, portMAX_DELAY);
    ws.textAll(doc.as<String>());
    xSemaphoreGive(wsMutex);
}

void wsSendTaginfo(uint8_t mac[6]) {

    String json = "";
    json = tagDBtoJson(mac);

    xSemaphoreTake(wsMutex, portMAX_DELAY);
    ws.textAll(json);
    xSemaphoreGive(wsMutex);

}

void init_web() {
    LittleFS.begin(true);

    if (!LittleFS.exists("/current")) {
        LittleFS.mkdir("/current");
    }
    if (!LittleFS.exists("/temp")) {
        LittleFS.mkdir("/temp");
    }

    WiFi.mode(WIFI_STA);
    WiFiManager wm;
    bool res;
    res = wm.autoConnect("AutoConnectAP");
    if (!res) {
        Serial.println("Failed to connect");
        ESP.restart();
    }
    Serial.print("Connected! IP address: ");
    Serial.println(WiFi.localIP());

    server.addHandler(new SPIFFSEditor(LittleFS, http_username, http_password));

    ws.onEvent(onEvent);
    server.addHandler(&ws);

    server.on("/reboot", HTTP_POST, [](AsyncWebServerRequest *request) {
        request->send(200, "text/plain", "OK Reboot");
        ESP.restart();
    });

    server.serveStatic("/current", LittleFS, "/current/");
    server.serveStatic("/", LittleFS, "/www/").setDefaultFile("index.html");
    
    server.on(
        "/imgupload", HTTP_POST, [](AsyncWebServerRequest *request) {
            request->send(200);
        },
        doImageUpload);

    server.on("/req_checkin", HTTP_POST, [](AsyncWebServerRequest *request) {
        String filename;
        String dst;
        if (request->hasParam("dst", true)) {
            dst = request->getParam("dst", true)->value();
            uint8_t mac_addr[12];  // I expected this to return like 8 values, but if I make the array 8 bytes long, things die.
            mac_addr[0] = 0x00;
            mac_addr[1] = 0x00;
            if (sscanf(dst.c_str(), "%02X%02X%02X%02X%02X%02X",
                       &mac_addr[2],
                       &mac_addr[3],
                       &mac_addr[4],
                       &mac_addr[5],
                       &mac_addr[6],
                       &mac_addr[7]) != 6) {
                request->send(200, "text/plain", "Something went wrong trying to parse the mac address");
            } else {
                *((uint64_t *)mac_addr) = swap64(*((uint64_t *)mac_addr));
                if (prepareDataAvail(&filename, DATATYPE_NOUPDATE, mac_addr,0)) {
                    request->send(200, "text/plain", "Sending check-in request to " + dst);
                }
            }
            return;
        }
        request->send(200, "text/plain", "Didn't get the required params");
        return;
    });

    server.on("/get_db", HTTP_GET, [](AsyncWebServerRequest *request) {
        String json = "";
        if (request->hasParam("mac")) {
            String dst = request->getParam("mac")->value();
            uint8_t mac[6];
            if (sscanf(dst.c_str(), "%02X%02X%02X%02X%02X%02X", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5])==6) {
                json = tagDBtoJson(mac);
            }
        } else {
            uint8_t startPos=0;
            if (request->hasParam("pos")) {
                startPos = atoi(request->getParam("pos")->value().c_str());
            }
            json = tagDBtoJson(nullptr,startPos);
        }
        request->send(200, "application/json", json);
    });

    server.on("/save_cfg", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (request->hasParam("mac", true)) {
            String dst = request->getParam("mac", true)->value();
            uint8_t mac[6];
            if (sscanf(dst.c_str(), "%02X%02X%02X%02X%02X%02X", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6) {
                tagRecord *taginfo = nullptr;
                taginfo = tagRecord::findByMAC(mac);
                if (taginfo != nullptr) {
                    taginfo->alias = request->getParam("alias", true)->value();
                    taginfo->modeConfigJson = request->getParam("modecfgjson", true)->value();
                    taginfo->contentMode = (contentModes)atoi(request->getParam("contentmode", true)->value().c_str());
                    taginfo->model = atoi(request->getParam("model", true)->value().c_str());
                    taginfo->nextupdate = 0;
                    wsSendTaginfo(mac);
                    saveDB("/current/tagDB.json");
                    request->send(200, "text/plain", "Ok, saved");
                } else {
                    request->send(200, "text/plain", "Error while saving: mac not found");
                }
            }
        }
        request->send(200, "text/plain", "Ok, saved");
    });

    server.onNotFound([](AsyncWebServerRequest *request) {
        if (request->url() == "/" || request->url() == "index.htm") {
            request->send(200, "text/html", "-");
            return;
        }
        request->send(404);
    });

    server.begin();
}

void doImageUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
    if (!index) {
        Serial.print((String) "UploadStart: " + filename);
        // open the file on first call and store the file handle in the request object
        request->_tempFile = LittleFS.open("/" + filename, "w");
    }
    if (len) {
        // stream the incoming chunk to the opened file
        request->_tempFile.write(data, len);
    }
    if (final) {
        Serial.print((String) "UploadEnd: " + filename + "," + index + len);
        // close the file handle as the upload is now done
        request->_tempFile.close();
        request->send(200, "text/plain", "File Uploaded !");
        /*
                sscanf() if (request->hasParam("id") && request->hasParam("file")) {
                    id = request->getParam("id")->value().toInt();
                    filename = request->getParam("file")->value();
                }
                */
    }
}