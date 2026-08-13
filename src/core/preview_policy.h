#pragma once

#include <algorithm>
#include <cwctype>
#include <string>
#include <string_view>

namespace filesxp::core
{
    // ponytail: The fallback is a viewport preview, not an editor. Keeping the synchronous
    // EDIT-control payload at 256 KiB bounds line-index/layout work on the UI thread.
    inline constexpr std::size_t maxTextPreviewBytes = 256 * 1024;
    inline constexpr std::size_t maxTextPreviewCharacters = maxTextPreviewBytes + 1;
    inline constexpr std::wstring_view textPreviewTruncationNotice =
        L"\r\n\r\n[Preview truncated at 256 KiB; open the file to read all content.]";

    [[nodiscard]] inline bool supportsTextPreview(std::wstring_view path)
    {
        const std::size_t slash = path.find_last_of(L"\\/");
        const std::size_t dot = path.find_last_of(L'.');
        if (dot == std::wstring_view::npos || (slash != std::wstring_view::npos && dot < slash))
            return false;
        std::wstring extension(path.substr(dot));
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
        constexpr std::wstring_view extensions[] = {
            L".txt", L".md", L".markdown", L".json", L".xml", L".yaml", L".yml",
            L".ini", L".log", L".csv", L".tsv", L".c", L".h", L".cpp", L".hpp",
            L".rs", L".py", L".js", L".ts", L".css", L".html", L".htm", L".ps1",
            L".bat", L".cmd", L".toml", L".sql", L".cs", L".xaml", L".rc",
            L".cmake", L".config", L".properties", L".gitignore"};
        return std::find(std::begin(extensions), std::end(extensions), extension) !=
            std::end(extensions);
    }
}
