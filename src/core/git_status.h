#pragma once

#include <cwctype>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace filesxp::core
{
    struct GitStatusEntry final
    {
        wchar_t code{};
        std::wstring path;
    };

    using GitDecorationMap = std::map<std::wstring, wchar_t, std::less<>>;

    [[nodiscard]] inline std::wstring normalizedGitPath(std::wstring_view value)
    {
        std::wstring result(value);
        while (result.size() > 3 && (result.back() == L'\\' || result.back() == L'/'))
            result.pop_back();
        for (wchar_t& character : result)
        {
            if (character == L'/') character = L'\\';
            else character = static_cast<wchar_t>(std::towlower(character));
        }
        return result;
    }

    [[nodiscard]] inline unsigned gitStatusPriority(wchar_t status) noexcept
    {
        switch (status)
        {
        case L'U': return 6;
        case L'D': return 5;
        case L'M': return 4;
        case L'A': return 3;
        case L'?': return 2;
        default: return 1;
        }
    }

    [[nodiscard]] inline bool parseGitStatusRecord(std::wstring_view record,
        GitStatusEntry& entry)
    {
        if (record.size() < 4 || record[2] != L' ' || record.starts_with(L"##")) return false;
        const wchar_t index = record[0];
        const wchar_t workTree = record[1];
        wchar_t code = workTree != L' ' ? workTree : index;
        if (index == L'?' && workTree == L'?') code = L'?';
        if (code == L'!' || code == L' ') return false;
        std::wstring path(record.substr(3));
        if (path.empty() || path.size() >= 32767 || path.front() == L'/' || path.front() == L'\\' ||
            path.find(L"../") != std::wstring::npos || path.find(L"..\\") != std::wstring::npos ||
            path.find(L'\0') != std::wstring::npos)
            return false;
        for (wchar_t& character : path)
        {
            if (character == L'/') character = L'\\';
            else character = static_cast<wchar_t>(std::towlower(character));
        }
        while (!path.empty() && path.back() == L'\\') path.pop_back();
        if (path.empty()) return false;
        std::size_t componentStart{};
        while (componentStart <= path.size())
        {
            const std::size_t separator = path.find(L'\\', componentStart);
            const std::wstring_view component(path.data() + componentStart,
                (separator == std::wstring::npos ? path.size() : separator) - componentStart);
            if (component.empty() || component == L"." || component == L"..") return false;
            if (separator == std::wstring::npos) break;
            componentStart = separator + 1;
        }
        entry = GitStatusEntry{code, std::move(path)};
        return true;
    }

    [[nodiscard]] inline std::vector<GitStatusEntry> parseGitStatus(std::wstring_view output)
    {
        std::vector<GitStatusEntry> result;
        std::size_t position{};
        while (position < output.size() && result.size() < 100000)
        {
            const std::size_t end = output.find(L'\0', position);
            std::wstring_view record = output.substr(position,
                end == std::wstring_view::npos ? output.size() - position : end - position);
            position = end == std::wstring_view::npos ? output.size() : end + 1;
            GitStatusEntry entry;
            if (parseGitStatusRecord(record, entry)) result.push_back(std::move(entry));
        }
        return result;
    }

    class GitDecorationBuilder final
    {
    public:
        [[nodiscard]] bool start(std::wstring_view workingDirectory, std::wstring statusOutput)
        {
            cancel();
            root_ = normalizedGitPath(workingDirectory);
            if (root_.empty() || statusOutput.empty()) return false;
            output_ = std::move(statusOutput);
            active_ = true;
            return true;
        }

        void cancel() noexcept
        {
            root_.clear();
            output_.clear();
            ancestorPath_.clear();
            decorations_.clear();
            position_ = 0;
            entryCount_ = 0;
            active_ = false;
        }

        void next(std::size_t maximumWork)
        {
            if (!active_ || maximumWork == 0) return;
            std::size_t work{};
            while (active_ && work < maximumWork)
            {
                if (!ancestorPath_.empty())
                {
                    const std::size_t separator = ancestorPath_.find_last_of(L'\\');
                    if (separator != std::wstring::npos && separator > root_.size())
                    {
                        ancestorPath_.resize(separator);
                        merge(ancestorPath_, L'M');
                    }
                    else
                    {
                        ancestorPath_.clear();
                    }
                    ++work;
                    continue;
                }
                if (position_ >= output_.size() || entryCount_ >= 100000)
                {
                    active_ = false;
                    break;
                }
                const std::size_t end = output_.find(L'\0', position_);
                const std::wstring_view record(output_.data() + position_,
                    (end == std::wstring::npos ? output_.size() : end) - position_);
                position_ = end == std::wstring::npos ? output_.size() : end + 1;
                GitStatusEntry entry;
                if (parseGitStatusRecord(record, entry))
                {
                    ++entryCount_;
                    ancestorPath_ = root_ + L"\\" + entry.path;
                    merge(ancestorPath_, entry.code);
                }
                ++work;
            }
            if (active_ && ancestorPath_.empty() &&
                (position_ >= output_.size() || entryCount_ >= 100000))
                active_ = false;
        }

        [[nodiscard]] bool active() const noexcept { return active_; }

        [[nodiscard]] GitDecorationMap take() noexcept
        {
            active_ = false;
            root_.clear();
            output_.clear();
            ancestorPath_.clear();
            position_ = 0;
            entryCount_ = 0;
            return std::move(decorations_);
        }

    private:
        void merge(const std::wstring& path, wchar_t status)
        {
            const auto existing = decorations_.find(path);
            if (existing == decorations_.end() ||
                gitStatusPriority(status) > gitStatusPriority(existing->second))
                decorations_[path] = status;
        }

        std::wstring root_;
        std::wstring output_;
        std::wstring ancestorPath_;
        GitDecorationMap decorations_;
        std::size_t position_{};
        std::size_t entryCount_{};
        bool active_{};
    };

    [[nodiscard]] inline GitDecorationMap buildGitDecorations(
        std::wstring_view workingDirectory, std::wstring_view statusOutput)
    {
        GitDecorationBuilder builder;
        if (!builder.start(workingDirectory, std::wstring(statusOutput))) return {};
        while (builder.active()) builder.next(4096);
        return builder.take();
    }
}
