#include "CytubeClient.hpp"

#include "json.hpp"

#include <algorithm>
#include <chrono>

namespace wxl_cytube
{
    namespace
    {
        constexpr size_t kMaxMessageBytes = 4u << 20;   // a big channel's playlist can be large; bigger is dropped
        constexpr size_t kMaxQueuedEvents = 256;
        constexpr int64_t kSilenceLimitMs = 150000;     // the server pings every 25 s (its ping timeout is 120 s)
        constexpr wchar_t kUserAgent[] = L"AzerothPlex/1.0 CyTube";

        std::wstring Wide(const std::string& text)
        {
            if (text.empty()) return {};
            const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            std::wstring wide(static_cast<size_t>(std::max(0, length)), L'\0');
            if (length > 0)
                MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
            return wide;
        }

        struct UrlParts
        {
            std::wstring host, path;
            INTERNET_PORT port = 0;
            bool secure = false;
        };

        bool CrackUrl(std::string url, UrlParts& parts)
        {
            if (url.rfind("wss://", 0) == 0) url = "https://" + url.substr(6);
            else if (url.rfind("ws://", 0) == 0) url = "http://" + url.substr(5);
            const std::wstring wide = Wide(url);
            URL_COMPONENTS components{sizeof(components)};
            components.dwSchemeLength = static_cast<DWORD>(-1);
            components.dwHostNameLength = static_cast<DWORD>(-1);
            components.dwUrlPathLength = static_cast<DWORD>(-1);
            components.dwExtraInfoLength = static_cast<DWORD>(-1);
            if (wide.empty() || !WinHttpCrackUrl(wide.c_str(), 0, 0, &components)) return false;
            parts.host.assign(components.lpszHostName, components.dwHostNameLength);
            parts.path.clear();
            if (components.lpszUrlPath && components.dwUrlPathLength) parts.path.assign(components.lpszUrlPath, components.dwUrlPathLength);
            if (components.lpszExtraInfo && components.dwExtraInfoLength)
                parts.path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
            if (parts.path.empty()) parts.path = L"/";
            parts.port = components.nPort;
            parts.secure = components.nScheme == INTERNET_SCHEME_HTTPS;
            return !parts.host.empty();
        }

        std::string JsonString(const std::string& text) { return nlohmann::json(text).dump(); }

        std::string MessageField(const std::string& jsonText, const char* field)
        {
            try
            {
                const auto value = nlohmann::json::parse(jsonText);
                if (value.is_object() && value.contains(field) && value[field].is_string()) return value[field].get<std::string>();
                if (value.is_string()) return value.get<std::string>();
            }
            catch (const nlohmann::json::exception&)
            {
            }
            return jsonText;
        }
    }

    const char* ToString(ClientState state)
    {
        switch (state)
        {
        case ClientState::Idle: return "Not in a channel";
        case ClientState::Connecting: return "Connecting";
        case ClientState::Joined: return "In the channel";
        case ClientState::Reconnecting: return "Reconnecting";
        case ClientState::Stopped: return "Stopped";
        }
        return "?";
    }

    CytubeClient::CytubeClient() : worker_([this](std::stop_token stop) { Worker(stop); })
    {
    }

    CytubeClient::~CytubeClient()
    {
        Leave();
        worker_.request_stop();
        changed_.notify_all();
    }

    int64_t CytubeClient::NowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    void CytubeClient::Join(const ChannelRef& channel, std::string password)
    {
        {
            // (Under the lock, so a waiting worker can't miss it.)
            std::lock_guard lock(mutex_);
            channel_ = channel;
            password_ = std::move(password);
            outgoing_.clear();
            ++generation_;
            wanted_ = true;
        }
        changed_.notify_all();
    }

    void CytubeClient::Leave()
    {
        {
            std::lock_guard lock(mutex_);
            wanted_ = false;
            ++generation_;
            outgoing_.clear();
        }
        changed_.notify_all();
    }

    void CytubeClient::SendPassword(const std::string& password)
    {
        std::lock_guard lock(mutex_);
        password_ = password;
        outgoing_.push_back(EventPacket("channelPassword", JsonString(password)));
    }

    void CytubeClient::RequestMedia()
    {
        std::lock_guard lock(mutex_);
        outgoing_.push_back(EventPacket("playerReady", ""));
    }

    bool CytubeClient::PopEvent(ClientEvent& event)
    {
        std::lock_guard lock(mutex_);
        if (events_.empty()) return false;
        event = std::move(events_.front());
        events_.pop_front();
        return true;
    }

    ChannelRef CytubeClient::Channel() const
    {
        std::lock_guard lock(mutex_);
        return channel_;
    }

    void CytubeClient::Push(ClientEvent::Kind kind, std::string text)
    {
        ClientEvent event;
        event.kind = kind;
        event.state = state_.load();
        event.text = std::move(text);
        event.receivedAtMs = NowMs();
        std::lock_guard lock(mutex_);
        if (events_.size() >= kMaxQueuedEvents)
        {
            // The UI thread is behind: drop the oldest clock update (a newer one follows), else the oldest event.
            const auto update = std::find_if(events_.begin(), events_.end(), [](const ClientEvent& e) {
                return e.kind == ClientEvent::Kind::MediaUpdate;
            });
            events_.erase(update != events_.end() ? update : events_.begin());
        }
        events_.push_back(std::move(event));
    }

    void CytubeClient::SetState(ClientState state, std::string why)
    {
        state_ = state;
        Push(ClientEvent::Kind::State, std::move(why));
    }

    bool CytubeClient::SleepUnlessChanged(std::stop_token stop, std::chrono::milliseconds wait, uint64_t generation)
    {
        std::unique_lock lock(mutex_);
        return !changed_.wait_for(lock, stop, wait, [&] { return generation_.load() != generation; });
    }

    bool CytubeClient::FetchServer(const ChannelRef& channel, std::string& serverUrl, std::string& error)
    {
        UrlParts parts;
        if (!CrackUrl(channel.origin + SocketConfigPath(channel), parts)) { error = "That CyTube address can't be used."; return false; }
        HINTERNET session = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) { error = "Windows couldn't open a web session."; return false; }
        WinHttpSetTimeouts(session, 10000, 10000, 10000, 10000);
        HINTERNET connection = WinHttpConnect(session, parts.host.c_str(), parts.port, 0);
        HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", parts.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                            WINHTTP_DEFAULT_ACCEPT_TYPES, parts.secure ? WINHTTP_FLAG_SECURE : 0)
                                       : nullptr;
        std::string body;
        DWORD status = 0;
        if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr))
        {
            DWORD statusBytes = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &status, &statusBytes, WINHTTP_NO_HEADER_INDEX);
            char buffer[8192];
            DWORD read = 0;
            while (body.size() < 256 * 1024 && WinHttpReadData(request, buffer, sizeof(buffer), &read) && read > 0)
                body.append(buffer, read);
        }
        if (request) WinHttpCloseHandle(request);
        if (connection) WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        if (status != 200)
        {
            error = status == 0 ? "Couldn't reach CyTube." : "CyTube answered " + std::to_string(status) + " for that channel.";
            return false;
        }
        return PickSocketServer(body, serverUrl, error);
    }

    HINTERNET CytubeClient::OpenWebSocket(const std::string& url, HINTERNET& session, HINTERNET& connection)
    {
        session = connection = nullptr;
        UrlParts parts;
        if (!CrackUrl(url, parts)) return nullptr;
        session = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return nullptr;
        // A 1 s receive timeout is the worker's tick (sends, Leave); the socket stays usable after it.
        WinHttpSetTimeouts(session, 10000, 10000, 10000, 1000);
        connection = WinHttpConnect(session, parts.host.c_str(), parts.port, 0);
        if (!connection) return nullptr;
        HINTERNET request = WinHttpOpenRequest(connection, L"GET", parts.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES, parts.secure ? WINHTTP_FLAG_SECURE : 0);
        if (!request) return nullptr;
        DWORD status = 0;
        DWORD statusBytes = sizeof(status);
        if (!WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) ||
            !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request, nullptr) ||
            !WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                 &status, &statusBytes, WINHTTP_NO_HEADER_INDEX) ||
            status != 101)
        {
            WinHttpCloseHandle(request);
            return nullptr;
        }
        HINTERNET socket = WinHttpWebSocketCompleteUpgrade(request, 0);
        WinHttpCloseHandle(request);
        return socket;
    }

    bool CytubeClient::SendText(HINTERNET socket, const std::string& text)
    {
        return WinHttpWebSocketSend(socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                    const_cast<char*>(text.data()), static_cast<DWORD>(text.size())) == ERROR_SUCCESS;
    }

    int CytubeClient::ReceiveText(HINTERNET socket, std::string& text)
    {
        text.clear();
        bool oversized = false;
        std::vector<char> buffer(64 * 1024);
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type = WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE;
        bool started = false;
        do
        {
            DWORD read = 0;
            const DWORD result = WinHttpWebSocketReceive(socket, buffer.data(), static_cast<DWORD>(buffer.size()), &read, &type);
            if (result == ERROR_WINHTTP_TIMEOUT)
            {
                if (!started) return 0;
                continue; // the rest of a message on its way
            }
            if (result != ERROR_SUCCESS || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) return -1;
            started = true;
            if (oversized) continue; // drain it
            if (text.size() + read > kMaxMessageBytes) { oversized = true; text.clear(); continue; }
            text.append(buffer.data(), read);
        } while (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE);
        return oversized ? 0 : 1;
    }

    // One connection to the channel; false when it must not reconnect (kicked, refused for good).
    bool CytubeClient::Session(std::stop_token stop, const ChannelRef& channel, const std::string& serverUrl)
    {
        const uint64_t generation = generation_.load();
        HINTERNET session = nullptr, connection = nullptr;
        HINTERNET socket = OpenWebSocket(WebSocketUrl(serverUrl), session, connection);
        bool again = true;
        if (!socket)
        {
            Push(ClientEvent::Kind::Message, "Couldn't open CyTube's connection.");
        }
        else
        {
            bool joined = false;
            int64_t lastHeard = NowMs();
            while (!stop.stop_requested() && wanted_.load() && generation_.load() == generation)
            {
                if (joined)
                {
                    std::deque<std::string> sends;
                    {
                        std::lock_guard lock(mutex_);
                        sends.swap(outgoing_);
                    }
                    bool sent = true;
                    for (const auto& text : sends) sent = sent && SendText(socket, text);
                    if (!sent) break;
                }
                std::string text;
                const int got = ReceiveText(socket, text);
                if (got < 0) break;
                const int64_t now = NowMs();
                if (got == 0)
                {
                    if (now - lastHeard > kSilenceLimitMs) { Push(ClientEvent::Kind::Message, "CyTube went quiet; reconnecting."); break; }
                    continue;
                }
                lastHeard = now;
                Packet packet;
                if (!ParsePacket(text, packet)) continue;
                if (packet.engine == '0') { if (!SendText(socket, "40")) break; continue; } // open: connect "/"
                if (packet.engine == '2') { if (!SendText(socket, "3")) break; continue; } // ping: pong
                if (packet.engine == '1') break;                                            // close
                if (packet.engine != '4') continue;
                if (packet.socket == '0')
                {
                    std::string password;
                    {
                        std::lock_guard lock(mutex_);
                        password = password_;
                    }
                    if (!SendText(socket, JoinChannelPacket(channel, password))) break;
                    joined = true;
                    SetState(ClientState::Joined);
                    continue;
                }
                if (packet.socket == '4')
                {
                    Push(ClientEvent::Kind::Message, "CyTube refused the connection: " + MessageField(packet.data, "message"));
                    break;
                }
                if (packet.socket == '1') break;
                if (packet.socket != '2') continue;
                const std::string& event = packet.event;
                if (event == "changeMedia") Push(ClientEvent::Kind::ChangeMedia, packet.data);
                else if (event == "mediaUpdate") Push(ClientEvent::Kind::MediaUpdate, packet.data);
                else if (event == "needPassword") Push(ClientEvent::Kind::NeedPassword, packet.data);
                else if (event == "cancelNeedPassword") Push(ClientEvent::Kind::PasswordAccepted);
                else if (event == "errorMsg") Push(ClientEvent::Kind::Message, MessageField(packet.data, "msg"));
                else if (event == "channelNotRegistered")
                    Push(ClientEvent::Kind::Message, "This channel isn't registered (nobody may be playing anything).");
                else if (event == "kick")
                {
                    Push(ClientEvent::Kind::Kicked, MessageField(packet.data, "reason"));
                    again = false;
                    break;
                }
                else if (event == "partitionChange")
                {
                    Push(ClientEvent::Kind::Message, "The channel moved to another CyTube server; reconnecting.");
                    break;
                }
            }
            WinHttpWebSocketClose(socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
            WinHttpCloseHandle(socket);
        }
        if (connection) WinHttpCloseHandle(connection);
        if (session) WinHttpCloseHandle(session);
        return again;
    }

    void CytubeClient::Worker(std::stop_token stop)
    {
        int failures = 0;
        while (!stop.stop_requested())
        {
            if (!wanted_.load())
            {
                if (state_.load() != ClientState::Idle && state_.load() != ClientState::Stopped) SetState(ClientState::Idle);
                failures = 0;
                std::unique_lock lock(mutex_);
                changed_.wait_for(lock, stop, std::chrono::seconds(1), [this] { return wanted_.load(); });
                continue;
            }
            const uint64_t generation = generation_.load();
            const ChannelRef channel = Channel();
            SetState(failures == 0 ? ClientState::Connecting : ClientState::Reconnecting);
            std::string server, error;
            bool again = true;
            if (!FetchServer(channel, server, error)) Push(ClientEvent::Kind::Message, error);
            else again = Session(stop, channel, server);
            if (stop.stop_requested()) break;
            if (!again)
            {
                wanted_ = false;
                SetState(ClientState::Stopped);
                continue;
            }
            if (generation_.load() != generation) { failures = 0; continue; } // Join or Leave: go at once
            if (!wanted_.load()) continue;
            ++failures;
            const auto wait = std::chrono::seconds(failures == 1 ? 10 : failures == 2 ? 30 : 60);
            SetState(ClientState::Reconnecting, "Trying again in " + std::to_string(wait.count()) + " s.");
            SleepUnlessChanged(stop, wait, generation);
        }
    }
}
