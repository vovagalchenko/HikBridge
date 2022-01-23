#include <iostream>

#include <plog/Log.h>
#include <plog/Record.h>
#include <plog/Initializers/RollingFileInitializer.h>
#include <plog/Appenders/ColorConsoleAppender.h>
#include <cxxopts.hpp>
#include <optional>
#include <functional>
#include <backward.hpp>
#include <utility>
#include "HikDeviceSession.h"


void shutdown(std::optional<std::string> errorMessage = std::nullopt) {
    if (auto msg = std::move(errorMessage)) {
        backward::StackTrace st;
        st.load_here();
        backward::Printer p;
        p.snippet = true;
        std::ostringstream btStream;
        p.print(st, btStream);
        PLOG_FATAL << "HikBridge shutting down due to error: " << std::endl << *msg << std::endl << btStream.str();
        exit(1);
    } else {
        PLOG_INFO << "HikBridge shutting down.";
        exit(0);
    }
}

int main(int argc, char** argv) {

    static plog::ColorConsoleAppender<plog::TxtFormatter> consoleAppender;
    plog::init(
        plog::verbose,
        "var/log/hikbridge/",
        10u * (1u << 30u), // 10MB
        100
    ).addAppender(&consoleAppender);

    PLOG_INFO << "HikBridge starting up...";

    cxxopts::Options options("HikBridge", "Hikbridge connects HikVision intercoms to Homebridge.");
    options.add_options()
        (
            "h,device-host",
            "The address of the Hikvision device we're connecting",
            cxxopts::value<std::string>()
        )
        (
            "r,device-port",
            "The port on the Hikvision device we're connecting to",
            cxxopts::value<unsigned short>()
        )
        (
            "u,device-username",
            "Username to use when connecting to the Hikvision device",
            cxxopts::value<std::string>()->default_value("admin")
        )
        (
            "p,device-password",
            "Password to use when connecting to the Hikvision device",
            cxxopts::value<std::string>()
        )
        (
            "s,soundcard-coordinates",
            "The ALSA name of the soundcard to read mu-law sound signal from",
            cxxopts::value<std::string>()
        );

    std::string deviceHost, deviceUsername, devicePassword, soundcardCoordinates;
    unsigned short devicePort;
    try {
        auto result = options.parse(argc, argv);
        deviceHost = result["device-host"].as<std::string>();
        devicePort = result["device-port"].as<unsigned short>();
        deviceUsername = result["device-username"].as<std::string>();
        devicePassword = result["device-password"].as<std::string>();
        soundcardCoordinates = result["soundcard-coordinates"].as<std::string>();
    } catch (const cxxopts::option_has_no_value_exception& e) {
        shutdown(std::make_optional(e.what()));
    }




    try {
        HikDeviceSession deviceSession = HikDeviceSession(
            deviceHost,
            devicePort,
            deviceUsername,
            devicePassword
        );
        deviceSession.subscribeToEvents([](HikDeviceSessionEvent e) {
            PLOG_INFO << "Received event: " << e;
        });

    } catch (const HikDeviceSessionException& e) {
        shutdown(std::make_optional(e.what()));
    }

    shutdown();
}

long logInToDevice(std::string host, unsigned short port, std::string username, std::string password) {
    PLOG_INFO << "Creating a session to a Hikvision device at "
              << username << ":" << password << "@" << host << ":" << port;
    bool initSuccessful = NET_DVR_Init();
    if (!initSuccessful) {
        throw HikDeviceSessionException("Failed to initialize.");
    }

    NET_DVR_SetConnectTime(2000, 1);
    NET_DVR_SetReconnect(10000, true);

    NET_DVR_USER_LOGIN_INFO loginInfo;
    loginInfo.bUseAsynLogin = 0;
    loginInfo.wPort = 8000;
    strcpy(loginInfo.sDeviceAddress, host.c_str());
    strcpy(loginInfo.sUserName, username.c_str());
    strcpy(loginInfo.sPassword, password.c_str());

    NET_DVR_DEVICEINFO_V40 deviceInfoV40 = {0};
    int sid = NET_DVR_Login_V40(&loginInfo, &deviceInfoV40);

    if (sid < 0) {
        int errorCode = 0;
        char *errMsg = NET_DVR_GetErrorMsg(&errorCode);
        std::stringstream ss;
        ss << "Failed to log in due to error <" << errorCode << "> - " << errMsg;
        throw HikDeviceSessionException(ss.str());
    } else {
        PLOG_INFO << "Successfully logged in with session id <" << sid << ">";
        return
    }
}




