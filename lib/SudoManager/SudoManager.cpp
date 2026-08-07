#include "SudoManager.h"

#include "config.h"

#include <set>

namespace {

std::set<std::string> g_elevatedUsers;

} // namespace

namespace SudoManager {

bool elevate(const std::string& userId, const std::string& password) {
    if (userId.empty() || password.empty() || password != BoardConfig::SUDO_PASSWORD) {
        return false;
    }

    g_elevatedUsers.insert(userId);
    return true;
}

void revoke(const std::string& userId) {
    g_elevatedUsers.erase(userId);
}

bool isElevated(const std::string& userId) {
    return g_elevatedUsers.find(userId) != g_elevatedUsers.end();
}

size_t elevatedCount() {
    return g_elevatedUsers.size();
}

} // namespace SudoManager
