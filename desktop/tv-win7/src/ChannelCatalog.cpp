#include "ChannelCatalog.h"
#include "StreamSecurity.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace zethropol::tvwin7 {

namespace {

std::string jsonEscape(const std::string &in)
{
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c >= 0x20) {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    return out;
}

bool extractJsonString(const std::string &json, const std::string &key, std::string &valueOut)
{
    const std::string needle = "\"" + key + "\"";
    const std::size_t keyPos = json.find(needle);
    if (keyPos == std::string::npos) {
        return false;
    }
    std::size_t pos = json.find(':', keyPos + needle.size());
    if (pos == std::string::npos) {
        return false;
    }
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '"') {
        return false;
    }
    ++pos;
    std::string out;
    while (pos < json.size()) {
        const char c = json[pos++];
        if (c == '\\' && pos < json.size()) {
            const char esc = json[pos++];
            switch (esc) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            default: out.push_back(esc); break;
            }
        } else if (c == '"') {
            valueOut = out;
            return true;
        } else {
            out.push_back(c);
        }
    }
    return false;
}

bool extractJsonInt(const std::string &json, const std::string &key, int &valueOut)
{
    const std::string needle = "\"" + key + "\"";
    const std::size_t keyPos = json.find(needle);
    if (keyPos == std::string::npos) {
        return false;
    }
    std::size_t pos = json.find(':', keyPos + needle.size());
    if (pos == std::string::npos) {
        return false;
    }
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
    if (pos >= json.size()) {
        return false;
    }
    bool neg = false;
    if (json[pos] == '-') {
        neg = true;
        ++pos;
    }
    if (pos >= json.size() || !std::isdigit(static_cast<unsigned char>(json[pos]))) {
        return false;
    }
    long val = 0;
    while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos]))) {
        val = val * 10 + (json[pos] - '0');
        if (val > 1000000) {
            break;
        }
        ++pos;
    }
    valueOut = static_cast<int>(neg ? -val : val);
    return true;
}

bool extractJsonBool(const std::string &json, const std::string &key, bool &valueOut)
{
    const std::string needle = "\"" + key + "\"";
    const std::size_t keyPos = json.find(needle);
    if (keyPos == std::string::npos) {
        return false;
    }
    std::size_t pos = json.find(':', keyPos + needle.size());
    if (pos == std::string::npos) {
        return false;
    }
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
    if (json.compare(pos, 4, "true") == 0) {
        valueOut = true;
        return true;
    }
    if (json.compare(pos, 5, "false") == 0) {
        valueOut = false;
        return true;
    }
    return false;
}

} // namespace

std::vector<ChannelEntry> ChannelCatalog::buildTurkish31Channels()
{
    // Exact parity with Stage 05 (6ddae21) desktop/tv/src/ChannelManager.cpp (v5 — 31 Turkish channels).
    // 1-10 Ulusal, 11-22 Haber, 23-24 Spor, 25-26 Belgesel, 27-30 Çocuk, 31 Kültür.
    struct Seed
    {
        const char *tvgId;
        const char *name;
        const char *group;
        const char *streamUrl;
        const char *sourcePageUrl;
        const char *fallback1;
        const char *fallback2;
        const char *fallback3;
    };

    static const Seed kSeeds[] = {
        // --- Ulusal Kanallar (1-10) ---
        {
            "TRT1.tr",
            "TRT 1",
            "Ulusal",
            "https://tv-trt1.medya.trt.com.tr/master.m3u8",
            "https://www.trtizle.com/canli/tv/trt-1",
            "https://trt.daioncdn.net/trt-1/master.m3u8?app=clean",
            "",
            "",
        },
        {
            "ATV.tr",
            "ATV",
            "Ulusal",
            "",
            "https://www.atv.com.tr/canli-yayin",
            "https://securevideotoken.tmgrup.com.tr/webtv/secure?url=https%3A%2F%2Ftrkvz-live.ercdn.net%2Fatvhd%2Fatvhd.m3u8",
            "https://www.youtube.com/@atvturkiye/live",
            "",
        },
        {
            "ShowTV.tr",
            "Show TV",
            "Ulusal",
            "https://ciner.daioncdn.net/showtv/showtv.m3u8?ce=3&app=4bc856ef-4c68-4a94-bc87-37dfaaa66558",
            "https://www.showtv.com.tr/canli-yayin",
            "https://www.youtube.com/@showtv/live",
            "",
            "",
        },
        {
            "StarTV.tr",
            "Star TV",
            "Ulusal",
            "https://dogus.daioncdn.net/startv/startv.m3u8?app=startv_web&ce=3",
            "https://www.startv.com.tr/canli-yayin",
            "https://www.youtube.com/@StarTVResmi/live",
            "",
            "",
        },
        {
            "KanalD.tr",
            "Kanal D",
            "Ulusal",
            "https://demiroren.daioncdn.net/kanald/kanald.m3u8?app=kanald_web&ce=3",
            "https://www.kanald.com.tr/canli-yayin",
            "https://kdlive.duhnet.tv/S2/HLS_LIVE/kanalddainp/playlist.m3u8",
            "https://www.youtube.com/@kanald/live",
            "",
        },
        {
            "NOW.tr",
            "NOW",
            "Ulusal",
            "",
            "https://www.nowtv.com.tr/canli-yayin",
            "https://www.youtube.com/@nowtvturkiye/live",
            "",
            "",
        },
        {
            "TV8.tr",
            "TV8",
            "Ulusal",
            "https://tv8.daioncdn.net/tv8/tv8.m3u8?app=7ddc255a-ef47-4e81-ab14-c0e5f2949788&ce=3",
            "https://www.tv8.com.tr/canli-yayin",
            "",
            "",
            "",
        },
        {
            "Kanal7.tr",
            "Kanal 7",
            "Ulusal",
            "https://kanal7-live.daioncdn.net/kanal7/kanal7.m3u8",
            "https://www.kanal7.com/canli-izle",
            "https://www.youtube.com/@Kanal7/live",
            "",
            "",
        },
        {
            "BeyazTV.tr",
            "Beyaz TV",
            "Ulusal",
            "https://beyaztv-live.daioncdn.net/beyaztv/beyaztv.m3u8",
            "https://www.beyaztv.com.tr/canli-yayin/",
            "https://www.youtube.com/@beyaztv/live",
            "",
            "",
        },
        {
            "TV85.tr",
            "TV8.5",
            "Ulusal",
            "https://tv8.daioncdn.net/tv8bucuk/tv8bucuk.m3u8?app=tv8bucuk_web&ce=3",
            "https://www.tv8bucuk.com/tv8-5-canli-yayin",
            "",
            "",
            "",
        },
        // --- Haber Kanalları (11-22) ---
        {
            "TRTHaber.tr",
            "TRT Haber",
            "Haber",
            "https://tv-trthaber.medya.trt.com.tr/master.m3u8",
            "https://www.tabii.com/tr/watch/live/trthaber",
            "https://www.youtube.com/@trthaber/live",
            "",
            "",
        },
        {
            "NTV.tr",
            "NTV",
            "Haber",
            "https://dogus.daioncdn.net/ntv/ntv.m3u8?app=ntv_web&ce=3",
            "https://www.ntv.com.tr/canli-yayin/ntv",
            "https://www.youtube.com/@NTV/live",
            "",
            "",
        },
        {
            "CNNTurk.tr",
            "CNN Türk",
            "Haber",
            "",
            "https://www.cnnturk.com/action/media/62d6814670380e2cdc7c124c",
            "https://www.cnnturk.com/canli-yayin",
            "https://www.youtube.com/@cnnturk/live",
            "",
        },
        {
            "Haberturk.tr",
            "Habertürk",
            "Haber",
            "https://ciner.daioncdn.net/haberturktv/haberturktv.m3u8?app=c98ab0b0-50cc-495b-bb37-778e91f5ff5b&ce=3",
            "https://www.haberturk.com/canliyayin",
            "https://www.youtube.com/@HaberturkTV/live",
            "",
            "",
        },
        {
            "HalkTV.tr",
            "Halk TV",
            "Haber",
            "https://halktv-live.daioncdn.net/halktv/halktv.m3u8",
            "https://halktv.com.tr/canli-yayin",
            "https://www.youtube.com/@halktvcomtr/live",
            "",
            "",
        },
        {
            "SozcuTV.tr",
            "Sözcü TV",
            "Haber",
            "",
            "https://www.youtube.com/@Sozcutelevizyonu/live",
            "https://www.sozcu.com.tr/",
            "",
            "",
        },
        {
            "AHaber.tr",
            "A Haber",
            "Haber",
            "https://trkvz-live.ercdn.net/ahaberhd/ahaberhd.m3u8",
            "https://www.ahaber.com.tr/video/canli-yayin",
            "https://securevideotoken.tmgrup.com.tr/webtv/secure?url=https%3A%2F%2Ftrkvz-live.ercdn.net%2Fahaberhd%2Fahaberhd.m3u8",
            "https://rnttwmjcin.turknet.ercdn.net/lcpmvefbyo/ahaber/ahaber.m3u8",
            "https://www.youtube.com/@Ahaber/live",
        },
        {
            "Tele1.tr",
            "Tele1",
            "Haber",
            "",
            "https://www.tele1.com.tr/canli-yayin/",
            "https://tele1-live.ercdn.net/tele1/tele1.m3u8",
            "https://www.youtube.com/@tele1comtr/live",
            "",
        },
        {
            "HaberGlobal.tr",
            "Haber Global",
            "Haber",
            "https://tv.ensonhaber.com/haberglobal/haberglobal.m3u8",
            "https://haberglobal.com/canli-yayin",
            "https://www.youtube.com/@HaberGlobal/live",
            "",
            "",
        },
        {
            "TGRTHaber.tr",
            "TGRT Haber",
            "Haber",
            "https://tgrthaber-live.daioncdn.net/tgrthaber/tgrthaber.m3u8",
            "https://www.tgrthaber.com/canli-yayin",
            "https://canli.tgrthaber.com/tgrt.m3u8",
            "https://www.youtube.com/@tgrthaber/live",
            "",
        },
        {
            "UlkeTV.tr",
            "Ülke TV",
            "Haber",
            "https://livetv.radyotvonline.net/kanal7live/ulketv/playlist.m3u8",
            "https://www.ulketv.com/",
            "https://www.youtube.com/@ulketv/live",
            "",
            "",
        },
        {
            "24TV.tr",
            "Kanal 24",
            "Haber",
            "https://kanal24-live.lg.mncdn.com/kanal24_turkmedya/live.m3u8",
            "https://www.yirmidort.tv/canli-yayin",
            "https://turkmedya-live.ercdn.net/tv24/tv24.m3u8",
            "https://www.youtube.com/channel/UCN7VYCsI4Lx1-J4_BtjoWUA/live",
            "",
        },
        // --- Spor (23-24) ---
        {
            "TRTSpor.tr",
            "TRT Spor",
            "Spor",
            "https://tv-trtspor1.medya.trt.com.tr/master.m3u8",
            "https://www.trtspor.com.tr/canli-yayin-izle/trt-spor",
            "https://trt.daioncdn.net/trtspor/master.m3u8?app=web&platform=trtspor",
            "",
            "",
        },
        {
            "TRTSpor2.tr",
            "TRT Spor Yıldız",
            "Spor",
            "https://tv-trtspor2.medya.trt.com.tr/master.m3u8",
            "https://www.trtizle.com/canli/tv/trt-spor-yildiz",
            "https://trt.daioncdn.net/trtspor-yildiz/master.m3u8?app=web&platform=trtspor",
            "",
            "",
        },
        // --- Belgesel (25-26) ---
        {
            "TRTBelgesel.tr",
            "TRT Belgesel",
            "Belgesel",
            "https://tv-trtbelgesel.medya.trt.com.tr/master.m3u8",
            "https://www.trtizle.com/canli/tv/trt-belgesel",
            "",
            "",
            "",
        },
        {
            "DMAX.tr",
            "DMAX",
            "Belgesel",
            "https://dogus.daioncdn.net/dmax/dmax.m3u8?app=dmax_web&ce=3",
            "https://www.dmax.com.tr/canli-izle",
            "",
            "",
            "",
        },
        // --- Çocuk (27-30) ---
        {
            "TRTCocuk.tr",
            "TRT Çocuk",
            "Çocuk",
            "https://tv-trtcocuk.medya.trt.com.tr/master.m3u8",
            "https://www.trtizle.com/canli/tv/trt-cocuk",
            "",
            "",
            "",
        },
        {
            "CartoonNetwork.tr",
            "Cartoon Network Türkiye",
            "Çocuk",
            "",
            "https://www.cartoonnetwork.com.tr/canli-yayin",
            "https://www.youtube.com/@cartoonnetworkturkiye/live",
            "",
            "",
        },
        {
            "MinikaGO.tr",
            "MinikaGO",
            "Çocuk",
            "",
            "https://www.minikago.com.tr/webtv/canli-yayin",
            "https://securevideotoken.tmgrup.com.tr/webtv/secure?url=https%3A%2F%2Ftrkvz-live.ercdn.net%2Fminikago%2Fminikago.m3u8",
            "https://rnttwmjcin.turknet.ercdn.net/lcpmvefbyo/minikago/minikago.m3u8",
            "",
        },
        {
            "MinikaCocuk.tr",
            "MinikaÇOCUK",
            "Çocuk",
            "",
            "https://www.minikacocuk.com.tr/webtv/canli-yayin",
            "https://securevideotoken.tmgrup.com.tr/webtv/secure?url=https%3A%2F%2Ftrkvz-live.ercdn.net%2Fminikagococuk%2Fminikagococuk.m3u8",
            "https://rnttwmjcin.turknet.ercdn.net/lcpmvefbyo/minikago_cocuk/minikago_cocuk.m3u8",
            "",
        },
        // --- Kültür (31) ---
        {
            "TRT2.tr",
            "TRT 2",
            "Kültür",
            "https://tv-trt2.medya.trt.com.tr/master.m3u8",
            "https://www.trtizle.com/canli/tv/trt-2",
            "",
            "",
            "",
        },
    };

    std::vector<ChannelEntry> out;
    out.reserve(sizeof(kSeeds) / sizeof(kSeeds[0]));
    int num = 1;
    for (const Seed &s : kSeeds) {
        const std::string rawStream = StreamSecurity::trimAscii(s.streamUrl);
        const std::string rawPage = StreamSecurity::trimAscii(s.sourcePageUrl);
        const std::string validStream = StreamSecurity::isAllowedMediaUrl(rawStream) ? rawStream : std::string();
        const std::string validPage = StreamSecurity::isAllowedMediaUrl(rawPage) ? rawPage : std::string();
        if (validStream.empty() && validPage.empty()) {
            continue;
        }
        ChannelEntry ch;
        ch.number = num++;
        ch.id = s.tvgId;
        ch.name = s.name;
        ch.group = s.group;
        ch.streamUrl = validStream;
        ch.sourcePageUrl = validPage;
        for (const char *fbRaw : {s.fallback1, s.fallback2, s.fallback3}) {
            const std::string fb = StreamSecurity::trimAscii(fbRaw);
            if (!fb.empty() && StreamSecurity::isAllowedMediaUrl(fb)) {
                if (std::find(ch.fallbackUrls.begin(), ch.fallbackUrls.end(), fb) == ch.fallbackUrls.end()) {
                    ch.fallbackUrls.push_back(fb);
                }
            }
        }
        out.push_back(std::move(ch));
    }
    return out;
}

std::vector<std::string> ChannelCatalog::uiCategories()
{
    return {"Haber", "Tümü", "Ulusal", "Belgesel", "Çocuk", "Spor", "Kültür"};
}

std::vector<int> ChannelCatalog::filterIndicesByCategory(const std::vector<ChannelEntry> &channels,
                                                         const std::string &category)
{
    std::vector<int> indices;
    const bool showAll = category.empty() || category == "Tümü";
    for (std::size_t i = 0; i < channels.size(); ++i) {
        if (showAll || channels[i].group == category) {
            indices.push_back(static_cast<int>(i));
        }
    }
    return indices;
}

int ChannelCatalog::findIndexById(const std::vector<ChannelEntry> &channels, const std::string &id)
{
    if (id.empty()) {
        return -1;
    }
    for (std::size_t i = 0; i < channels.size(); ++i) {
        if (channels[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int ChannelCatalog::findStartupChannelIndex(const std::vector<ChannelEntry> &channels,
                                            const PortableState &state)
{
    if (channels.empty()) {
        return -1;
    }
    const int saved = findIndexById(channels, state.lastChannelId);
    if (saved >= 0) {
        return saved;
    }
    // First run default: first channel in Haber category (TRT Haber).
    const auto haberIndices = filterIndicesByCategory(channels, "Haber");
    if (!haberIndices.empty()) {
        return haberIndices.front();
    }
    return 0;
}

std::string ChannelCatalog::serializeStateJson(const PortableState &state)
{
    std::ostringstream oss;
    oss << "{\n"
        << "  \"version\": 1,\n"
        << "  \"selectedCategory\": \"" << jsonEscape(state.selectedCategory) << "\",\n"
        << "  \"lastChannelId\": \"" << jsonEscape(state.lastChannelId) << "\",\n"
        << "  \"volume\": " << std::clamp(state.volume, 0, 100) << ",\n"
        << "  \"muted\": " << (state.muted ? "true" : "false") << ",\n"
        << "  \"hlsBitrateCap\": " << (state.hlsBitrateCap ? "true" : "false") << "\n"
        << "}\n";
    return oss.str();
}

PortableState ChannelCatalog::parseStateJson(const std::string &jsonText)
{
    PortableState st;
    std::string cat;
    if (extractJsonString(jsonText, "selectedCategory", cat)) {
        const auto cats = uiCategories();
        if (std::find(cats.begin(), cats.end(), cat) != cats.end()) {
            st.selectedCategory = cat;
        }
    }
    extractJsonString(jsonText, "lastChannelId", st.lastChannelId);
    int vol = 80;
    if (extractJsonInt(jsonText, "volume", vol)) {
        st.volume = std::clamp(vol, 0, 100);
    }
    extractJsonBool(jsonText, "muted", st.muted);
    extractJsonBool(jsonText, "hlsBitrateCap", st.hlsBitrateCap);
    return st;
}

PortableState ChannelCatalog::loadStateFromFile(const std::string &path)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        return PortableState{};
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return parseStateJson(ss.str());
}

bool ChannelCatalog::saveStateToFile(const std::string &path, const PortableState &state)
{
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        return false;
    }
    const std::string content = serializeStateJson(state);
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    return ofs.good();
}

} // namespace zethropol::tvwin7
