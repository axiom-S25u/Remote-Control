#include "server.hpp"
#include <httplib.h>
#include <Geode/Geode.hpp>
#include <thread>
#include <chrono>

using namespace geode::prelude;

static std::mutex g_mtx;
static std::queue<std::string> g_queue;
static httplib::Server* g_srv = nullptr;
static std::thread g_thread;

static int g_holdLeft = 0;
static int g_holdRight = 0;
static int g_holdJump = 0;

static std::chrono::steady_clock::time_point g_lastHeartbeat;
static bool g_heartbeatActive = false;
static std::thread g_watchdogThread;
static bool g_watchdogRunning = false;

static const char* CONTROLLER_HTML = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>controller</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{background:#0f0f0f;color:#e0e0e0;font-family:system-ui,sans-serif;height:100vh;display:flex;flex-direction:column;align-items:center;justify-content:center;-webkit-user-select:none;user-select:none;touch-action:manipulation}
.row{display:flex;gap:12px;margin:8px 0}
button{background:#1a1a1a;color:#e0e0e0;border:1px solid #333;border-radius:8px;font-size:18px;padding:20px 32px;min-width:90px;cursor:pointer;-webkit-tap-highlight-color:transparent;transition:background 0.08s}
button.held{background:#333;border-color:#888}
.jump{padding:28px 48px;font-size:22px;min-width:140px}
.dir{padding:24px 36px;min-width:100px}
.small{padding:16px 24px;font-size:14px;min-width:80px}
</style>
</head>
<body>
<div class="row"><button class="jump" id="btn-jump" ontouchstart="hold('jump',this)" ontouchend="release('jump',this)" ontouchcancel="release('jump',this)" onmousedown="hold('jump',this)" onmouseup="release('jump',this)" onmouseleave="release('jump',this)">Jump</button></div>
<div class="row">
  <button class="dir" id="btn-left" ontouchstart="hold('left',this)" ontouchend="release('left',this)" ontouchcancel="release('left',this)" onmousedown="hold('left',this)" onmouseup="release('left',this)" onmouseleave="release('left',this)">Left</button>
  <button class="dir" id="btn-right" ontouchstart="hold('right',this)" ontouchend="release('right',this)" ontouchcancel="release('right',this)" onmousedown="hold('right',this)" onmouseup="release('right',this)" onmouseleave="release('right',this)">Right</button>
</div>
<div class="row"><button class="small" ontouchstart="send('restart')" onmousedown="send('restart')">Restart</button></div>
<script>
var held={jump:false,left:false,right:false};
var hbTimer=null;

function send(a){fetch('/input',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action:a})}).catch(function(){})}

function sendHeartbeat(){
  fetch('/heartbeat',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({jump:held.jump,left:held.left,right:held.right})}).catch(function(){});
}

function startHeartbeat(){
  if(hbTimer)return;
  sendHeartbeat();
  hbTimer=setInterval(sendHeartbeat,80);
}

function stopHeartbeat(){
  if(hbTimer){clearInterval(hbTimer);hbTimer=null;}
  sendHeartbeat();
}

function anyHeld(){return held.jump||held.left||held.right;}

function hold(a,el){
  if(held[a])return;
  held[a]=true;
  el.classList.add('held');
  send(a+'_start');
  startHeartbeat();
}

function release(a,el){
  if(!held[a])return;
  held[a]=false;
  el.classList.remove('held');
  send(a+'_stop');
  if(!anyHeld())stopHeartbeat();
}

function releaseAll(){
  ['jump','left','right'].forEach(function(a){
    var el=document.getElementById('btn-'+a);
    if(el&&held[a]){held[a]=false;el.classList.remove('held');send(a+'_stop');}
  });
  stopHeartbeat();
}

document.addEventListener('touchend',function(){if(!anyHeld())releaseAll();});
document.addEventListener('touchcancel',function(){releaseAll();});
window.addEventListener('blur',function(){releaseAll();});
window.addEventListener('visibilitychange',function(){if(document.hidden)releaseAll();});
</script>
</body>
</html>
)rawhtml";
// i mean, do you NEED more than this?

void queueInput(std::string action) {
    std::lock_guard<std::mutex> lock(g_mtx);
    g_queue.push(action);
}

void drainInputs(std::vector<std::string>& out) {
    std::lock_guard<std::mutex> lock(g_mtx);
    while (!g_queue.empty()) {
        out.push_back(g_queue.front());
        g_queue.pop();
    }
}

int getHoldLeft() {
    std::lock_guard<std::mutex> lock(g_mtx);
    return g_holdLeft;
}

int getHoldRight() {
    std::lock_guard<std::mutex> lock(g_mtx);
    return g_holdRight;
}

static void forceReleaseAll() {
    // called with g_mtx already held
    if (g_holdJump > 0) {
        g_holdJump = 0;
        g_queue.push("jump_stop");
    }
    if (g_holdLeft > 0) {
        g_holdLeft = 0;
        g_queue.push("left_stop");
    }
    if (g_holdRight > 0) {
        g_holdRight = 0;
        g_queue.push("right_stop");
    }
}

void startServer() {
    if (g_srv) return;

    g_lastHeartbeat = std::chrono::steady_clock::now();
    g_heartbeatActive = false;

    g_srv = new httplib::Server();

    g_srv->Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(CONTROLLER_HTML, "text/html");
    });

    g_srv->Post("/input", [](const httplib::Request& req, httplib::Response& res) {
        std::string body = req.body;
        if (body.empty()) {
            res.status = 400;
            return;
        }

        matjson::Value parsed = matjson::parse(body).unwrapOrDefault();
        if (parsed.contains("action") && parsed["action"].isString()) {
            std::string action = parsed["action"].asString().unwrapOrDefault();
            if (!action.empty()) {
                std::lock_guard<std::mutex> lock(g_mtx);
                if (action == "left_start") {
                    g_holdLeft++;
                    g_queue.push("left_start");
                } else if (action == "left_stop") {
                    if (g_holdLeft > 0) g_holdLeft--;
                    if (g_holdLeft == 0) g_queue.push("left_stop");
                } else if (action == "right_start") {
                    g_holdRight++;
                    g_queue.push("right_start");
                } else if (action == "right_stop") {
                    if (g_holdRight > 0) g_holdRight--;
                    if (g_holdRight == 0) g_queue.push("right_stop");
                } else if (action == "jump_start") {
                    g_holdJump++;
                    g_queue.push("jump_start");
                } else if (action == "jump_stop") {
                    if (g_holdJump > 0) g_holdJump--;
                    if (g_holdJump == 0) g_queue.push("jump_stop");
                } else {
                    // restart etc
                    g_queue.push(action);
                }
            }
        }
        res.set_content("{\"ok\":true}", "application/json");
    });

    g_srv->Post("/heartbeat", [](const httplib::Request& req, httplib::Response& res) {
        matjson::Value parsed = matjson::parse(req.body).unwrapOrDefault();
        bool wantJump  = parsed.contains("jump")  && parsed["jump"].isBool()  && parsed["jump"].asBool().unwrapOr(false);
        bool wantLeft  = parsed.contains("left")  && parsed["left"].isBool()  && parsed["left"].asBool().unwrapOr(false);
        bool wantRight = parsed.contains("right") && parsed["right"].isBool() && parsed["right"].asBool().unwrapOr(false);

        std::lock_guard<std::mutex> lock(g_mtx);
        g_lastHeartbeat = std::chrono::steady_clock::now();
        g_heartbeatActive = true;

        if (!wantJump && g_holdJump > 0) {
            g_holdJump = 0;
            g_queue.push("jump_stop");
        }
        if (!wantLeft && g_holdLeft > 0) {
            g_holdLeft = 0;
            g_queue.push("left_stop");
        }
        if (!wantRight && g_holdRight > 0) {
            g_holdRight = 0;
            g_queue.push("right_stop");
        }

        res.set_content("{\"ok\":true}", "application/json");
    });

    g_watchdogRunning = true;
    g_watchdogThread = std::thread([]() {
        while (g_watchdogRunning) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            std::lock_guard<std::mutex> lock(g_mtx);
            if (!g_heartbeatActive) continue;

            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastHeartbeat).count();
            if (elapsed > 250) {
                forceReleaseAll();
                g_heartbeatActive = false;
            }
        }
    });

    g_thread = std::thread([]() {
        log::info("starting http server on port 8080");
        g_srv->listen("0.0.0.0", 8080);
        log::info("http server stopped");
    });
}

void stopServer() {
    g_watchdogRunning = false;
    if (g_watchdogThread.joinable()) {
        g_watchdogThread.join();
    }

    if (g_srv) {
        g_srv->stop();
        if (g_thread.joinable()) {
            g_thread.join();
        }
        delete g_srv;
        g_srv = nullptr;
    }

    std::lock_guard<std::mutex> lock(g_mtx);
    while (!g_queue.empty()) g_queue.pop();
    g_holdLeft = 0;
    g_holdRight = 0;
    g_holdJump = 0;
    g_heartbeatActive = false;
}
