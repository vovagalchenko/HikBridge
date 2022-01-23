//
// Created by Vova Galchenko on 1/20/22.
//

#include "HikDeviceSession.h"
#include "hik-sdk-include/HCNetSDK.h"

HikDeviceSession::HikDeviceSession(std::string host, int port, std::string username, std::string password) {

}

void HikDeviceSession::subscribeToEvents(const HikDeviceEventCallback callback) {
    if (registeredCallback != NULL) {
        throw HikDeviceSessionException("HikDeviceSession doesn't support more than one callback registered.");
    }
    registeredCallback = callback;
    callback = callback;
    NET_DVR_SetDVRMessageCallBack_V50(0, NULL, NULL);
}

void HikDeviceSession::deviceCallback(LONG lCommand, NET_DVR_ALARMER *pAlarmer, char *pAlarmInfo, DWORD dwBufLen, void* pUser) const {
    PLOG_INFO << "HELLO";
}

const char *HikDeviceSessionException::what() const noexcept {
    std::string str = "Hikvision Device Session Exception: " + msg;
    return strcpy(new char[str.length() + 1], str.c_str());
}