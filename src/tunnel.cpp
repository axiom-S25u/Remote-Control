#include "tunnel.hpp"
#include <Geode/Geode.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/ui/Popup.hpp>
#include <thread>
#include <filesystem>
#include <cstdlib>

using namespace geode::prelude;

static std::mutex g_urlMtx;
static std::string g_url;
static std::thread g_readerThread;
static bool g_running = false;
static geode::async::TaskHolder<geode::utils::web::WebResponse> g_downloadTask;
class DownloadProgressPopup;
static DownloadProgressPopup* g_downloadPopup = nullptr;

#ifdef GEODE_IS_WINDOWS
#include <windows.h>

static HANDLE g_process = nullptr;
static HANDLE g_readPipe = nullptr;

void startTunnel() {
    if (g_running) return;
    g_running = true;
    g_url.clear();

    std::filesystem::path cfPath = Mod::get()->getSaveDir() / "cloudflared.exe";
    std::error_code ec;
    if (!std::filesystem::exists(cfPath, ec)) {
        log::error("cloudflared binary not found at {}", geode::utils::string::pathToString(cfPath));
        g_running = false;
        return;
    }
    std::string cfStr = geode::utils::string::pathToString(cfPath);
    std::string cmd = "\"" + cfStr + "\" tunnel --url http://localhost:8080 --no-autoupdate";

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE writePipe = nullptr;
    CreatePipe(&g_readPipe, &writePipe, &sa, 0);
    SetHandleInformation(g_readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.hStdError = writePipe;
    si.hStdOutput = writePipe;
    si.dwFlags = STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi = {};

    BOOL ok = CreateProcessA(
        nullptr,
        const_cast<char*>(cmd.c_str()),
        nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW,
        nullptr, nullptr,
        &si, &pi
    );

    CloseHandle(writePipe);

    if (!ok) {
        log::error("failed to start cloudflared: {}", GetLastError());
        g_running = false;
        CloseHandle(g_readPipe);
        g_readPipe = nullptr;
        return;
    }

    g_process = pi.hProcess;
    CloseHandle(pi.hThread);

    g_readerThread = std::thread([]() {
        char buf[4096];
        DWORD bytesRead;
        std::string leftover;

        while (g_running && ReadFile(g_readPipe, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0) {
            buf[bytesRead] = '\0';
            leftover += buf;

            size_t pos;
            while ((pos = leftover.find('\n')) != std::string::npos) {
                std::string line = leftover.substr(0, pos);
                leftover = leftover.substr(pos + 1);

                size_t idx = line.find("trycloudflare.com");
                if (idx != std::string::npos) {
                    size_t start = line.rfind("https://", idx);
                    if (start != std::string::npos) {
                        size_t end = idx + 17;
                        while (end < line.size() && line[end] != ' ' && line[end] != '"' && line[end] != '\r') end++;
                        std::string url = line.substr(start, end - start);
                        log::info("tunnel url: {}", url);
                        std::lock_guard<std::mutex> lock(g_urlMtx);
                        g_url = url;
                    }
                }
            }
        }
    });
}

void stopTunnel() {
    g_running = false;

    if (g_process) {
        TerminateProcess(g_process, 0);
        CloseHandle(g_process);
        g_process = nullptr;
    }

    if (g_readPipe) {
        CloseHandle(g_readPipe);
        g_readPipe = nullptr;
    }

    if (g_readerThread.joinable()) {
        g_readerThread.join();
    }

    std::lock_guard<std::mutex> lock(g_urlMtx);
    g_url.clear();
}

#elif defined(GEODE_IS_MACOS)
#include <unistd.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>

static pid_t g_pid = 0;
static int g_readFd = -1;

void startTunnel() {
    if (g_running) return;
    g_running = true;
    g_url.clear();

    std::filesystem::path cfPath = Mod::get()->getSaveDir() /
#ifdef GEODE_IS_INTEL_MAC
        "cloudflared-intel";
#else
        "cloudflared-mac";
#endif
    std::error_code ec;
    if (!std::filesystem::exists(cfPath, ec)) {
        log::error("cloudflared binary not found at {}", geode::utils::string::pathToString(cfPath));
        g_running = false;
        Loader::get()->queueInMainThread([]() {
            Notification::create("download cloudflared from the button in the main menu", NotificationIcon::Warning)->show();
        });
        return;
    }
    std::string cfStr = geode::utils::string::pathToString(cfPath);
    std::filesystem::permissions(cfPath,
        std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
        std::filesystem::perm_options::add, ec);

    if (ec) {
        log::error("chmod failed: {}", ec.message());
    }

    int pipeFds[2];
    if (pipe(pipeFds) != 0) {
        log::error("pipe() failed");
        g_running = false;
        return;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipeFds[1], STDERR_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipeFds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipeFds[0]);
    posix_spawn_file_actions_addclose(&actions, pipeFds[1]);

    char* argv[] = {
        const_cast<char*>(cfStr.c_str()),
        const_cast<char*>("tunnel"),
        const_cast<char*>("--url"),
        const_cast<char*>("http://localhost:8080"),
        const_cast<char*>("--no-autoupdate"),
        nullptr
    };

    int ret = posix_spawn(&g_pid, cfStr.c_str(), &actions, nullptr, argv, nullptr);
    posix_spawn_file_actions_destroy(&actions);
    close(pipeFds[1]);

    if (ret != 0) {
        log::error("posix_spawn failed: {}", ret);
        close(pipeFds[0]);
        g_running = false;
        return;
    }

    g_readFd = pipeFds[0];

    g_readerThread = std::thread([]() {
        char buf[4096];
        std::string leftover;

        while (g_running) {
            ssize_t n = read(g_readFd, buf, sizeof(buf) - 1);
            if (n <= 0) break;
            buf[n] = '\0';
            leftover += buf;

            size_t pos;
            while ((pos = leftover.find('\n')) != std::string::npos) {
                std::string line = leftover.substr(0, pos);
                leftover = leftover.substr(pos + 1);

                size_t idx = line.find("trycloudflare.com");
                if (idx != std::string::npos) {
                    size_t start = line.rfind("https://", idx);
                    if (start != std::string::npos) {
                        size_t end = idx + 17;
                        while (end < line.size() && line[end] != ' ' && line[end] != '"' && line[end] != '\n') end++;
                        std::string url = line.substr(start, end - start);
                        log::info("tunnel url: {}", url);
                        std::lock_guard<std::mutex> lock(g_urlMtx);
                        g_url = url;
                    }
                }
            }
        }
    });
}

void stopTunnel() {
    g_running = false;

    if (g_pid > 0) {
        kill(g_pid, SIGTERM);
        int status;
        waitpid(g_pid, &status, 0);
        g_pid = 0;
    }

    if (g_readFd >= 0) {
        close(g_readFd);
        g_readFd = -1;
    }

    if (g_readerThread.joinable()) {
        g_readerThread.join();
    }

    std::lock_guard<std::mutex> lock(g_urlMtx);
    g_url.clear();
}

#endif

std::string getTunnelURL() {
    std::lock_guard<std::mutex> lock(g_urlMtx);
    return g_url;
}

class DownloadProgressPopup : public Popup {
protected:
    cocos2d::CCLabelBMFont* m_percentLabel = nullptr;
    cocos2d::CCLayerColor* m_fillLayer = nullptr;
    float m_barWidth = 200.f;

    ~DownloadProgressPopup() override {
        g_downloadPopup = nullptr;
    }

    bool init(float w, float h) {
        log::info("DownloadProgressPopup::init starting");
        if (!Popup::init(w, h)) {
            log::error("Popup::init failed!");
            return false;
        }
        g_downloadPopup = this;

        this->setTitle("Downloading tunnel...");

        cocos2d::CCSize size = m_mainLayer->getContentSize();

        cocos2d::CCLayerColor* bgBar = cocos2d::CCLayerColor::create(
            cocos2d::ccColor4B{30, 30, 30, 150}, m_barWidth, 16.f
        );
        bgBar->setPosition(cocos2d::CCPoint{size.width / 2.f - m_barWidth / 2.f, 60.f});
        m_mainLayer->addChild(bgBar);

        m_fillLayer = cocos2d::CCLayerColor::create(
            cocos2d::ccColor4B{0, 200, 255, 255}, 0.f, 16.f
        );
        m_fillLayer->setPosition(cocos2d::CCPoint{size.width / 2.f - m_barWidth / 2.f, 60.f});
        m_mainLayer->addChild(m_fillLayer);

        m_percentLabel = cocos2d::CCLabelBMFont::create("0%", "goldFont.fnt");
        m_percentLabel->setScale(0.6f);
        m_percentLabel->setPosition(cocos2d::CCPoint{size.width / 2.f, 35.f});
        m_mainLayer->addChild(m_percentLabel);

        log::info("DownloadProgressPopup::init success");
        return true;
    }

public:
    void onClose(cocos2d::CCObject* sender) override {
        g_downloadTask.cancel();
        Popup::onClose(sender);
    }

    static DownloadProgressPopup* create() {
        log::info("DownloadProgressPopup::create starting");
        DownloadProgressPopup* ret = new DownloadProgressPopup();
        if (ret && ret->init(280.f, 140.f)) {
            ret->autorelease();
            log::info("DownloadProgressPopup::create success");
            return ret;
        }
        log::error("DownloadProgressPopup::create failed or ret is null!");
        CC_SAFE_DELETE(ret);
        return nullptr;
    }

    void setProgress(float percent) {
        if (m_fillLayer) {
            m_fillLayer->setContentSize(cocos2d::CCSize{m_barWidth * (percent / 100.f), 16.f});
        }
        if (m_percentLabel) {
            m_percentLabel->setString((std::to_string(static_cast<int>(percent)) + "%").c_str());
        }
    }
};

void checkAndDownloadCloudflared(bool forceReinstall) {
    log::info("checkAndDownloadCloudflared called with forceReinstall={}", forceReinstall);
    std::filesystem::path saveDir = Mod::get()->getSaveDir();
#ifdef GEODE_IS_WINDOWS
    std::filesystem::path cfPath = saveDir / "cloudflared.exe";
#elif defined(GEODE_IS_INTEL_MAC)
    std::filesystem::path cfPath = saveDir / "cloudflared-intel";
#else
    std::filesystem::path cfPath = saveDir / "cloudflared-mac";
#endif

    std::error_code ec;
    if (forceReinstall) {
        log::info("forceReinstall is true, deleting file if exists");
        g_downloadTask.cancel();
        if (g_downloadPopup) {
            g_downloadPopup->onClose(nullptr);
        }
        if (std::filesystem::exists(cfPath, ec)) {
            log::info("deleting {}", geode::utils::string::pathToString(cfPath));
            std::filesystem::remove(cfPath, ec);
            if (ec) {
                log::error("Failed to remove file: {}", ec.message());
            }
        }
    } else {
        log::info("forceReinstall is false, checking if exists");
        if (std::filesystem::exists(cfPath, ec)) {
            log::info("file already exists, returning");
            return;
        }
    }

    if (g_downloadTask.isPending()) {
        log::info("g_downloadTask is pending, returning");
        return;
    }

    std::error_code ec2;
    std::filesystem::create_directories(saveDir, ec2);

    log::info("attempting to create DownloadProgressPopup");
    DownloadProgressPopup* popup = DownloadProgressPopup::create();
    if (popup) {
        log::info("popup created successfully, calling show()");
        popup->show();
    } else {
        log::error("popup is nullptr!");
    }

    std::string url = "";
#ifdef GEODE_IS_WINDOWS
    url = "https://github.com/cloudflare/cloudflared/releases/download/2026.6.1/cloudflared-windows-amd64.exe";
#elif defined(GEODE_IS_INTEL_MAC)
    url = "https://github.com/axiom-S25u/RC-CF-Download/releases/download/67/cloudflared-intel";
#elif defined(GEODE_IS_MACOS)
    url = "https://github.com/axiom-S25u/RC-CF-Download/releases/download/67/cloudflared-mac";
#endif // on the offical gh releases it was a tgz i just unzipped and shoved in my repo

    if (url.empty()) return;

    geode::utils::web::WebRequest req;
    req.userAgent("RemoteControl/1.0.0");
    req.followRedirects(true);

    req.onProgress([](geode::utils::web::WebProgress const& prog) {
        std::optional<float> opt = prog.downloadProgress();
        if (opt.has_value()) {
            float percent = opt.value();
            geode::Loader::get()->queueInMainThread([percent]() {
                if (g_downloadPopup) {
                    g_downloadPopup->setProgress(percent);
                }
            });
        }
    });

    g_downloadTask.spawn(
        req.get(url),
        [cfPath, saveDir](geode::utils::web::WebResponse const& res) {
            if (!res.ok()) {
                log::error("Cloudflared download failed with code: {}", res.code());
                Notification::create("Download failed!", NotificationIcon::Error)->show();
                Loader::get()->queueInMainThread([]() {
                    if (g_downloadPopup) {
                        g_downloadPopup->onClose(nullptr);
                    }
                });
                return;
            }

            std::error_code ec;
            std::filesystem::create_directories(saveDir, ec);
            Result<> wrote = res.into(cfPath);
            if (wrote.isErr()) {
                log::error("Failed to write cloudflared: {}", wrote.unwrapErr());
                Notification::create("Failed to save cloudflared!", NotificationIcon::Error)->show();
            } else {
                log::info("Successfully downloaded cloudflared");
#ifndef GEODE_IS_WINDOWS
                std::filesystem::permissions(cfPath,
                    std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
                    std::filesystem::perm_options::add, ec);
#endif
                Notification::create("Tunnel binary downloaded!", NotificationIcon::Success)->show();
            }
            Loader::get()->queueInMainThread([]() {
                if (g_downloadPopup) {
                    g_downloadPopup->onClose(nullptr);
                }
            });
        }
    );
}