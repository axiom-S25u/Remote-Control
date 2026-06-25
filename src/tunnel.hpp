#pragma once
#include <string>
#include <mutex>

void startTunnel();
void stopTunnel();
std::string getTunnelURL();
void checkAndDownloadCloudflared(bool forceReinstall = false);
