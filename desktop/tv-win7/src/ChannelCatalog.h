#pragma once

#include <string>
#include <vector>

namespace zethropol::tvwin7 {

struct ChannelEntry
{
    int number = 0;
    std::string id;            // stable tvgId (e.g. "TRTHaber.tr")
    std::string name;          // UTF-8 Turkish display name
    std::string group;         // "Ulusal", "Haber", "Spor", "Belgesel", "Çocuk", "Kültür"
    std::string streamUrl;     // Direct HTTPS HLS endpoint (optional)
    std::string sourcePageUrl; // Official HTTPS broadcaster live page / API
    std::vector<std::string> fallbackUrls;
};

struct PortableState
{
    std::string selectedCategory = "Haber";
    std::string lastChannelId;
    int volume = 80;
    bool muted = false;
    bool hlsBitrateCap = true; // true = 2500000 bps (~720p Atom/Win7), false = max (Tam Kalite)
};

class ChannelCatalog
{
public:
    static std::vector<ChannelEntry> buildTurkish31Channels();
    static std::vector<std::string> uiCategories();
    static std::vector<int> filterIndicesByCategory(const std::vector<ChannelEntry> &channels,
                                                    const std::string &category);
    static int findIndexById(const std::vector<ChannelEntry> &channels, const std::string &id);
    static int findStartupChannelIndex(const std::vector<ChannelEntry> &channels,
                                       const PortableState &state);

    static std::string serializeStateJson(const PortableState &state);
    static PortableState parseStateJson(const std::string &jsonText);
    static PortableState loadStateFromFile(const std::string &path);
    static bool saveStateToFile(const std::string &path, const PortableState &state);
};

} // namespace zethropol::tvwin7
