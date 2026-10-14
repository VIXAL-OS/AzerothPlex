#pragma once

// A passive CyTube viewer's connection, on a worker thread: fetch the channel's socketconfig, open its socket.io
// WebSocket (WinHTTP: Schannel, no OpenSSL), join the channel, answer pings, and queue what the UI thread needs
// (changeMedia, mediaUpdate, password prompts, messages, kicks). Reconnects no faster than every 10 s (CyTube allows a
// burst of 5 connections and then one per 10 s per IP), re-reading the socketconfig each time; not after a kick.

#include "CytubeProtocol.hpp"

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wxl_cytube
{
    enum class ClientState
    {
        Idle,
        Connecting,
        Joined,
        Reconnecting,
        Stopped, // kicked or refused: no reconnect until Join again
    };

    const char* ToString(ClientState state);

    struct ClientEvent
    {
        enum class Kind
        {
            State,            // text: why (when there's something to say)
            ChangeMedia,      // text: the event's JSON
            MediaUpdate,      // text: the event's JSON
            NeedPassword,     // text: "true" when the last password was wrong
            PasswordAccepted,
            Message,          // text: a message from the channel (errorMsg) or about the connection
            Kicked,           // text: the reason
        };
        Kind kind = Kind::Message;
        ClientState state = ClientState::Idle;
        std::string text;
        int64_t receivedAtMs = 0; // system clock, when it arrived
    };

    class CytubeClient final
    {
    public:
        CytubeClient();
        ~CytubeClient();
        CytubeClient(const CytubeClient&) = delete;
        CytubeClient& operator=(const CytubeClient&) = delete;

        void Join(const ChannelRef& channel, std::string password = {});
        void Leave();
        void SendPassword(const std::string& password);
        void RequestMedia(); // playerReady: the server sends the current media again
        bool PopEvent(ClientEvent& event);

        ClientState State() const { return state_.load(); }
        ChannelRef Channel() const;

        static int64_t NowMs();

    private:
        void Worker(std::stop_token stop);
        bool Session(std::stop_token stop, const ChannelRef& channel, const std::string& serverUrl);
        bool FetchServer(const ChannelRef& channel, std::string& serverUrl, std::string& error);
        HINTERNET OpenWebSocket(const std::string& url, HINTERNET& session, HINTERNET& connection);
        bool SendText(HINTERNET socket, const std::string& text);
        int ReceiveText(HINTERNET socket, std::string& text); // 1 message, 0 timeout, -1 closed or failed
        void Push(ClientEvent::Kind kind, std::string text = {});
        void SetState(ClientState state, std::string why = {});
        bool SleepUnlessChanged(std::stop_token stop, std::chrono::milliseconds wait, uint64_t generation);

        mutable std::mutex mutex_;
        std::condition_variable_any changed_;
        ChannelRef channel_;
        std::string password_;
        std::deque<std::string> outgoing_;
        std::deque<ClientEvent> events_;
        std::atomic<ClientState> state_{ClientState::Idle};
        std::atomic<uint64_t> generation_{0}; // bumped by Join and Leave
        std::atomic<bool> wanted_{false};
        std::jthread worker_;
    };
}
