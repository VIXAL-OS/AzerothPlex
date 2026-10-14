#include "CytubeProtocol.hpp"

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace wxl_cytube
{
    namespace
    {
        using json = nlohmann::json;

        std::string Trim(std::string_view text)
        {
            size_t begin = 0, end = text.size();
            while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
            while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
            return std::string(text.substr(begin, end - begin));
        }

        std::string Lower(std::string text)
        {
            for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        bool ValidChannel(std::string_view name)
        {
            if (name.empty() || name.size() > 30) return false;
            return std::all_of(name.begin(), name.end(), [](char c) {
                return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
            });
        }

        std::string StringField(const json& object, const char* name)
        {
            const auto it = object.find(name);
            if (it == object.end()) return {};
            if (it->is_string()) return it->get<std::string>();
            if (it->is_number_integer()) return std::to_string(it->get<long long>());
            return {};
        }

        double NumberField(const json& object, const char* name, double fallback = 0.0)
        {
            const auto it = object.find(name);
            return it != object.end() && it->is_number() ? it->get<double>() : fallback;
        }

        // "a;b" -> a, b.
        bool SplitPair(const std::string& id, std::string& a, std::string& b)
        {
            const size_t at = id.find(';');
            if (at == std::string::npos || at == 0 || at + 1 >= id.size()) return false;
            a = id.substr(0, at);
            b = id.substr(at + 1);
            return true;
        }

        // A custom manifest's (or Vimeo workaround's) "direct": {"<quality>":[{"link":...}], ...}: the highest quality
        // up to maxHeight, else the lowest.
        std::string PickDirect(const json& direct, int maxHeight)
        {
            if (!direct.is_object()) return {};
            int best = -1, lowest = 1 << 30;
            std::string bestLink, lowestLink;
            for (auto it = direct.begin(); it != direct.end(); ++it)
            {
                if (!it.value().is_array() || it.value().empty()) continue;
                const json& source = it.value().front();
                if (!source.is_object()) continue;
                const std::string link = StringField(source, "link");
                if (link.rfind("https://", 0) != 0 && link.rfind("http://", 0) != 0) continue;
                int quality = 0;
                try { quality = std::stoi(it.key()); } catch (...) { quality = 0; }
                if (quality <= maxHeight && quality > best) { best = quality; bestLink = link; }
                if (quality < lowest) { lowest = quality; lowestLink = link; }
            }
            return !bestLink.empty() ? bestLink : lowestLink;
        }
    }

    bool ParseChannelRef(std::string_view text, ChannelRef& out, std::string& error)
    {
        std::string value = Trim(text);
        out = {};
        if (value.empty()) { error = "Enter a CyTube channel link (cytu.be/r/<channel>)."; return false; }
        std::string name;
        const size_t scheme = value.find("://");
        if (scheme == std::string::npos && value.find('/') == std::string::npos)
        {
            name = value; // a bare channel name
        }
        else
        {
            if (scheme == std::string::npos) value = "https://" + value;
            const size_t hostStart = value.find("://") + 3;
            const size_t pathStart = value.find('/', hostStart);
            const std::string origin = Lower(value.substr(0, pathStart));
            const std::string path = pathStart == std::string::npos ? std::string{} : value.substr(pathStart);
            if (origin.rfind("https://", 0) != 0 && origin.rfind("http://", 0) != 0)
            { error = "A CyTube link starts with https://."; return false; }
            if (path.rfind("/r/", 0) != 0) { error = "A CyTube channel link looks like cytu.be/r/<channel>."; return false; }
            name = path.substr(3);
            for (const char stop : { '/', '?', '#' })
                if (const size_t at = name.find(stop); at != std::string::npos) name.resize(at);
            out.origin = origin == "https://www.cytu.be" ? "https://cytu.be" : origin;
        }
        if (!ValidChannel(name)) { error = "CyTube channel names are 1-30 letters, digits, _ or -."; return false; }
        out.channel = Lower(name);
        return true;
    }

    std::string SocketConfigPath(const ChannelRef& ref)
    {
        return "/socketconfig/" + ref.channel + ".json";
    }

    bool PickSocketServer(std::string_view text, std::string& url, std::string& error)
    {
        url.clear();
        try
        {
            const json config = json::parse(text.begin(), text.end());
            const auto servers = config.find("servers");
            if (servers == config.end() || !servers->is_array() || servers->empty())
            { error = "CyTube listed no socket servers."; return false; }
            std::string fallback;
            for (const json& server : *servers)
            {
                if (!server.is_object()) continue;
                const std::string candidate = StringField(server, "url");
                const bool secure = server.value("secure", false) && candidate.rfind("https://", 0) == 0;
                if (!secure) continue;
                if (fallback.empty()) fallback = candidate;
                if (!server.value("ipv6", false)) { url = candidate; return true; }
            }
            if (!fallback.empty()) { url = fallback; return true; }
            error = "CyTube listed no secure socket server.";
        }
        catch (const json::exception&)
        {
            error = "CyTube's socket configuration couldn't be read.";
        }
        return false;
    }

    std::string WebSocketUrl(const std::string& serverUrl)
    {
        std::string base = serverUrl;
        if (base.rfind("https://", 0) == 0) base = "wss://" + base.substr(8);
        else if (base.rfind("http://", 0) == 0) base = "ws://" + base.substr(7);
        while (!base.empty() && base.back() == '/') base.pop_back();
        return base + "/socket.io/?EIO=4&transport=websocket";
    }

    bool ParsePacket(std::string_view text, Packet& out)
    {
        out = {};
        if (text.empty()) return false;
        out.engine = text[0];
        if (out.engine != '4')
        {
            out.data = std::string(text.substr(1));
            return out.engine >= '0' && out.engine <= '6';
        }
        if (text.size() < 2) return false;
        out.socket = text[1];
        size_t at = 2;
        // A namespace other than "/" would start with "/" and end at ","; CyTube uses the default one.
        if (at < text.size() && text[at] == '/')
        {
            const size_t comma = text.find(',', at);
            if (comma == std::string_view::npos) return true;
            at = comma + 1;
        }
        while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at]))) ++at; // an ack id
        const std::string_view payload = text.substr(at);
        if (out.socket != '2')
        {
            out.data = std::string(payload);
            return true;
        }
        try
        {
            const json array = json::parse(payload.begin(), payload.end());
            if (!array.is_array() || array.empty() || !array[0].is_string()) return false;
            out.event = array[0].get<std::string>();
            if (array.size() > 1) out.data = array[1].dump();
            return true;
        }
        catch (const json::exception&)
        {
            return false;
        }
    }

    std::string EventPacket(std::string_view event, std::string_view jsonArgument)
    {
        json array = json::array();
        array.push_back(std::string(event));
        if (!jsonArgument.empty())
        {
            try { array.push_back(json::parse(jsonArgument.begin(), jsonArgument.end())); }
            catch (const json::exception&) { array.push_back(std::string(jsonArgument)); }
        }
        return "42" + array.dump();
    }

    std::string JoinChannelPacket(const ChannelRef& ref, std::string_view password)
    {
        json argument = {{"name", ref.channel}};
        if (!password.empty()) argument["pw"] = std::string(password);
        return EventPacket("joinChannel", argument.dump());
    }

    bool ParseChangeMedia(std::string_view text, Media& out, std::string& error)
    {
        out = {};
        try
        {
            const json media = json::parse(text.begin(), text.end());
            if (!media.is_object()) { error = "changeMedia wasn't an object."; return false; }
            out.id = StringField(media, "id");
            out.title = StringField(media, "title");
            out.type = StringField(media, "type");
            out.seconds = std::max(0.0, NumberField(media, "seconds"));
            out.currentTime = NumberField(media, "currentTime");
            out.paused = media.value("paused", false);
            if (const auto meta = media.find("meta"); meta != media.end() && meta->is_object()) out.metaJson = meta->dump();
            if (out.id.empty() || out.type.empty()) { error = "changeMedia had no id or type."; return false; }
            return true;
        }
        catch (const json::exception&)
        {
            error = "changeMedia couldn't be read.";
            return false;
        }
    }

    bool ParseMediaUpdate(std::string_view text, double& currentTime, bool& paused)
    {
        try
        {
            const json update = json::parse(text.begin(), text.end());
            if (!update.is_object() || !update.contains("currentTime") || !update["currentTime"].is_number()) return false;
            currentTime = update["currentTime"].get<double>();
            paused = update.value("paused", false);
            return true;
        }
        catch (const json::exception&)
        {
            return false;
        }
    }

    Playable ResolvePlayable(const Media& media, int maxHeight)
    {
        Playable playable;
        const std::string& id = media.id;
        const std::string& type = media.type;
        json meta = json::object();
        if (!media.metaJson.empty())
        {
            try { meta = json::parse(media.metaJson); } catch (const json::exception&) { meta = json::object(); }
        }
        const auto direct = [&](std::string url) { playable.kind = Playable::Kind::Direct; playable.url = std::move(url); };
        // mpv's yt-dlp hook runs first for youtube.com, youtu.be and twitch.tv; others need "ytdl://" to go through it.
        const auto ytdl = [&](std::string url, bool whitelisted) {
            playable.kind = Playable::Kind::Ytdl;
            playable.url = whitelisted ? std::move(url) : "ytdl://" + url;
        };
        std::string a, b;
        if (type == "yt") ytdl("https://www.youtube.com/watch?v=" + id, true);
        else if (type == "vi")
        {
            const std::string link = meta.contains("direct") ? PickDirect(meta["direct"], maxHeight) : std::string{};
            if (!link.empty()) direct(link);
            else ytdl("https://vimeo.com/" + id, false);
        }
        else if (type == "dm") ytdl("https://www.dailymotion.com/video/" + id, false);
        else if (type == "fi") direct(id);
        else if (type == "cm")
        {
            const std::string link = meta.contains("direct") ? PickDirect(meta["direct"], maxHeight) : std::string{};
            if (!link.empty()) direct(link);
            else playable.reason = "this custom media has no playable source";
        }
        else if (type == "hl") { direct(id); playable.live = true; }
        else if (type == "tw") { ytdl("https://www.twitch.tv/" + id, true); playable.live = true; }
        else if (type == "tv") ytdl("https://www.twitch.tv/videos/" + (id.size() > 1 && id[0] == 'v' ? id.substr(1) : id), true);
        else if (type == "tc") ytdl("https://clips.twitch.tv/" + id, true);
        else if (type == "gd") ytdl("https://drive.google.com/file/d/" + id, false);
        else if (type == "sc") ytdl(id, false);
        else if (type == "sb") ytdl("https://streamable.com/" + id, false);
        else if (type == "pt" && SplitPair(id, a, b)) ytdl("https://" + a + "/videos/watch/" + b, false);
        else if (type == "bc") ytdl("https://www.bitchute.com/video/" + id + "/", false);
        else if (type == "bn" && SplitPair(id, a, b)) ytdl("https://" + a + ".bandcamp.com/track/" + b, false);
        else if (type == "od" && SplitPair(id, a, b)) ytdl("https://odysee.com/@" + a + "/" + b, false);
        else if (type == "nv") ytdl("https://www.nicovideo.jp/watch/" + id, false);
        else playable.reason = "CyTube's " + type + " media plays only in a browser";
        if (playable.kind != Playable::Kind::Unsupported && media.seconds <= 0.0) playable.live = true;
        if (playable.kind == Playable::Kind::Direct && playable.url.rfind("https://", 0) != 0 && playable.url.rfind("http://", 0) != 0)
        {
            playable = {};
            playable.reason = "this media's link isn't a web address";
        }
        return playable;
    }

    wxl_party::MediaIdentity IdentityFor(const ChannelRef& ref, const Media& media)
    {
        wxl_party::MediaIdentity identity;
        identity.serverIdentifier = "cytube:" + ref.channel;
        identity.ratingKey = media.type + ":" + media.id;
        identity.title = media.title;
        identity.type = "cytube";
        identity.durationMs = static_cast<int64_t>(std::llround(media.seconds * 1000.0));
        return identity;
    }

    wxl_party::PartyPlaybackState ToPlaybackState(const ChannelRef& ref, const Media& media, double currentTime, bool paused,
                                                  int64_t receivedAtMs, uint64_t epoch, uint64_t sequence)
    {
        wxl_party::PartyPlaybackState state;
        state.media = IdentityFor(ref, media);
        state.paused = paused || currentTime < 0.0;
        state.playing = !state.paused;
        state.stopped = false;
        state.positionMs = static_cast<int64_t>(std::llround(std::max(0.0, currentTime) * 1000.0));
        state.durationMs = state.media.durationMs;
        state.authoritativeTimestampMs = receivedAtMs;
        state.roomEpoch = epoch;
        state.sequence = sequence;
        return state;
    }
}
