#include "../src/core/natural_order.h"
#include "../src/core/ftp_request.h"
#include "../src/core/git_status.h"
#include "../src/core/tag_result_codec.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    std::size_t count = 100000;
    if (argc == 2)
    {
        count = static_cast<std::size_t>(std::stoull(argv[1]));
    }

    std::vector<std::wstring> names;
    names.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto scrambled = (index * 48271u) % std::max<std::size_t>(count, 1);
        names.push_back(L"File-" + std::to_wstring(scrambled) + L".txt");
    }

    const auto started = std::chrono::steady_clock::now();
    std::sort(names.begin(), names.end(), filesxp::core::NaturalLess{});
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();

    if (!std::is_sorted(names.begin(), names.end(), filesxp::core::NaturalLess{}))
    {
        std::cerr << "Sort verification failed.\n";
        return 1;
    }

    std::wstring ftpListing;
    ftpListing.reserve(count * 24);
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto scrambled = (index * 48271u) % std::max<std::size_t>(count, 1);
        ftpListing += L"remote-" + std::to_wstring(scrambled) + L".bin\r\n";
    }
    const auto ftpStarted = std::chrono::steady_clock::now();
    const auto ftpNames = filesxp::core::parseFtpNameList(ftpListing);
    const double ftpElapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - ftpStarted).count();
    if (ftpNames.size() != count ||
        !std::is_sorted(ftpNames.begin(), ftpNames.end(), filesxp::core::NaturalLess{}))
    {
        std::cerr << "FTP listing verification failed.\n";
        return 6;
    }

    filesxp::core::FtpNameListCursor ftpCursor;
    if (!ftpCursor.start(ftpListing))
    {
        std::cerr << "FTP listing cursor did not start.\n";
        return 7;
    }
    std::vector<std::wstring> ftpMaterialized;
    ftpMaterialized.reserve(count);
    double maximumFtpDispatch{};
    std::size_t ftpDispatches{};
    while (ftpCursor.active())
    {
        const auto dispatchStarted = std::chrono::steady_clock::now();
        for (std::size_t line = 0; line < 64 && ftpCursor.active(); ++line)
        {
            std::wstring_view name;
            const auto step = ftpCursor.next(name);
            if (step == filesxp::core::FtpNameListStep::item)
                ftpMaterialized.emplace_back(name);
        }
        maximumFtpDispatch = std::max(maximumFtpDispatch,
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - dispatchStarted).count());
        ++ftpDispatches;
    }
    if (ftpMaterialized.size() != count || ftpCursor.accepted() != count ||
        ftpDispatches == 0 || maximumFtpDispatch > 8.0)
    {
        std::cerr << "FTP listing cursor benchmark failed.\n";
        return 8;
    }

    std::wstring porcelain;
    porcelain.reserve(count * 30);
    for (std::size_t index = 0; index < count; ++index)
    {
        porcelain += L" M folder-" + std::to_wstring(index % 100) +
            L"/file-" + std::to_wstring(index) + L".txt";
        porcelain.push_back(L'\0');
    }
    filesxp::core::GitDecorationBuilder builder;
    if (!builder.start(L"C:\\Bench", std::move(porcelain)))
    {
        std::cerr << "Git decoration builder did not start.\n";
        return 2;
    }
    const auto gitStarted = std::chrono::steady_clock::now();
    double maximumDispatch{};
    double maximumCpuDispatch{};
    std::size_t dispatches{};
    while (builder.active())
    {
        const auto dispatchStarted = std::chrono::steady_clock::now();
        const std::clock_t cpuDispatchStarted = std::clock();
        const auto dispatchDeadline = dispatchStarted + std::chrono::milliseconds(6);
        std::size_t processed{};
        do
        {
            constexpr std::size_t quantum = 2;
            builder.next(std::min(quantum, std::size_t{64} - processed));
            processed += std::min(quantum, std::size_t{64} - processed);
        } while (builder.active() && processed < 64 &&
            std::chrono::steady_clock::now() < dispatchDeadline);
        maximumDispatch = std::max(maximumDispatch,
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - dispatchStarted).count());
        const std::clock_t cpuDispatchFinished = std::clock();
        if (cpuDispatchStarted != static_cast<std::clock_t>(-1) &&
            cpuDispatchFinished != static_cast<std::clock_t>(-1))
            maximumCpuDispatch = std::max(maximumCpuDispatch,
                1000.0 * static_cast<double>(cpuDispatchFinished - cpuDispatchStarted) /
                    static_cast<double>(CLOCKS_PER_SEC));
        ++dispatches;
    }
    const double gitElapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - gitStarted).count();
    const auto decorations = builder.take();
    if (decorations.size() < count || dispatches == 0 || maximumCpuDispatch > 8.0)
    {
        std::cerr << "Git decoration benchmark verification failed.\n";
        return 3;
    }

    std::vector<std::wstring> tagPaths;
    tagPaths.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
        tagPaths.push_back(L"C:\\Tagged\\file-" + std::to_wstring(index) + L".txt");
    const auto encodedTags = filesxp::core::encodeTagResults(tagPaths);
    filesxp::core::TagResultCursor tagCursor;
    if (!tagCursor.start(encodedTags.data(), encodedTags.size()))
    {
        std::cerr << "Tag result cursor did not start.\n";
        return 4;
    }
    const auto tagStarted = std::chrono::steady_clock::now();
    double maximumTagDispatch{};
    std::size_t tagDispatches{};
    while (tagCursor.active())
    {
        const auto dispatchStarted = std::chrono::steady_clock::now();
        for (std::size_t batch = 0; batch < 32; ++batch)
        {
            std::wstring_view path;
            if (tagCursor.next(path) != filesxp::core::TagResultStep::item) break;
        }
        maximumTagDispatch = std::max(maximumTagDispatch,
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - dispatchStarted).count());
        ++tagDispatches;
    }
    const double tagElapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - tagStarted).count();
    if (tagCursor.count() != count || tagDispatches == 0 || maximumTagDispatch > 50.0)
    {
        std::cerr << "Tag result cursor benchmark verification failed.\n";
        return 5;
    }

    std::cout << "items=" << count << '\n'
              << std::fixed << std::setprecision(3)
              << "natural_sort_ms=" << elapsed << '\n'
              << "ftp_validate_sort_ms=" << ftpElapsed << '\n'
              << "ftp_cursor_dispatches=" << ftpDispatches << '\n'
              << "ftp_cursor_max_dispatch_ms=" << maximumFtpDispatch << '\n'
              << "git_decoration_total_ms=" << gitElapsed << '\n'
              << "git_decoration_dispatches=" << dispatches << '\n'
              << "git_decoration_max_dispatch_ms=" << maximumDispatch << '\n'
              << "git_decoration_max_cpu_dispatch_ms=" << maximumCpuDispatch << '\n'
              << "tag_cursor_total_ms=" << tagElapsed << '\n'
              << "tag_cursor_dispatches=" << tagDispatches << '\n'
              << "tag_cursor_max_dispatch_ms=" << maximumTagDispatch << '\n';
}

