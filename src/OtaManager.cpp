#include "OtaManager.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#include <mbedtls/md.h>

#include "DisplayManager.h"
#include "globals.h"

static const char* OTA_USER_AGENT = "nightscout-clock-ota";
static const int OTA_FILESYSTEM_ATTEMPTS = 3;

OtaManager_& OtaManager_::getInstance() {
    static OtaManager_ instance;
    return instance;
}

OtaManager_& OtaManager = OtaManager.getInstance();

// ---------- shared helpers ----------

// A retry after a failed write finds the filesystem unmounted; the earlier backup is kept then.
void OtaManager_::beginFilesystemWrite() {
    File file = LittleFS.open(CONFIG_JSON, "r");
    if (file) {
        configBackup = file.readString();
        file.close();
    }
    LittleFS.end();
}

bool OtaManager_::endFilesystemWrite() {
    if (!LittleFS.begin(false)) {
        return false;
    }
    if (configBackup.length() > 0) {
        File file = LittleFS.open(CONFIG_JSON, FILE_WRITE);
        if (file) {
            file.print(configBackup);
            file.close();
        }
    }
    configBackup = "";
    return true;
}

String OtaManager_::readCurrentVersion() {
    String version = "";
    File file = LittleFS.open("/version.txt", "r");
    if (file) {
        version = file.readString();
        file.close();
    }
    version.trim();
    return version;
}

// Compares "1.2.10" with "1.2.9" part by part; a leading "v" or "v-" is ignored.
int OtaManager_::compareVersions(const String& a, const String& b) {
    auto strip = [](String v) {
        v.trim();
        if (v.startsWith("v") || v.startsWith("V")) v.remove(0, 1);
        if (v.startsWith("-")) v.remove(0, 1);
        return v;
    };
    String x = strip(a), y = strip(b);
    int i = 0, j = 0;
    while (i < (int)x.length() || j < (int)y.length()) {
        long px = 0, py = 0;
        while (i < (int)x.length() && isDigit(x[i])) px = px * 10 + (x[i++] - '0');
        while (j < (int)y.length() && isDigit(y[j])) py = py * 10 + (y[j++] - '0');
        if (px != py) return px < py ? -1 : 1;
        while (i < (int)x.length() && !isDigit(x[i])) i++;
        while (j < (int)y.length() && !isDigit(y[j])) j++;
    }
    return 0;
}

// ---------- requests from the web server (any task) ----------

void OtaManager_::requestCheck() {
    OTA_STATE s = state;
    if (s == OTA_STATE::IDLE || s == OTA_STATE::CHECKED || s == OTA_STATE::ERROR) {
        state = OTA_STATE::CHECK_REQUESTED;
    }
}

bool OtaManager_::requestInstall() {
    OTA_STATE s = state;
    if (s == OTA_STATE::IDLE || s == OTA_STATE::CHECKED || s == OTA_STATE::ERROR) {
        progress = 0;
        state = OTA_STATE::INSTALL_REQUESTED;
        return true;
    }
    return false;
}

String OtaManager_::statusJson() {
    static const char* names[] = {"idle",        "check_requested",        "checking",
                                  "checked",     "install_requested",      "downloading_firmware",
                                  "downloading_filesystem", "restarting", "error"};
    JsonDocument doc;
    doc["state"] = names[static_cast<uint8_t>(state)];
    doc["repo"] = OTA_GITHUB_REPO;
    doc["current"] = currentVersion;
    doc["latest"] = latestVersion;
    doc["available"] = latestVersion[0] != 0 && currentVersion[0] != 0 &&
                       compareVersions(currentVersion, latestVersion) < 0;
    doc["progress"] = progress;
    doc["error"] = lastError;
    String out;
    serializeJson(doc, out);
    return out;
}

// ---------- work on the main loop ----------

void OtaManager_::tick() {
    switch (state) {
        case OTA_STATE::CHECK_REQUESTED:
            state = OTA_STATE::CHECKING;
            if (fetchLatestRelease()) {
                state = OTA_STATE::CHECKED;
            }
            break;
        case OTA_STATE::INSTALL_REQUESTED:
            if (install()) {
                state = OTA_STATE::RESTARTING;
                restartAtMs = millis() + 2000;
            }
            break;
        case OTA_STATE::RESTARTING:
            if ((long)(millis() - restartAtMs) >= 0) {
                LittleFS.end();
                ESP.restart();
            }
            break;
        default:
            break;
    }
}

void OtaManager_::fail(const String& error) {
    DEBUG_PRINTF("OTA: %s\n", error.c_str());
    strlcpy(lastError, error.c_str(), sizeof(lastError));
    state = OTA_STATE::ERROR;
}

void OtaManager_::showOnDisplay(const char* text) {
    DisplayManager.clearMatrix();
    DisplayManager.setFont(FONT_TYPE::MEDIUM);
    DisplayManager.setTextColor(COLOR_CYAN);
    DisplayManager.printText(0, 6, text, TEXT_ALIGNMENT::CENTER, 2);
    DisplayManager.update();
}

bool OtaManager_::fetchLatestRelease() {
    lastError[0] = 0;
    strlcpy(currentVersion, readCurrentVersion().c_str(), sizeof(currentVersion));

    WiFiClientSecure client;
    client.setInsecure();  // integrity is checked with the SHA-256 digests GitHub publishes for assets
    HTTPClient http;
    http.useHTTP10(true);  // no chunked encoding, so the JSON can be parsed straight from the stream
    http.setTimeout(15000);
    http.setUserAgent(OTA_USER_AGENT);
    if (!http.begin(client, "https://api.github.com/repos/" OTA_GITHUB_REPO "/releases/latest")) {
        fail("could not reach GitHub");
        return false;
    }
    http.addHeader("Accept", "application/vnd.github+json");
    int code = http.GET();
    if (code != 200) {
        http.end();
        fail(code == 404 ? String("no release found on GitHub") : String("GitHub answered ") + String(code));
        return false;
    }

    JsonDocument filter;
    filter["tag_name"] = true;
    filter["assets"][0]["name"] = true;
    filter["assets"][0]["browser_download_url"] = true;
    filter["assets"][0]["digest"] = true;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    if (err) {
        fail(String("bad answer from GitHub: ") + err.c_str());
        return false;
    }

    String tag = doc["tag_name"] | "";
    String version = tag;
    if (version.startsWith("v") || version.startsWith("V")) version.remove(0, 1);
    if (version.startsWith("-")) version.remove(0, 1);
    strlcpy(latestVersion, version.c_str(), sizeof(latestVersion));

    firmwareUrl = firmwareDigest = filesystemUrl = filesystemDigest = "";
    for (JsonObject asset : doc["assets"].as<JsonArray>()) {
        String name = asset["name"] | "";
        if (name == "firmware.bin") {
            firmwareUrl = asset["browser_download_url"] | "";
            firmwareDigest = asset["digest"] | "";
        } else if (name == "littlefs.bin") {
            filesystemUrl = asset["browser_download_url"] | "";
            filesystemDigest = asset["digest"] | "";
        }
    }
    if (firmwareUrl.length() == 0 || filesystemUrl.length() == 0) {
        fail("release " + tag + " has no firmware.bin and littlefs.bin");
        return false;
    }
    DEBUG_PRINTF("OTA: current %s, latest %s\n", currentVersion, latestVersion);
    return true;
}

bool OtaManager_::install() {
    state = OTA_STATE::CHECKING;
    if (!fetchLatestRelease()) {
        return false;
    }

    // Firmware first: it only takes effect after the restart.
    state = OTA_STATE::DOWNLOADING_FIRMWARE;
    showOnDisplay("UPD FW");
    if (!download(firmwareUrl, U_FLASH, firmwareDigest, 0, 80)) {
        return false;
    }

    // Then the settings page and version file, keeping config.json.
    state = OTA_STATE::DOWNLOADING_FILESYSTEM;
    showOnDisplay("UPD FS");
    for (int attempt = 1; attempt <= OTA_FILESYSTEM_ATTEMPTS; attempt++) {
        beginFilesystemWrite();
        bool ok = download(filesystemUrl, U_SPIFFS, filesystemDigest, 80, 100);
        bool mounted = endFilesystemWrite();
        if (ok && mounted) {
            showOnDisplay("REBOOT");
            return true;
        }
        if (ok && !mounted) {
            fail("the new filesystem image could not be mounted");
        }
        state = OTA_STATE::DOWNLOADING_FILESYSTEM;
        DEBUG_PRINTF("OTA: filesystem attempt %d failed\n", attempt);
    }
    // The new firmware is already staged; don't restart so the update can be retried first.
    fail(String("filesystem update failed: ") + lastError + " - try again before unplugging the clock");
    showOnDisplay("UPD ERR");
    return false;
}

bool OtaManager_::download(
    const String& url, int command, const String& digest, uint8_t progressFrom, uint8_t progressTo) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.useHTTP10(true);
    http.setTimeout(20000);
    http.setUserAgent(OTA_USER_AGENT);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);  // GitHub redirects assets to its CDN
    if (!http.begin(client, url)) {
        fail("could not start the download");
        return false;
    }
    int code = http.GET();
    if (code != 200) {
        http.end();
        fail("download answered " + String(code));
        return false;
    }
    int total = http.getSize();
    if (total <= 0) {
        http.end();
        fail("download size unknown");
        return false;
    }
    if (!Update.begin(total, command)) {
        http.end();
        fail(String("not enough space: ") + Update.errorString());
        return false;
    }

    mbedtls_md_context_t sha;
    mbedtls_md_init(&sha);
    mbedtls_md_setup(&sha, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
    mbedtls_md_starts(&sha);

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buffer[1024];
    int written = 0;
    unsigned long lastDataMs = millis();
    while (written < total) {
        size_t available = stream->available();
        if (available > 0) {
            int n = stream->readBytes(buffer, min(available, sizeof(buffer)));
            if (n <= 0) continue;
            if (Update.write(buffer, n) != (size_t)n) {
                break;
            }
            mbedtls_md_update(&sha, buffer, n);
            written += n;
            lastDataMs = millis();
            progress = progressFrom + (uint32_t)(progressTo - progressFrom) * written / total;
        } else {
            if (!stream->connected() || millis() - lastDataMs > 20000) {
                break;
            }
            delay(2);
        }
    }
    http.end();

    uint8_t hash[32];
    mbedtls_md_finish(&sha, hash);
    mbedtls_md_free(&sha);

    if (written != total) {
        Update.abort();
        String reason = "download interrupted";
        if (Update.hasError()) {
            reason = String("write failed: ") + Update.errorString();
        }
        fail(reason);
        return false;
    }

    if (digest.startsWith("sha256:")) {
        char hex[65];
        for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", hash[i]);
        if (!digest.substring(7).equalsIgnoreCase(hex)) {
            Update.abort();
            fail("checksum mismatch, update rejected");
            return false;
        }
    } else {
        DEBUG_PRINTLN("OTA: GitHub gave no digest for this asset, skipping the checksum");
    }

    if (!Update.end(true)) {
        fail(String("could not finish: ") + Update.errorString());
        return false;
    }
    progress = progressTo;
    DEBUG_PRINTF("OTA: %d bytes written\n", written);
    return true;
}
