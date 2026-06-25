#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/BasedButtonSprite.hpp>
#include <Geode/utils/web.hpp>
#include <filesystem>
#include "server.hpp"
#include "tunnel.hpp"

using namespace geode::prelude;

static geode::async::TaskHolder<geode::utils::web::WebResponse> g_webhookTask;

static void sendWebhook(std::string const& url) {
    std::string webhookUrl = Mod::get()->getSettingValue<std::string>("webhook-url");
    if (webhookUrl.empty()) return;

    std::string body = "{\"content\":\"Remote Control tunnel URL: " + url + "\"}";

    g_webhookTask.spawn(
        geode::utils::web::WebRequest()
            .userAgent("RemoteControl/1.0.0")
            .header("Content-Type", "application/json")
            .bodyString(body)
            .post(webhookUrl),
        [](geode::utils::web::WebResponse res) {
            if (!res.ok()) {
                log::warn("webhook failed with code: {}", res.code());
            } else {
                log::info("webhook sent successfully");
            }
        }
    );
}

class $modify(RemoteMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        Loader::get()->queueInMainThread([]() {
            checkAndDownloadCloudflared(false);
        });

        cocos2d::CCSpriteFrameCache::sharedSpriteFrameCache()->addSpriteFramesWithFile("GJ_GameSheet.plist");
        cocos2d::CCSpriteFrameCache::sharedSpriteFrameCache()->addSpriteFramesWithFile("GJ_GameSheet02.plist");

        auto bottomMenu = typeinfo_cast<cocos2d::CCMenu*>(this->getChildByID("bottom-menu"));
        if (!bottomMenu) return true;

        auto spr = cocos2d::CCSprite::create("logo.png"_spr);
        if (!spr) {
            log::error("failed to create reinstall button sprite!");
            return true;
        }

        float scale = 48.f / spr->getContentSize().width;
        spr->setScale(scale);

        auto btn = CCMenuItemSpriteExtra::create(
            spr, this, menu_selector(RemoteMenuLayer::onReinstallTunnel)
        );
        btn->setID("reinstall-tunnel-btn"_spr);
        bottomMenu->addChild(btn);
        bottomMenu->updateLayout();

        return true;
    }

    void onReinstallTunnel(cocos2d::CCObject*) {
        std::filesystem::path cfPath = Mod::get()->getSaveDir() /
#ifdef GEODE_IS_WINDOWS
            "cloudflared.exe";
#elif defined(GEODE_IS_INTEL_MAC)
            "cloudflared-intel";
#else
            "cloudflared-mac";
#endif
        std::error_code ec;
        if (std::filesystem::exists(cfPath, ec)) {
            createQuickPopup(
                "Reinstall?",
                "You already installed it, reinstall?",
                "Cancel", "Reinstall",
                [](FLAlertLayer*, bool btn2) {
                    if (btn2) checkAndDownloadCloudflared(true);
                }
            );
            return;
        }

        checkAndDownloadCloudflared(true);
    }
};

static bool g_active = false;

class $modify(RemotePlayLayer, PlayLayer) {
    struct Fields {
        CCLabelBMFont* urlLabel = nullptr;
        bool holdingLeft = false;
        bool holdingRight = false;
        bool holdingJump = false;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        CCSize winSize = CCDirector::sharedDirector()->getWinSize();
        std::string currentUrl = getTunnelURL();

        auto lbl = CCLabelBMFont::create(
            (!currentUrl.empty() ? currentUrl.c_str() : (g_active ? "starting tunnel..." : "tunnel inactive")),
            "bigFont.fnt"
        );
        lbl->setScale(0.25f);
        lbl->setPosition(ccp(winSize.width - 5.f, winSize.height - 10.f));
        lbl->setAnchorPoint(ccp(1.f, 1.f));
        lbl->setOpacity(180);
        lbl->setID("remote-url-label"_spr);

        int topZ = 0;
        for (auto node : CCArrayExt<CCNode*>(this->getChildren())) {
            if (node->getZOrder() > topZ) topZ = node->getZOrder();
        }
        this->addChild(lbl, topZ + 1);
        m_fields->urlLabel = lbl;

        this->schedule(schedule_selector(RemotePlayLayer::pollInputs), 0.f);
        this->schedule(schedule_selector(RemotePlayLayer::pollURL), 1.0f);

        return true;
    }

    void pollURL(float dt) {
        if (!m_fields->urlLabel) return;
        std::string url = getTunnelURL();
        if (!url.empty()) {
            m_fields->urlLabel->setString(url.c_str());
        }
    }

    void pollInputs(float dt) {
        if (!g_active) return;

        std::vector<std::string> inputs;
        drainInputs(inputs);

        for (auto& action : inputs) {
            if (action == "jump_start") {
                if (!m_fields->holdingJump) {
                    GJBaseGameLayer::handleButton(true, (int)PlayerButton::Jump, true);
                    m_fields->holdingJump = true;
                }
            } else if (action == "jump_stop") {
                if (m_fields->holdingJump) {
                    GJBaseGameLayer::handleButton(false, (int)PlayerButton::Jump, true);
                    m_fields->holdingJump = false;
                }
            } else if (action == "left_start") {
                if (m_fields->holdingRight) {
                    GJBaseGameLayer::handleButton(false, (int)PlayerButton::Right, true);
                    m_fields->holdingRight = false;
                }
                if (!m_fields->holdingLeft) {
                    GJBaseGameLayer::handleButton(true, (int)PlayerButton::Left, true);
                    m_fields->holdingLeft = true;
                }
            } else if (action == "left_stop") {
                if (m_fields->holdingLeft) {
                    GJBaseGameLayer::handleButton(false, (int)PlayerButton::Left, true);
                    m_fields->holdingLeft = false;
                    if (getHoldRight() > 0) {
                        GJBaseGameLayer::handleButton(true, (int)PlayerButton::Right, true);
                        m_fields->holdingRight = true;
                    }
                }
            } else if (action == "right_start") {
                if (m_fields->holdingLeft) {
                    GJBaseGameLayer::handleButton(false, (int)PlayerButton::Left, true);
                    m_fields->holdingLeft = false;
                }
                if (!m_fields->holdingRight) {
                    GJBaseGameLayer::handleButton(true, (int)PlayerButton::Right, true);
                    m_fields->holdingRight = true;
                }
            } else if (action == "right_stop") {
                if (m_fields->holdingRight) {
                    GJBaseGameLayer::handleButton(false, (int)PlayerButton::Right, true);
                    m_fields->holdingRight = false;
                    if (getHoldLeft() > 0) {
                        GJBaseGameLayer::handleButton(true, (int)PlayerButton::Left, true);
                        m_fields->holdingLeft = true;
                    }
                }
            }
        }
    }

    void onQuit() {
        g_active = false;
        this->unschedule(schedule_selector(RemotePlayLayer::pollInputs));
        this->unschedule(schedule_selector(RemotePlayLayer::pollURL));
        stopServer();
        stopTunnel();
        m_fields->urlLabel = nullptr;
        PlayLayer::onQuit();
    }
};

class $modify(RemotePauseLayer, PauseLayer) {
    struct Fields {
        bool webhookSent = false;
    };

    void customSetup() {
        PauseLayer::customSetup();

        auto leftMenu = typeinfo_cast<CCMenu*>(this->getChildByID("left-button-menu"));
        if (!leftMenu) return;

        auto spr = CCSprite::create("logo.png"_spr);
        if (!spr) spr = CCSprite::createWithSpriteFrameName("GJ_likeBtn_001.png");

        float scale = 32.f / spr->getContentSize().width;
        spr->setScale(scale);

        auto btn = CCMenuItemSpriteExtra::create(
            spr, this, menu_selector(RemotePauseLayer::onToggleTunnel)
        );
        btn->setID("remote-control-btn"_spr);
        leftMenu->addChild(btn);
        leftMenu->updateLayout();
    }

    void onToggleTunnel(CCObject*) {
        if (g_active) {
            std::string url = getTunnelURL();
            if (!url.empty()) {
                geode::utils::clipboard::write(url);
                Notification::create("Link copied!", NotificationIcon::Success)->show();
            } else {
                Notification::create("Tunnel starting, please wait...", NotificationIcon::Info)->show();
            }
            return;
        }

        startServer();
        startTunnel();
        g_active = true;
        m_fields->webhookSent = false;

        if (auto pl = PlayLayer::get()) {
            if (auto lbl = typeinfo_cast<CCLabelBMFont*>(pl->getChildByID("remote-url-label"_spr))) {
                lbl->setString("starting tunnel...");
            }
        }

        this->schedule(schedule_selector(RemotePauseLayer::pollURLUpdate), 0.5f);
    }

    void pollURLUpdate(float dt) {
        std::string url = getTunnelURL();
        if (url.empty()) return;

        if (auto pl = PlayLayer::get()) {
            if (auto lbl = typeinfo_cast<CCLabelBMFont*>(pl->getChildByID("remote-url-label"_spr))) {
                lbl->setString(url.c_str());
            }
        }

        if (!m_fields->webhookSent) {
            m_fields->webhookSent = true;
            sendWebhook(url);
        }

        this->unschedule(schedule_selector(RemotePauseLayer::pollURLUpdate));
    }
};