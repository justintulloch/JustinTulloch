// profile_card.cpp
//
// Generates assets/activity-card.svg for the GitHub profile README.
// Pulls:
//   - lines changed this week (GitHub REST API, across configured repos)
//   - current weather for York, PA (Open-Meteo, no API key required)
//   - this week's objectives (read from objectives.txt, edited by hand)
//
// Build (matches what the GitHub Action installs):
//   g++ -std=c++17 profile_card.cpp -o profile_card -lcurl
//
// Usage:
//   GITHUB_TOKEN=... ./profile_card <github_username> <repo1,repo2,...> <objectives_file> <output_svg_path>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

// ---------- HTTP ----------

size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buffer = static_cast<std::string*>(userdata);
    buffer->append(ptr, size * nmemb);
    return size * nmemb;
}

// Performs a GET request. `headers` are passed verbatim, e.g. "Authorization: Bearer X".
// Returns the response body. Throws std::runtime_error on transport failure.
std::string httpGet(const std::string& url, const std::vector<std::string>& headers = {}) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("failed to init curl");
    }

    std::string response;
    struct curl_slist* headerList = nullptr;
    for (const auto& h : headers) {
        headerList = curl_slist_append(headerList, h.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "profile-card-generator/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    if (headerList) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
    }

    CURLcode res = curl_easy_perform(curl);

    long statusCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &statusCode);

    if (headerList) {
        curl_slist_free_all(headerList);
    }
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        throw std::runtime_error(std::string("curl error: ") + curl_easy_strerror(res));
    }
    if (statusCode >= 400) {
        throw std::runtime_error("HTTP " + std::to_string(statusCode) + " from " + url);
    }
    return response;
}

// ---------- date helpers ----------

// Returns an ISO-8601 UTC timestamp for `daysAgo` days before now, e.g. "2026-06-15T00:00:00Z".
std::string isoTimestampDaysAgo(int daysAgo) {
    using namespace std::chrono;
    auto now = system_clock::now() - hours(24 * daysAgo);
    std::time_t t = system_clock::to_time_t(now);
    std::tm tmUtc{};
    gmtime_r(&t, &tmUtc);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT00:00:00Z", &tmUtc);
    return std::string(buf);
}

std::string currentDateRangeLabel() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto weekAgo = now - hours(24 * 7);

    std::time_t tNow = system_clock::to_time_t(now);
    std::time_t tWeekAgo = system_clock::to_time_t(weekAgo);
    std::tm tmNow{}, tmWeekAgo{};
    gmtime_r(&tNow, &tmNow);
    gmtime_r(&tWeekAgo, &tmWeekAgo);

    char bufStart[16], bufEnd[16];
    std::strftime(bufStart, sizeof(bufStart), "%b %d", &tmWeekAgo);
    std::strftime(bufEnd, sizeof(bufEnd), "%b %d", &tmNow);
    return std::string(bufStart) + " - " + std::string(bufEnd);
}

// ---------- GitHub: lines changed this week ----------

// Sums additions+deletions across the given repos for commits authored by `username`
// in the last 7 days. Requires a GitHub token with repo read access (set via GITHUB_TOKEN).
int linesChangedThisWeek(const std::string& username,
                          const std::vector<std::string>& repos,
                          const std::string& token) {
    int totalLines = 0;
    std::string since = isoTimestampDaysAgo(7);
    std::vector<std::string> headers = {
        "Authorization: Bearer " + token,
        "Accept: application/vnd.github+json",
        "X-GitHub-Api-Version: 2022-11-28",
    };

    for (const auto& repo : repos) {
        std::string url = "https://api.github.com/repos/" + repo +
                           "/commits?author=" + username + "&since=" + since + "&per_page=100";

        std::string body;
        try {
            body = httpGet(url, headers);
        } catch (const std::exception& e) {
            std::cerr << "warning: could not list commits for " << repo << ": " << e.what() << "\n";
            continue;
        }

        json commits = json::parse(body, nullptr, false);
        if (!commits.is_array()) {
            std::cerr << "warning: unexpected response listing commits for " << repo << "\n";
            continue;
        }

        for (const auto& commit : commits) {
            if (!commit.contains("sha")) continue;
            std::string sha = commit["sha"].get<std::string>();

            std::string statUrl = "https://api.github.com/repos/" + repo + "/commits/" + sha;
            try {
                std::string statBody = httpGet(statUrl, headers);
                json statJson = json::parse(statBody, nullptr, false);
                if (statJson.contains("stats") && statJson["stats"].contains("total")) {
                    totalLines += statJson["stats"]["total"].get<int>();
                }
            } catch (const std::exception& e) {
                std::cerr << "warning: could not fetch stats for " << repo << "@" << sha << ": "
                          << e.what() << "\n";
            }
        }
    }
    return totalLines;
}

// ---------- weather ----------

struct Weather {
    double tempC = 0.0;
    bool ok = false;
};

// York, PA coordinates. Change these constants if you relocate.
constexpr double kLatitude = 39.9626;
constexpr double kLongitude = -76.7277;

Weather fetchWeather() {
    Weather result;
    std::ostringstream url;
    url << "https://api.open-meteo.com/v1/forecast?latitude=" << kLatitude
        << "&longitude=" << kLongitude << "&current=temperature_2m";

    try {
        std::string body = httpGet(url.str());
        json data = json::parse(body, nullptr, false);
        if (data.contains("current") && data["current"].contains("temperature_2m")) {
            result.tempC = data["current"]["temperature_2m"].get<double>();
            result.ok = true;
        }
    } catch (const std::exception& e) {
        std::cerr << "warning: weather fetch failed: " << e.what() << "\n";
    }
    return result;
}

// ---------- objectives file ----------

// Reads non-empty, non-comment lines from a plain text file, one objective per line.
// Lines starting with '#' are treated as comments and skipped.
std::vector<std::string> readObjectives(const std::string& path) {
    std::vector<std::string> objectives;
    std::ifstream file(path);
    if (!file) {
        std::cerr << "warning: could not open objectives file at " << path << "\n";
        return objectives;
    }

    std::string line;
    while (std::getline(file, line)) {
        // trim whitespace
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        size_t end = line.find_last_not_of(" \t\r\n");
        line = line.substr(start, end - start + 1);

        if (line.empty() || line[0] == '#') continue;
        objectives.push_back(line);
    }
    return objectives;
}

// ---------- SVG escaping ----------

std::string escapeXml(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (char c : input) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

// ---------- SVG rendering ----------

void renderSvg(int lines, const Weather& weather, const std::vector<std::string>& objectives,
               const std::string& outputPath) {
    std::ostringstream svg;

    svg << R"(<svg width="700" height="220" viewBox="0 0 700 220" xmlns="http://www.w3.org/2000/svg" role="img">)";
    svg << R"(<title>Weekly activity card</title>)";
    svg << R"(<desc>Lines changed this week, current weather, and this week's objectives</desc>)";

    // Lines changed card
    svg << R"(<rect x="0" y="0" width="335" height="100" rx="16" fill="#F4F3F8"/>)";
    svg << R"(<text x="24" y="30" font-family="Helvetica, Arial, sans-serif" font-size="13" fill="#6B6B75">Lines changed</text>)";
    svg << R"(<text x="24" y="68" font-family="Helvetica, Arial, sans-serif" font-size="32" font-weight="600" fill="#1A1A1F">)"
        << lines << "</text>";
    svg << R"(<text x="24" y="90" font-family="Helvetica, Arial, sans-serif" font-size="11" fill="#8B8B95">this week</text>)";
    svg << R"(<circle cx="280" cy="55" r="28" fill="#E8855A"/>)";
    svg << R"(<circle cx="280" cy="55" r="28" fill="#C77DE0" opacity="0.25"/>)";

    // Weather card
    svg << R"(<rect x="0" y="112" width="335" height="100" rx="16" fill="#E8693A"/>)";
    svg << R"(<text x="24" y="138" font-family="Helvetica, Arial, sans-serif" font-size="12" fill="#FFE8DD">York, Pennsylvania</text>)";
    if (weather.ok) {
        std::ostringstream tempStr;
        tempStr.precision(0);
        tempStr << std::fixed << weather.tempC;
        svg << R"(<text x="24" y="178" font-family="Helvetica, Arial, sans-serif" font-size="28" font-weight="600" fill="#FFFFFF">)"
            << tempStr.str() << " C</text>";
    } else {
        svg << R"(<text x="24" y="178" font-family="Helvetica, Arial, sans-serif" font-size="28" font-weight="600" fill="#FFFFFF">-- C</text>)";
    }

    // Objectives card
    svg << R"(<rect x="351" y="0" width="349" height="212" rx="16" fill="#E7E9F2"/>)";
    svg << R"(<text x="375" y="30" font-family="Helvetica, Arial, sans-serif" font-size="14" font-weight="500" fill="#1A1A1F">This week's objectives</text>)";
    svg << R"(<text x="676" y="30" text-anchor="end" font-family="Helvetica, Arial, sans-serif" font-size="11" fill="#8B8B95">)"
        << currentDateRangeLabel() << "</text>";

    if (objectives.empty()) {
        svg << R"(<text x="375" y="68" font-family="Helvetica, Arial, sans-serif" font-size="12" fill="#8B8B95">No objectives set</text>)";
    } else {
        constexpr int kMaxObjectives = 3;
        constexpr size_t kMaxChars = 42;
        int rowTop = 50;
        constexpr int kRowHeight = 54;
        for (size_t i = 0; i < objectives.size() && i < kMaxObjectives; ++i) {
            std::string text = objectives[i];
            if (text.size() > kMaxChars) {
                text = text.substr(0, kMaxChars - 1) + "…";
            }
            int numberBaseline = rowTop + 18;
            svg << R"(<text x="375" y=")" << numberBaseline
                << R"(" font-family="Helvetica, Arial, sans-serif" font-size="13" fill="#1A1A1F">)"
                << (i + 1) << ". " << escapeXml(text) << "</text>";

            if (i + 1 < objectives.size() && i + 1 < kMaxObjectives) {
                int dividerY = rowTop + kRowHeight - 12;
                svg << R"(<line x1="375" y1=")" << dividerY
                    << R"(" x2="676" y2=")" << dividerY << R"(" stroke="#D3D5E0" stroke-width="1"/>)";
            }
            rowTop += kRowHeight;
        }
    }

    svg << "</svg>";

    std::ofstream out(outputPath);
    if (!out) {
        throw std::runtime_error("could not write to " + outputPath);
    }
    out << svg.str();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "usage: " << argv[0]
                  << " <github_username> <repo1,repo2,...> <objectives_file> <output_svg_path>\n";
        return 1;
    }

    std::string username = argv[1];
    std::string repoListRaw = argv[2];
    std::string objectivesPath = argv[3];
    std::string outputPath = argv[4];

    const char* tokenEnv = std::getenv("GITHUB_TOKEN");
    std::string token = tokenEnv ? tokenEnv : "";
    if (token.empty()) {
        std::cerr << "warning: GITHUB_TOKEN not set, commit stats will be skipped\n";
    }

    std::vector<std::string> repos;
    std::stringstream ss(repoListRaw);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) repos.push_back(item);
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    int lines = 0;
    if (!token.empty()) {
        try {
            lines = linesChangedThisWeek(username, repos, token);
        } catch (const std::exception& e) {
            std::cerr << "warning: failed to compute lines changed: " << e.what() << "\n";
        }
    }

    Weather weather = fetchWeather();
    std::vector<std::string> objectives = readObjectives(objectivesPath);

    try {
        renderSvg(lines, weather, objectives, outputPath);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        curl_global_cleanup();
        return 1;
    }

    curl_global_cleanup();
    std::cout << "wrote " << outputPath << " (lines=" << lines
              << ", temp=" << (weather.ok ? std::to_string(weather.tempC) : "unavailable")
              << ", objectives=" << objectives.size() << ")\n";
    return 0;
}
