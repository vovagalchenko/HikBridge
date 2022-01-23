#include <utility>

//
// Created by Vova Galchenko on 1/20/22.
//

#ifndef HIKBRIDGE_HIKDEVICESESSION_H
#define HIKBRIDGE_HIKDEVICESESSION_H

#include <functional>
#include <HCNetSDK.h>

enum HikDeviceSessionEvent {
    button_pressed, unknown
};

typedef std::function<void(const HikDeviceSessionEvent)> HikDeviceEventCallback;

class HikDeviceSession {
public:
    HikDeviceSession(std::string host, int port, std::string username, std::string password);
    void subscribeToEvents(HikDeviceEventCallback callback);

private:
    int sessionId;
    HikDeviceEventCallback registeredCallback;
    void deviceCallback(LONG lCommand, NET_DVR_ALARMER *pAlarmer, char *pAlarmInfo, DWORD dwBufLen, void* pUser) const;
};




struct HikDeviceSessionException : public std::exception {
public:
    explicit HikDeviceSessionException(std::string message): msg(std::move(message)) {}
    const char *what() const noexcept override;

private:
    const std::string msg;

};


#endif //HIKBRIDGE_HIKDEVICESESSION_H
