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
// this mod might be my coolest mod
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

        cocos2d::CCMenu* bottomMenu = typeinfo_cast<cocos2d::CCMenu*>(this->getChildByID("bottom-menu"));
        if (bottomMenu) {
            cocos2d::CCSprite* spr = cocos2d::CCSprite::create("logo.png"_spr);
            if (spr) {
                log::info("reinstall button created!");
                float scale = 48.f / spr->getContentSize().width;
                spr->setScale(scale);
                CCMenuItemSpriteExtra* btn = CCMenuItemSpriteExtra::create(
                    spr, this, menu_selector(RemoteMenuLayer::onReinstallTunnel)
                );
                btn->setID("reinstall-tunnel-btn"_spr);
                bottomMenu->addChild(btn);
                bottomMenu->updateLayout();
            } else {
                log::error("failed to create reinstall button sprite!");
            }
        }

        return true;
    }

    void onReinstallTunnel(cocos2d::CCObject* sender) {
        log::info("onReinstallTunnel clicked!");

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
                    if (btn2) {
                        checkAndDownloadCloudflared(true);
                    }
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
        CCLabelTTF* urlLabel = nullptr;
        bool holdingLeft = false;
        bool holdingRight = false;
        bool holdingJump = false;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        CCSize winSize = CCDirector::sharedDirector()->getWinSize();
        std::string currentUrl = getTunnelURL();
        CCLabelTTF* lbl = nullptr;
        if (!currentUrl.empty()) {
            lbl = CCLabelTTF::create(currentUrl.c_str(), "Arial", 10.0f);
        } else {
            lbl = CCLabelTTF::create(g_active ? "starting tunnel..." : "tunnel inactive", "Arial", 10.0f);
        }
        lbl->setPosition(ccp(winSize.width - 5.f, winSize.height - 10.f));
        lbl->setAnchorPoint(ccp(1.f, 1.f));
        lbl->setOpacity(180);

        int topZ = 0;
        for (CCNode* node : CCArrayExt<CCNode*>(this->getChildren())) {
            if (node->getZOrder() > topZ) topZ = node->getZOrder();
        }
        this->addChild(lbl, topZ + 1);
        lbl->setID("remote-url-label"_spr);
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

        for (std::string& action : inputs) {
            if (action == "jump_start") {
                if (!m_fields->holdingJump) {
                    GJBaseGameLayer::handleButton(true, (int)PlayerButton::Jump, true);
                    m_fields->holdingJump = true;
                }
            }
            else if (action == "jump_stop") {
                if (m_fields->holdingJump) {
                    GJBaseGameLayer::handleButton(false, (int)PlayerButton::Jump, true);
                    m_fields->holdingJump = false;
                }
            }
            else if (action == "left_start") {
                // left wins, cancel right if it was active
                if (m_fields->holdingRight) {
                    GJBaseGameLayer::handleButton(false, (int)PlayerButton::Right, true);
                    m_fields->holdingRight = false;
                }
                if (!m_fields->holdingLeft) {
                    GJBaseGameLayer::handleButton(true, (int)PlayerButton::Left, true);
                    m_fields->holdingLeft = true;
                }
            }
            else if (action == "left_stop") {
                // server only sends left_stop when ALL left holders released
                if (m_fields->holdingLeft) {
                    GJBaseGameLayer::handleButton(false, (int)PlayerButton::Left, true);
                    m_fields->holdingLeft = false;
                    // if right is still physically held by someone, re-apply
                    if (getHoldRight() > 0) {
                        GJBaseGameLayer::handleButton(true, (int)PlayerButton::Right, true);
                        m_fields->holdingRight = true;
                    }
                }
            }
            else if (action == "right_start") {
                // right wins, cancel left if it was active
                if (m_fields->holdingLeft) {
                    GJBaseGameLayer::handleButton(false, (int)PlayerButton::Left, true);
                    m_fields->holdingLeft = false;
                }
                if (!m_fields->holdingRight) {
                    GJBaseGameLayer::handleButton(true, (int)PlayerButton::Right, true);
                    m_fields->holdingRight = true;
                }
            }
            else if (action == "right_stop") {
                if (m_fields->holdingRight) {
                    GJBaseGameLayer::handleButton(false, (int)PlayerButton::Right, true);
                    m_fields->holdingRight = false;
                    // if left is still physically held by someone, re-apply
                    if (getHoldLeft() > 0) {
                        GJBaseGameLayer::handleButton(true, (int)PlayerButton::Left, true);
                        m_fields->holdingLeft = true;
                    }
                }
            }
            else if (action == "restart") {
                Loader::get()->queueInMainThread([this]() {
                    PlayLayer::resetLevel();
                });
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
        CCMenuItemSpriteExtra* thumbsUpBtn = nullptr;
        CCMenuItemLabel* linkBtn = nullptr;
        CCMenuItemSpriteExtra* copyBtn = nullptr;
        CCMenu* menu = nullptr;
        bool webhookSent = false;
    };

    void customSetup() {
        PauseLayer::customSetup();
        CCSize winSize = CCDirector::sharedDirector()->getWinSize();
        CCMenu* menu = CCMenu::create();
        menu->setPosition(ccp(40.f, 40.f));
        this->addChild(menu, 10);
        m_fields->menu = menu;

        std::string url = getTunnelURL();
        if (g_active && !url.empty()) {
            this->showLink(url);
        } else {
            this->showThumbsUp();
        }
    }

    void showThumbsUp() {
        if (m_fields->thumbsUpBtn) return;
        if (m_fields->linkBtn) {
            m_fields->linkBtn->removeFromParent();
            m_fields->linkBtn = nullptr;
        }
        if (m_fields->copyBtn) {
            m_fields->copyBtn->removeFromParent();
            m_fields->copyBtn = nullptr;
        }

        CCSprite* sprite = CCSprite::create("logo.png"_spr);
        if (sprite) {
            float scale = 32.f / sprite->getContentSize().width;
            sprite->setScale(scale);
        } else {
            sprite = CCSprite::createWithSpriteFrameName("GJ_likeBtn_001.png");
        }
        m_fields->thumbsUpBtn = CCMenuItemSpriteExtra::create(
            sprite, this, menu_selector(RemotePauseLayer::onStartTunnel)
        );
        m_fields->thumbsUpBtn->setPosition(ccp(0.f, 0.f));
        m_fields->menu->addChild(m_fields->thumbsUpBtn);
    }

    void showLink(std::string const& url) {
        if (m_fields->thumbsUpBtn) {
            m_fields->thumbsUpBtn->removeFromParent();
            m_fields->thumbsUpBtn = nullptr;
        }
        if (m_fields->linkBtn) {
            m_fields->linkBtn->removeFromParent();
            m_fields->linkBtn = nullptr;
        }
        if (m_fields->copyBtn) {
            m_fields->copyBtn->removeFromParent();
            m_fields->copyBtn = nullptr;
        }

        CCLabelTTF* label = CCLabelTTF::create(url.c_str(), "Arial", 12.0f);
        label->setColor(cocos2d::ccColor3B{0, 200, 255});
        m_fields->linkBtn = CCMenuItemLabel::create(
            label, this, menu_selector(RemotePauseLayer::onCopyLink)
        );
        m_fields->linkBtn->setAnchorPoint(ccp(0.f, 0.5f));
        m_fields->linkBtn->setPosition(ccp(0.f, 10.f));
        m_fields->menu->addChild(m_fields->linkBtn);

        CCLabelBMFont* copyLabel = CCLabelBMFont::create("Copy Link", "chatFont.fnt");
        copyLabel->setScale(0.6f);
        copyLabel->setColor(cocos2d::ccColor3B{255, 200, 0});
        m_fields->copyBtn = CCMenuItemSpriteExtra::create(
            copyLabel, this, menu_selector(RemotePauseLayer::onCopyLink)
        );
        m_fields->copyBtn->setAnchorPoint(ccp(0.f, 0.5f));
        m_fields->copyBtn->setPosition(ccp(0.f, -10.f));
        m_fields->menu->addChild(m_fields->copyBtn);
    }

    void onStartTunnel(CCObject* sender) {
        startServer();
        startTunnel();
        g_active = true;
        m_fields->webhookSent = false;

        this->showLink("starting tunnel...");

        PlayLayer* pl = PlayLayer::get();
        if (pl) {
            CCNode* lbl = pl->getChildByID("remote-url-label"_spr);
            if (lbl) {
                CCLabelTTF* label = typeinfo_cast<CCLabelTTF*>(lbl);
                if (label) {
                    label->setString("starting tunnel...");
                }
            }
        }

        this->schedule(schedule_selector(RemotePauseLayer::pollURLUpdate), 0.5f);
    }

    void pollURLUpdate(float dt) {
        std::string url = getTunnelURL();
        if (!url.empty()) {
            this->showLink(url);

            PlayLayer* pl = PlayLayer::get();
            if (pl) {
                CCNode* lbl = pl->getChildByID("remote-url-label"_spr);
                if (lbl) {
                    CCLabelTTF* label = typeinfo_cast<CCLabelTTF*>(lbl);
                    if (label) {
                        label->setString(url.c_str());
                    }
                }
            }

            // fire webhook once when url is first known
            if (!m_fields->webhookSent) {
                m_fields->webhookSent = true;
                sendWebhook(url);
            }

            this->unschedule(schedule_selector(RemotePauseLayer::pollURLUpdate));
        }
    }

    void onCopyLink(CCObject* sender) {
        std::string url = getTunnelURL();
        if (!url.empty()) {
            geode::utils::clipboard::write(url);
            Notification::create("Link copied!", NotificationIcon::Success)->show();
        } else {
            Notification::create("Tunnel starting, please wait...", NotificationIcon::Info)->show();
        }
    }
};