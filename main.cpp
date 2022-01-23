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
#include <HCNetSDK.h>
#include <thread>


typedef int HikSessionId, HikEventListeningHandle, HikVoiceComHandle;

std::string ringtoneAudioFilePath;
HikSessionId sessionId;
HikVoiceComHandle voiceComHandle = -1;

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

std::string obtainHikSDKErrorMsg(const std::string& prefix = "HikSDK Error") {
    int errorCode = 0;
    char *errMsg = NET_DVR_GetErrorMsg(&errorCode);
    std::stringstream ss;
    ss << prefix << " | <" << errorCode << "> " << errMsg;
    return ss.str();
}

HikSessionId logInToDevice(
    const std::string& host,
    unsigned short port,
    const std::string& username,
    const std::string& password
) {
    PLOG_INFO << "Creating a session to a Hikvision device at "
              << username << ":" << password << "@" << host << ":" << port;
    bool initSuccessful = NET_DVR_Init();
    if (!initSuccessful) {
        shutdown("Failed to initialize Hik SDK.");
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
        shutdown(obtainHikSDKErrorMsg("Failed to log in to Hik device."));
    }
    PLOG_INFO << "Successfully logged in with session id <" << sid << ">";
    return sid;
}


void hikEventsCallback(LONG lCommand, NET_DVR_ALARMER *pAlarmer, char *pAlarmInfo, DWORD dwBufLen, void* pUser) {
    if (lCommand == COMM_ALARM_VIDEO_INTERCOM) {
        auto *videoIntercomAlarm = reinterpret_cast<NET_DVR_VIDEO_INTERCOM_ALARM *>(pAlarmInfo);
        PLOG_INFO << "Received Hik video intercom alarm: <" << (int) videoIntercomAlarm->byAlarmType << ">";
        if (videoIntercomAlarm->byAlarmType == 0x11) {
            // The bell button was pressed
        }
    } else {
        PLOG_INFO << "Received Hik device event <" << lCommand << ">.";
    }
}

HikEventListeningHandle registerForHikEvents() {
    PLOG_INFO << "Registering for Hikvision events on session id <" << sessionId << ">";

    NET_DVR_SETUPALARM_PARAM setupParam;
    setupParam.dwSize = sizeof(NET_DVR_SETUPALARM_PARAM);
    setupParam.byAlarmInfoType = 1; // Real-time alarm
    setupParam.byLevel = 2; // Priority

    HikEventListeningHandle handle = NET_DVR_SetupAlarmChan_V41(sessionId, &setupParam);
    if (handle < 0) {
        shutdown(obtainHikSDKErrorMsg("Failed to register for events for Hik device."));
    }
    PLOG_INFO << "Successfully registered for receiving Hik device events with handle <" << handle << ">";
    return handle;
}

HikVoiceComHandle startVoiceCommunications() {
    PLOG_INFO << "Starting voice communications on session id <" << sessionId << ">";

    voiceComHandle = NET_DVR_StartVoiceCom_MR_V30(
        sessionId,
        1,
        /*hikVoiceCommunicationsCallback*/NULL,
        nullptr
    );
    if (voiceComHandle < 0) {
        shutdown(obtainHikSDKErrorMsg("Failed to establish voice comms."));
    }
    PLOG_INFO << "Successfully started voice communications with handle <" << voiceComHandle << ">";
    return voiceComHandle;
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
            "t,ringtone-audio",
            "Path to file to use as ringtone audio",
                cxxopts::value<std::string>()->default_value("")
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
        ringtoneAudioFilePath = result["ringtone-audio"].as<std::string>();

    } catch (const cxxopts::option_has_no_value_exception& e) {
        shutdown(std::make_optional(e.what()));
    }

    sessionId = logInToDevice(
        deviceHost,
        devicePort,
        deviceUsername,
        devicePassword
    );

    NET_DVR_COMPRESSION_AUDIO audioSettings = { 0 };
    audioSettings.byAudioEncType = 1;
    audioSettings.byAudioSamplingRate = 5;
    audioSettings.byAudioBitRate = BITRATE_ENCODE_128kps;
    audioSettings.bySupport = 0;
    if (!NET_DVR_SetDVRConfig(
        sessionId,
        NET_DVR_SET_COMPRESSCFG_AUD,
        1,
        &audioSettings,
        sizeof(audioSettings)
    )) {
        shutdown(obtainHikSDKErrorMsg("Failed to set audio settings"));
    } else {
        PLOG_INFO << "Successfully set Hik device audio settings.";
    }

    NET_DVR_SetDVRMessageCallBack_V50(0, &hikEventsCallback, nullptr);

    HikEventListeningHandle eventLHandle = registerForHikEvents();

    sleep(20);

    shutdown();
}






