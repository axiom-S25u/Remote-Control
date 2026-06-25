#pragma once
#include <string>
#include <vector>
#include <queue>
#include <mutex>

void startServer();
void stopServer();
void queueInput(std::string action);
void drainInputs(std::vector<std::string>& out);
int getHoldLeft();
int getHoldRight();
