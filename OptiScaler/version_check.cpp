#include "pch.h"

#include "version_check.h"

#include "State.h"
#include "resource.h"

#define HELIXSR_SCALER_REPO_W L"lonewolf0622/HelixSR-Scaler" // HelixSR Scaler

#include <json.hpp>

#include <winhttp.h>

#include <charconv>
#include <optional>
#include <string_view>
#include <thread>
#include <mutex>
#include <format>

namespace
{
struct SemanticVersion
{
    int major = 0;
    int minor = 0;
    int patch = 0;
};

struct LatestReleaseInfo
{
    std::string tag;
    std::string url;
};

// HelixSR Scaler: its own version and releases
SemanticVersion CurrentVersion()
{
    return { HELIXSR_SCALER_VERSION_MAJOR, HELIXSR_SCALER_VERSION_MINOR, HELIXSR_SCALER_VERSION_PATCH };
}

std::optional<SemanticVersion> ParseVersionString(std::string_view version)
{
    if (version.empty())
        return std::nullopt;

    if (version.front() == 'v' || version.front() == 'V')
        version.remove_prefix(1);

    const auto suffixPos = version.find_first_of("-+ ");
    if (suffixPos != std::string_view::npos)
        version = version.substr(0, suffixPos);

    SemanticVersion parsed {};
    int components[3] = { 0, 0, 0 };
    size_t componentIndex = 0;
    size_t begin = 0;

    while (begin <= version.size() && componentIndex < 3)
    {
        const size_t end = version.find('.', begin);
        const auto part = version.substr(begin, (end == std::string_view::npos ? version.size() : end) - begin);

        if (part.empty())
            return std::nullopt;

        int value = 0;
        const auto result = std::from_chars(part.data(), part.data() + part.size(), value);
        if (result.ec != std::errc())
            return std::nullopt;

        components[componentIndex++] = value;

        if (end == std::string_view::npos)
            break;

        begin = end + 1;
    }

    parsed.major = components[0];
    parsed.minor = components[1];
    parsed.patch = components[2];

    return parsed;
}

int CompareVersions(const SemanticVersion& lhs, const SemanticVersion& rhs)
{
    if (lhs.major != rhs.major)
        return lhs.major < rhs.major ? -1 : 1;
    if (lhs.minor != rhs.minor)
        return lhs.minor < rhs.minor ? -1 : 1;
    if (lhs.patch != rhs.patch)
        return lhs.patch < rhs.patch ? -1 : 1;
    return 0;
}

std::optional<LatestReleaseInfo> FetchLatestRelease()
{
    HINTERNET session = nullptr;
    HINTERNET connection = nullptr;
    HINTERNET request = nullptr;

    auto cleanup = [&]()
    {
        if (request != nullptr)
        {
            WinHttpCloseHandle(request);
            request = nullptr;
        }
        if (connection != nullptr)
        {
            WinHttpCloseHandle(connection);
            connection = nullptr;
        }
        if (session != nullptr)
        {
            WinHttpCloseHandle(session);
            session = nullptr;
        }
    };

    session = WinHttpOpen(L"OptiScaler Version Check/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                          WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr)
    {
        LOG_WARN("Version check failed to open WinHTTP session: {}", GetLastError());
        cleanup();
        return std::nullopt;
    }

    WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);

    connection = WinHttpConnect(session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (connection == nullptr)
    {
        LOG_WARN("Version check failed to connect: {}", GetLastError());
        cleanup();
        return std::nullopt;
    }

    request = WinHttpOpenRequest(connection, L"GET", L"/repos/" HELIXSR_SCALER_REPO_W L"/releases/latest", nullptr,
                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (request == nullptr)
    {
        LOG_WARN("Version check failed to open request: {}", GetLastError());
        cleanup();
        return std::nullopt;
    }

    LPCWSTR headers =
        L"User-Agent: HelixSR-Scaler\r\nAccept: application/vnd.github+json\r\nAccept-Encoding: identity\r\n";
    if (!WinHttpSendRequest(request, headers, (DWORD) -1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
    {
        LOG_WARN("Version check failed to send request: {}", GetLastError());
        cleanup();
        return std::nullopt;
    }

    if (!WinHttpReceiveResponse(request, nullptr))
    {
        LOG_WARN("Version check failed to receive response: {}", GetLastError());
        cleanup();
        return std::nullopt;
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX))
    {
        LOG_WARN("Version check failed to read status code: {}", GetLastError());
        cleanup();
        return std::nullopt;
    }

    if (statusCode != HTTP_STATUS_OK)
    {
        LOG_WARN("Version check returned HTTP status {}", statusCode);
        cleanup();
        return std::nullopt;
    }

    std::string response;
    DWORD available = 0;
    do
    {
        if (!WinHttpQueryDataAvailable(request, &available))
        {
            LOG_WARN("Version check failed querying data availability: {}", GetLastError());
            cleanup();
            return std::nullopt;
        }

        if (available == 0)
            break;

        std::string buffer;
        buffer.resize(available);
        DWORD downloaded = 0;
        if (!WinHttpReadData(request, buffer.data(), available, &downloaded))
        {
            LOG_WARN("Version check failed reading response: {}", GetLastError());
            cleanup();
            return std::nullopt;
        }

        response.append(buffer.data(), downloaded);
    } while (available > 0);

    cleanup();

    try
    {
        auto json = nlohmann::json::parse(response);
        LatestReleaseInfo info;
        info.tag = json.value("tag_name", std::string {});
        info.url = json.value("html_url", std::string {});

        if (info.tag.empty())
            return std::nullopt;

        return info;
    }
    catch (const std::exception& ex)
    {
        LOG_WARN("Version check failed to parse response: {}", ex.what());
        return std::nullopt;
    }
}

void FinishVersionCheck()
{
    auto& state = State::Instance();
    std::scoped_lock lock(state.versionCheckMutex);
    state.versionCheckInProgress = false;
    state.versionCheckCompleted = true;
}

void RunVersionCheck()
{
    struct Finalizer
    {
        ~Finalizer() { FinishVersionCheck(); }
    } finalize;

    auto release = FetchLatestRelease();
    if (!release.has_value())
    {
        auto& state = State::Instance();
        std::scoped_lock lock(state.versionCheckMutex);
        state.versionCheckError = "Unable to check for updates.";
        state.updateAvailable = false;
        return;
    }

    const auto remoteVersion = ParseVersionString(release->tag);

    auto& state = State::Instance();
    {
        std::scoped_lock lock(state.versionCheckMutex);
        state.latestVersionTag = release->tag;
        state.latestVersionUrl = release->url;
    }

    if (!remoteVersion.has_value())
    {
        LOG_WARN("Version check received unrecognized tag format: {}", release->tag);
        std::scoped_lock lock(state.versionCheckMutex);
        state.versionCheckError = "Received an unknown version format from update server.";
        state.updateAvailable = false;
        return;
    }

    const auto localVersion = CurrentVersion();
    const bool updateAvailable = CompareVersions(remoteVersion.value(), localVersion) > 0;

    {
        std::scoped_lock lock(state.versionCheckMutex);
        state.updateAvailable = updateAvailable;
        state.versionCheckError.clear();
    }

    if (updateAvailable)
    {
        LOG_WARN("New OptiScaler release available: {} (current {}.{}.{}).", release->tag, VER_MAJOR_VERSION,
                 VER_MINOR_VERSION, VER_HOTFIX_VERSION);
    }
    else
    {
        LOG_INFO("OptiScaler is up to date (current {}.{}.{})", VER_MAJOR_VERSION, VER_MINOR_VERSION,
                 VER_HOTFIX_VERSION);
    }
}
} // namespace

const std::string& VersionCheck::CurrentVersionString()
{
    static const std::string version =
        std::format("{}.{}.{}", HELIXSR_SCALER_VERSION_MAJOR, HELIXSR_SCALER_VERSION_MINOR,
                    HELIXSR_SCALER_VERSION_PATCH); // HelixSR Scaler
    return version;
}

void VersionCheck::Start()
{
    auto& state = State::Instance();
    {
        std::scoped_lock lock(state.versionCheckMutex);
        if (state.versionCheckInProgress || state.versionCheckCompleted)
            return;

        state.versionCheckInProgress = true;
        state.versionCheckCompleted = false;
        state.updateAvailable = false;
        state.versionCheckError.clear();
        state.latestVersionTag.clear();
        state.latestVersionUrl.clear();
    }

    std::thread(
        []()
        {
            try
            {
                RunVersionCheck();
            }
            catch (const std::exception& ex)
            {
                LOG_ERROR("Version check failed with exception: {}", ex.what());
                auto& state = State::Instance();
                std::scoped_lock lock(state.versionCheckMutex);
                state.versionCheckError = "Update check failed.";
                state.updateAvailable = false;
            }
            catch (...)
            {
                LOG_ERROR("Version check failed with unknown exception");
                auto& state = State::Instance();
                std::scoped_lock lock(state.versionCheckMutex);
                state.versionCheckError = "Update check failed.";
                state.updateAvailable = false;
            }
        })
        .detach();
}
