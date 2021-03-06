#pragma clang diagnostic push
#pragma ide diagnostic ignored "misc-no-recursion"
#include <iostream>

#include <plog/Log.h>
#include <plog/Record.h>
#include <plog/Initializers/RollingFileInitializer.h>
#include <plog/Appenders/ColorConsoleAppender.h>
#include <cxxopts.hpp>
#include <optional>
#include <backward.hpp>
#include <utility>
#include <HCNetSDK.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include "cpp-httplib/httplib.h"
#ifdef REMOTE
    #include <alsa/asoundlib.h>
#else
    #include "alsa-lib-1.2.6.1/include/asoundlib.h"
#endif


#define MILLIS_OF_SILENCE_BEFORE_HANGUP 5000
typedef int HikSessionId, HikEventListeningHandle, HikVoiceComHandle;

HikSessionId sessionId;
char soundcardReadBuffer[160];
std::mutex soundcardReadBufferMutex;
std::mutex voiceComHandleMutex;
std::condition_variable soundcardReadBufferCV;
std::string doorbellHost;
unsigned short doorbellPort;
std::string doorbellPath;
HikVoiceComHandle voiceComHandle = -1;
long lastSoundcardLoopTime;
bool intercomGotFuckedWith;

void shutdown(std::stringstream &stream) {
    backward::StackTrace st;
    st.load_here();
    backward::Printer p;
    p.snippet = true;
    std::ostringstream btStream;
    p.print(st, btStream);
    PLOG_FATAL << "HikBridge shutting down due to error: " << std::endl << stream.str() << std::endl << btStream.str();
    exit(1);
}

void shutdown(std::optional<std::string> errorMessage = std::nullopt) {
    if (auto msg = std::move(errorMessage)) {
        std::stringstream ss;
        ss << *msg;
        shutdown(ss);
    } else {
        PLOG_INFO << "HikBridge shutting down gracefully.";
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

std::optional<std::string> checkAlsaError(int errCode) {
    if (errCode < 0) {
        std::stringstream alsaErrStream;
        alsaErrStream << "ALSA ERROR CODE | <" << errCode << "> ??? " << snd_strerror(errCode);
        return alsaErrStream.str();
    }
    return std::nullopt;
}

void recoverPcm(snd_pcm_t *handle, int errCode) {
    if (errCode == -EPIPE) {
        PLOG_WARNING << "Experiencing xrun.";
        snd_pcm_status_t *status;
        snd_pcm_status_alloca(&status);
        if (auto pcmStatusErrMsg = checkAlsaError(snd_pcm_status(handle, status))) {
            std::stringstream ss;
            ss << "Failed to get PCM status after xrun: " << *pcmStatusErrMsg;
            shutdown(ss);
        }
        snd_pcm_status_get_state(status);
        snd_output_t *statusOutput;
        snd_output_buffer_open(&statusOutput);
        snd_pcm_status_dump(status, statusOutput);
        char *buff;
        snd_output_buffer_string(statusOutput, &buff);
        PLOG_WARNING << "PCM status: " << std::endl << buff;
        snd_output_close(statusOutput);

        if (auto recoverErrorMsg = checkAlsaError(
            snd_pcm_recover(handle, (int) errCode, 0)
        )) {
            std::stringstream ss;
            ss << "Failed to recover after xrun: " << *recoverErrorMsg;
            shutdown(ss);
        } else {
            PLOG_WARNING << "Recovered seemingly successfully.";
        }
    } else if (auto errMsg = checkAlsaError(errCode)){
        shutdown(*errMsg);
    }
}

bool hikRelayEnabled = false;
void hikVoiceCommunicationsCallback(
        HikVoiceComHandle lVoiceComHandle,
        char *pRecvDataBuffer,
        DWORD dwBufSize,
        [[maybe_unused]] BYTE byAudioFlag,
        [[maybe_unused]] void* pUser
) {
    assert(dwBufSize == sizeof(soundcardReadBuffer));
    if (!hikRelayEnabled) {
        PLOG_INFO << "Hik relay is disabled, so we're going to short circuit the mutex/CV dance.";
        return;
    }
    std::unique_lock<std::mutex> soundcardReadBufferLock(soundcardReadBufferMutex);
    soundcardReadBufferCV.notify_one();
    soundcardReadBufferCV.wait(soundcardReadBufferLock);
    memcpy(pRecvDataBuffer, soundcardReadBuffer, dwBufSize);
    soundcardReadBufferLock.unlock();
    soundcardReadBufferCV.notify_one();

    if (!hikRelayEnabled) {
        PLOG_INFO << "Hik relay is disabled, so we're going to short circuit the voice comm call.";
        return;
    }

    if (NET_DVR_VoiceComSendData(lVoiceComHandle, pRecvDataBuffer, dwBufSize)) {
        PLOG_DEBUG << "Successfully sent " << dwBufSize << " bytes of audio to the Hik device.";
    } else {
        PLOG_WARNING << obtainHikSDKErrorMsg("Failed sending audio to the Hik device.");
    }
}

void startVoiceCommunications(bool restart = false, unsigned short retryNum = 1) {
    std::unique_lock<std::mutex> lk(voiceComHandleMutex);
    if (restart && voiceComHandle < 0) {
        PLOG_INFO << "No voice comms to restart. Abandoning...";
        return;
    } else if (restart) {
        PLOG_INFO << "Restarting voice comms...";
    } else {
        PLOG_INFO << "Starting voice communications on session id <" << sessionId << ">";
    }

    HikVoiceComHandle voiceComHandleCandidate = NET_DVR_StartVoiceCom_MR_V30(
            sessionId,
            1,
            hikVoiceCommunicationsCallback,
            nullptr
    );
    if (voiceComHandleCandidate < 0) {
        PLOG_ERROR << obtainHikSDKErrorMsg("Failed to establish voice comms.");
        if (retryNum < 4) {
            lk.unlock();
            PLOG_WARNING << "Retry num " << retryNum;
            return startVoiceCommunications(restart, retryNum + 1);
        } else {
            shutdown(obtainHikSDKErrorMsg("Failed to establish voice comms."));
        }
    } else {
        PLOG_INFO << "Successfully started voice communications with handle <" << voiceComHandleCandidate << ">";
    }
    voiceComHandle = voiceComHandleCandidate;
}

void callDoorbell(int retryNum = 0) {
    if (retryNum > 0) {
        PLOG_WARNING << "Doorbell call retry number " << retryNum;
    }
    std::stringstream ss;
    ss << "http://" << doorbellHost << ":" << doorbellPort;
    httplib::Client doorbellHttpCall(ss.str());
    doorbellHttpCall.set_url_encode(true);

    PLOG_INFO << "Notifying doorbell service @ " << ss.str() << doorbellPath.c_str();
    auto res = doorbellHttpCall.Get(doorbellPath.c_str());
    PLOG_INFO << "Received result status: " << res->status;
    if (res->status >= 300 && retryNum < 3) {
        PLOG_WARNING << "The result is unexpected. Retrying...";
        callDoorbell(retryNum + 1);
    } else if (res->status >= 300) {
        PLOG_ERROR << "Exhausted retries, but unable to make the doorbell HTTP callback :(";
    } else {
        PLOG_INFO << "Doorbell callback was successful";
    }

}

void hikEventsCallback(
    LONG lCommand,
    [[maybe_unused]] NET_DVR_ALARMER *pAlarmer,
    char *pAlarmInfo,
    [[maybe_unused]] DWORD dwBufLen,
    [[maybe_unused]] void* pUser
) {
    if (lCommand == COMM_ALARM_VIDEO_INTERCOM) {
        auto *videoIntercomAlarm = reinterpret_cast<NET_DVR_VIDEO_INTERCOM_ALARM *>(pAlarmInfo);
        PLOG_INFO << "Received Hik video intercom alarm: <" << (int) videoIntercomAlarm->byAlarmType << ">";
        if (videoIntercomAlarm->byAlarmType == 0x11) {
            PLOG_INFO << "Bell button was pressed";
            callDoorbell();
        } else if (videoIntercomAlarm->byAlarmType == 0x12) {
            PLOG_INFO << "The intercom thinks it's being fucked with";
            intercomGotFuckedWith = true;
            soundcardReadBufferCV.notify_one();
        }
    } else {
        PLOG_INFO << "Received Hik device event <" << lCommand << ">.";
    }
    PLOG_INFO << "Finished processing Hik device event";
}

HikEventListeningHandle registerForHikEvents() {
    PLOG_INFO << "Registering for Hikvision events on session id <" << sessionId << ">";

    NET_DVR_SetDVRMessageCallBack_V50(0, &hikEventsCallback, nullptr);

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

void stopVoiceCommunications() {
    PLOG_INFO << "Wrapping up voice communications on session id <" << sessionId << ">";

    std::unique_lock<std::mutex> lk(voiceComHandleMutex);
    if (!NET_DVR_StopVoiceCom(voiceComHandle)) {
        shutdown(obtainHikSDKErrorMsg("Failed to tear down voice comms."));
    } else {
        PLOG_INFO << "Successfully wrapped up voice communications on session id <" << sessionId << ">";
    }
    voiceComHandle = -1;
}

void alsaErrorLogger(
    const char *file,
    int line,
    [[maybe_unused]] const char *function,
    [[maybe_unused]] int err,
    const char *fmt, ...
) {
    va_list args;
    va_start (args, fmt);
    std::stringstream fmtStream;
    fmtStream << "Error logged from ALSA internals @ <" << file << ">:" << line << ": " << fmt;
#define BUFF_SIZE ((1 << 10) * 5)
    char buff[BUFF_SIZE];
    vsnprintf(buff, BUFF_SIZE, fmt, args);
    va_end(args);
    PLOG_ERROR << buff;
}

long currTimeInMillis() {
    struct timeval tv {};
    gettimeofday(&tv, nullptr);
    return (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
};

long currTimeInSeconds() {
    return currTimeInMillis() / 1000;
};

[[noreturn]] void soundcardReadLoop(const std::string &soundcardCoordinates) {
    PLOG_INFO << "Starting reading from soundcard @ " << soundcardCoordinates;

    snd_lib_error_set_handler(alsaErrorLogger);

    snd_pcm_t *captureHandle;
    if (
        auto sndOpenErrorMsg = checkAlsaError(
            snd_pcm_open(
                &captureHandle,
                soundcardCoordinates.c_str(),
                SND_PCM_STREAM_CAPTURE,
                0
            )
        )
    ) {
        shutdown(*sndOpenErrorMsg);
    } else {
        PLOG_INFO << "Successfully opened an ALSA capture handle.";
    }

    snd_pcm_format_t format = SND_PCM_FORMAT_MU_LAW;
    unsigned short numChannels = 1;
    unsigned int sampleRate = 8000;
    int allowResampling = 1;
    unsigned int requiredLatencyInUs = 500000;
    if (
        auto pcmSetParamsErrorMsg = checkAlsaError(
            snd_pcm_set_params(
                captureHandle,
                format,
                SND_PCM_ACCESS_RW_INTERLEAVED,
                numChannels,
                sampleRate,
                allowResampling,
                requiredLatencyInUs
            )
        )
    ) {
        shutdown(*pcmSetParamsErrorMsg);
    } else {
        PLOG_INFO << "Successfully set PCM params for capture handle";
    }

    unsigned int bitsPerSample = snd_pcm_format_width(format);
    unsigned short bitsPerByte = 8;


    long startOfSilence = -1;
    unsigned long numFramesToRead = sizeof(soundcardReadBuffer) / (numChannels * (bitsPerSample / bitsPerByte));

    bool isBufferReady = false;
    auto readFromPcm = [captureHandle, numFramesToRead, &isBufferReady]() {
        std::unique_lock<std::mutex> lk(soundcardReadBufferMutex);
        if (isBufferReady && voiceComHandle >= 0 && !intercomGotFuckedWith) {
            soundcardReadBufferCV.notify_one();
            soundcardReadBufferCV.wait(lk);
        }
        isBufferReady = false;
        return snd_pcm_readi(captureHandle, soundcardReadBuffer, numFramesToRead);
    };

    PLOG_INFO << "Capturing sound from the soundcard";
    while (true) {
        long errCode;
        PLOG_DEBUG << "About to read " << numFramesToRead << " frames from the soundcard";

        lastSoundcardLoopTime = currTimeInMillis();

        if (
            (
                errCode = readFromPcm()
            ) != numFramesToRead
        ) {
            auto errMsg = checkAlsaError((int) errCode);
            PLOG_WARNING << "Failed reading audio from soundcard: " << *errMsg;
            recoverPcm(captureHandle, (int) errCode);
        } else {
            isBufferReady = true;
            soundcardReadBufferCV.notify_one();
            bool isSilence = true;
            for (char c : soundcardReadBuffer) {
                if (c != (char) 0xFF) {
                    isSilence = false;
                    break;
                }
            }

            enum AudioRelayAction { shouldStart, shouldEnd, none };
            AudioRelayAction actionToTake = none;
            if (voiceComHandle < 0 && !isSilence) {
                PLOG_INFO << "Detected audio! Going to start relaying audio to Hik device.";
                actionToTake = shouldStart;
            } else if (voiceComHandle >= 0 && intercomGotFuckedWith) {
                PLOG_INFO << "It looks like intercom got fucked with, so we're going to need to restart voice comms.";
                actionToTake = shouldStart;
            } else if (voiceComHandle >= 0 && startOfSilence < 0 && isSilence) {
                PLOG_INFO << "Detected start of silence. If no sound is heard for "
                    << MILLIS_OF_SILENCE_BEFORE_HANGUP << " millis we will hang up voice communications.";
                startOfSilence = currTimeInMillis();
            } else if (voiceComHandle >= 0 && startOfSilence >= 0 && !isSilence) {
                PLOG_INFO << "Heard sound. Postponing hang up.";
                startOfSilence = -1;
            } else if (
                voiceComHandle >= 0 &&
                currTimeInMillis() - startOfSilence > MILLIS_OF_SILENCE_BEFORE_HANGUP &&
                isSilence
            ) {
                PLOG_INFO << "Observed " << MILLIS_OF_SILENCE_BEFORE_HANGUP << " millis of silence. Hanging up.";
                actionToTake = shouldEnd;
                startOfSilence = -1;
            }
            intercomGotFuckedWith = false;

            switch (actionToTake) {
                case shouldStart:
                    hikRelayEnabled = true;
                    soundcardReadBufferCV.notify_one();
                    startVoiceCommunications();
                    break;
                case shouldEnd:
                    hikRelayEnabled = false;
                    soundcardReadBufferCV.notify_one();
                    stopVoiceCommunications();
                    voiceComHandle = -1;
                    break;
                default:
                    soundcardReadBufferCV.notify_one();
            }
        }
    }
}

#define WATCHDOG_LOOP_INTERVAL_IN_SECONDS 10
[[noreturn]] void watchdogLoop() {
    PLOG_INFO << "Starting the watchdog loop thread";

    while (true) {
        sleep(WATCHDOG_LOOP_INTERVAL_IN_SECONDS);
        long millisSinceLastSoundcardLoop = currTimeInMillis() - lastSoundcardLoopTime;
        if (millisSinceLastSoundcardLoop > WATCHDOG_LOOP_INTERVAL_IN_SECONDS * 1000) {
            std::stringstream ss;
            ss << "The soundcard loop appears to be dead. The last loop took place "
               << millisSinceLastSoundcardLoop << " ms ago";
            shutdown(ss.str());
        } else {
            PLOG_INFO << "Still capturing sound from the soundcard.";
        }
    }

}

int main(int argc, char** argv) {

    static plog::ColorConsoleAppender<plog::TxtFormatter> consoleAppender;
    plog::init(
        plog::info,
        "/var/log/hikbridge/runtime",
        10u * (1u << 20u), // 10MB
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
            "s,audio-capture-coordinates",
            "The ALSA name of the soundcard to read mu-law sound signal from",
            cxxopts::value<std::string>()
        )
        (
            "d,doorbell-host",
            "The host to make an HTTP GET request to when the doorbell is rung",
            cxxopts::value<std::string>()
        )
        (
            "o,doorbell-port",
            "The port to make an HTTP GET request to when the doorbell is rung",
            cxxopts::value<unsigned short>()
        )
        (
            "a,doorbell-path",
            "The path to make an HTTP GET request to when the doorbell is rung",
            cxxopts::value<std::string>()
        );

    std::string deviceHost, deviceUsername, devicePassword, audioCaptureCoordinates;
    unsigned short devicePort;
    try {
        auto result = options.parse(argc, argv);
        deviceHost = result["device-host"].as<std::string>();
        devicePort = result["device-port"].as<unsigned short>();
        deviceUsername = result["device-username"].as<std::string>();
        devicePassword = result["device-password"].as<std::string>();
        audioCaptureCoordinates = result["audio-capture-coordinates"].as<std::string>();
        doorbellHost = result["doorbell-host"].as<std::string>();
        doorbellPort = result["doorbell-port"].as<unsigned short>();
        doorbellPath = result["doorbell-path"].as<std::string>();
    } catch (const cxxopts::option_has_no_value_exception& e) {
        shutdown(std::make_optional(e.what()));
    }

    sessionId = logInToDevice(
        deviceHost,
        devicePort,
        deviceUsername,
        devicePassword
    );

    registerForHikEvents();

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

    std::thread soundcardReadThread(soundcardReadLoop, audioCaptureCoordinates);

    std::thread watchdogThread(watchdogLoop);

    soundcardReadThread.join();

    shutdown();
}


#pragma clang diagnostic pop