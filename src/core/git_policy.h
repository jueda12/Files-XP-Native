#pragma once

#include <array>
#include <cstdint>
#include <cwctype>
#include <span>
#include <string_view>

namespace filesxp::core
{
    inline constexpr std::size_t maxGitInputLength = 4096;
    inline constexpr std::uint64_t gitStatusRefreshIntervalMs = 1500;

    enum class GitOperation : std::uint32_t
    {
        init,
        status,
        fetch,
        pull,
        push,
        sync,
        count
    };

    [[nodiscard]] inline std::span<const wchar_t* const> gitArguments(GitOperation operation) noexcept
    {
        static constexpr std::array<const wchar_t*, 1> init{L"init"};
        static constexpr std::array<const wchar_t*, 10> status{
            L"-c", L"core.quotepath=false", L"-c", L"status.relativePaths=true",
            L"status", L"--porcelain=v1", L"-z", L"--branch", L"--untracked-files=normal",
            L"--no-renames"};
        static constexpr std::array<const wchar_t*, 4> fetch{
            L"fetch", L"--all", L"--prune", L"--progress"};
        static constexpr std::array<const wchar_t*, 3> pull{L"pull", L"--ff-only", L"--progress"};
        static constexpr std::array<const wchar_t*, 2> push{L"push", L"--progress"};
        switch (operation)
        {
        case GitOperation::init: return init;
        case GitOperation::status: return status;
        case GitOperation::fetch: return fetch;
        case GitOperation::pull: return pull;
        case GitOperation::push: return push;
        case GitOperation::sync:
        case GitOperation::count:
            return {};
        }
        return {};
    }

    [[nodiscard]] inline bool validGitBranchName(std::wstring_view value) noexcept
    {
        if (value.empty() || value.size() > 255 || value.front() == L'-' ||
            value.front() == L'/' || value.back() == L'/' || value.back() == L'.' ||
            value.find(L"..") != std::wstring_view::npos ||
            value.find(L"@{") != std::wstring_view::npos ||
            value.find(L"//") != std::wstring_view::npos)
        {
            return false;
        }
        for (wchar_t character : value)
        {
            if (character < 32 || character == 127 || std::iswspace(character) != 0 ||
                character == L'~' || character == L'^' || character == L':' ||
                character == L'?' || character == L'*' || character == L'[' ||
                character == L'\\')
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] inline bool validGitRepositoryInput(std::wstring_view value) noexcept
    {
        if (value.empty() || value.size() > maxGitInputLength)
        {
            return false;
        }
        for (wchar_t character : value)
        {
            if (character < 32 || character == 127)
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] inline bool shouldRefreshGitStatus(std::wstring_view previousDirectory,
        std::wstring_view currentDirectory, std::uint64_t previousTick,
        std::uint64_t currentTick) noexcept
    {
        if (currentDirectory.empty()) return false;
        if (previousDirectory != currentDirectory) return true;
        if (currentTick < previousTick) return true;
        return currentTick - previousTick >= gitStatusRefreshIntervalMs;
    }
}
