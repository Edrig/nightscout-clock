#ifndef OtaManager_h
#define OtaManager_h

#include <Arduino.h>

// GitHub repository ("owner/name") whose latest release provides firmware.bin and littlefs.bin.
// Override with -DOTA_GITHUB_REPO=\"owner/name\" in platformio.local.ini if you prefer.
#ifndef OTA_GITHUB_REPO
#define OTA_GITHUB_REPO "Edrig/nightscout-clock"
#endif

enum class OTA_STATE : uint8_t {
    IDLE,
    CHECK_REQUESTED,
    CHECKING,
    CHECKED,
    INSTALL_REQUESTED,
    DOWNLOADING_FIRMWARE,
    DOWNLOADING_FILESYSTEM,
    RESTARTING,
    ERROR,
};

// Over-the-air updates: helpers shared with the upload route, and installing the latest GitHub release.
// The network work runs from tick() on the main loop, never inside a web request handler.
class OtaManager_ {
public:
    static OtaManager_& getInstance();

    void tick();
    void requestCheck();
    bool requestInstall();
    String statusJson();

    // Keep config.json across a filesystem image write.
    void beginFilesystemWrite();
    bool endFilesystemWrite();

    String readCurrentVersion();
    static int compareVersions(const String& a, const String& b);

private:
    OtaManager_() = default;

    volatile OTA_STATE state = OTA_STATE::IDLE;
    volatile uint8_t progress = 0;
    char currentVersion[24] = "";
    char latestVersion[24] = "";
    char lastError[96] = "";
    String firmwareUrl, firmwareDigest, filesystemUrl, filesystemDigest;
    String configBackup;
    unsigned long restartAtMs = 0;

    bool fetchLatestRelease();
    bool install();
    bool download(const String& url, int command, const String& digest, uint8_t progressFrom, uint8_t progressTo);
    void fail(const String& error);
    void showOnDisplay(const char* text);
};

extern OtaManager_& OtaManager;

#endif
