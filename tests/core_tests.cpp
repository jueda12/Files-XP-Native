#include "../src/core/bounded_queue.h"
#include "../src/core/bulk_rename_request.h"
#include "../src/core/archive_request.h"
#include "../src/core/batch_cursor.h"
#include "../src/core/clipboard_path.h"
#include "../src/core/coalescing_gate.h"
#include "../src/core/copydata_path.h"
#include "../src/core/command_match.h"
#include "../src/core/filename_policy.h"
#include "../src/core/flatten_policy.h"
#include "../src/core/folder_selection_request.h"
#include "../src/core/ftp_request.h"
#include "../src/core/generation_gate.h"
#include "../src/core/git_policy.h"
#include "../src/core/git_status.h"
#include "../src/core/localization_policy.h"
#include "../src/core/locale_pack.h"
#include "../src/core/natural_order.h"
#include "../src/core/preview_policy.h"
#include "../src/core/preview_queue.h"
#include "../src/core/selection_inversion.h"
#include "../src/core/search_request.h"
#include "../src/core/shelf_order.h"
#include "../src/core/session_codec.h"
#include "../src/core/settings_codec.h"
#include "../src/core/shell_operation_request.h"
#include "../src/core/shell_artifact_request.h"
#include "../src/core/shortcut_map.h"
#include "../src/core/tag_codec.h"
#include "../src/core/tag_color.h"
#include "../src/core/tag_identity.h"
#include "../src/core/tag_request.h"
#include "../src/core/tag_result_codec.h"
#include "../src/core/windows_command_line.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace
{
    void generation_gate_rejects_stale_work()
    {
        filesxp::core::GenerationGate gate;
        const auto first = gate.next();
        assert(gate.accepts(first));
        const auto second = gate.next();
        assert(!gate.accepts(first));
        assert(gate.accepts(second));
    }

    void bounded_queue_applies_backpressure()
    {
        filesxp::core::BoundedQueue<int> queue(2);
        assert(queue.try_push(10));
        assert(queue.try_push(20));
        assert(!queue.try_push(30));
        assert(queue.try_pop() == 10);
        assert(queue.try_push(30));
        const auto remaining = queue.drain();
        assert((remaining == std::vector<int>{20, 30}));
    }

    void bounded_queue_survives_concurrent_producers()
    {
        filesxp::core::BoundedQueue<int> queue(1000);
        std::vector<std::thread> producers;
        for (int producer = 0; producer < 4; ++producer)
        {
            producers.emplace_back([producer, &queue]
            {
                for (int item = 0; item < 100; ++item)
                {
                    const bool accepted = queue.try_push(producer * 100 + item);
                    assert(accepted);
                }
            });
        }
        for (auto& producer : producers)
        {
            producer.join();
        }
        assert(queue.size() == 400);
    }

    void natural_order_handles_numbers_and_case()
    {
        std::vector<std::wstring> values{L"file10", L"File2", L"file01", L"file1"};
        std::sort(values.begin(), values.end(), [](const auto& left, const auto& right)
        {
            return filesxp::core::NaturalLess{}(left, right);
        });
        assert((values == std::vector<std::wstring>{L"file1", L"file01", L"File2", L"file10"}));
    }

    void session_codec_round_trips_and_rejects_malformed_state()
    {
        const filesxp::core::SessionSnapshot expected{
            .activeIndex = 1,
            .locations = {L"C:\\Users\\Test\\Documents", L"shell:::{20D04FE0-3AEA-1069-A2D8-08002B30309D}"}};
        const auto encoded = filesxp::core::SessionCodec::encode(expected);
        assert(!encoded.empty());

        filesxp::core::SessionSnapshot actual;
        assert(filesxp::core::SessionCodec::decode(encoded.data(), encoded.size(), actual));
        assert(actual.activeIndex == expected.activeIndex);
        assert(actual.locations == expected.locations);

        auto truncated = encoded;
        truncated.pop_back();
        assert(!filesxp::core::SessionCodec::decode(truncated.data(), truncated.size(), actual));

        filesxp::core::SessionSnapshot tooMany;
        tooMany.locations.assign(filesxp::core::SessionCodec::maxTabs + 1, L"C:\\");
        assert(filesxp::core::SessionCodec::encode(tooMany).empty());

        const wchar_t invalidIndex[] = L"FXP1\0x\0C:\\\0\0";
        assert(!filesxp::core::SessionCodec::decode(invalidIndex, std::size(invalidIndex), actual));
    }

    void windows_arguments_are_quoted_without_shell_interpretation()
    {
        using filesxp::core::quoteWindowsArgument;
        assert(quoteWindowsArgument(L"plain") == L"plain");
        assert(quoteWindowsArgument(L"two words") == L"\"two words\"");
        assert(quoteWindowsArgument(L"C:\\trailing\\") == L"C:\\trailing\\");
        assert(quoteWindowsArgument(L"C:\\two words\\") == L"\"C:\\two words\\\\\"");
        assert(quoteWindowsArgument(L"say\"hello") == L"\"say\\\"hello\"");
        assert(quoteWindowsArgument(L"") == L"\"\"");
    }

    void copydata_paths_reject_ambiguous_embedded_nulls()
    {
        const wchar_t valid[] = L"C:\\Work";
        assert(filesxp::core::validCopyDataPath(valid, std::size(valid)));
        const wchar_t embedded[]{L'C', L':', L'\\', L'a', L'\0', L'b', L'\0'};
        assert(!filesxp::core::validCopyDataPath(embedded, std::size(embedded)));
        assert(!filesxp::core::validCopyDataPath(valid, std::size(valid) - 1));
        assert(!filesxp::core::validCopyDataPath(L"", 1));
    }

    void settings_codec_is_bounded_and_rejects_unknown_flags()
    {
        filesxp::core::AppSettings expected;
        expected.locale = filesxp::core::Locale::traditionalChinese;
        expected.set(filesxp::core::compactToolbar, true);
        expected.defaultView = 4;
        expected.toolbarButtons = 0x15;
        expected.shortcuts[0] = filesxp::core::shortcutControl | filesxp::core::shortcutAlt | 'T';
        expected.previewProvider = filesxp::core::PreviewProvider::seer;
        expected.startLocation = L"shell:Downloads";
        const auto encoded = filesxp::core::SettingsCodec::encode(expected);
        assert(!encoded.empty());

        filesxp::core::AppSettings actual;
        assert(filesxp::core::SettingsCodec::decode(encoded.data(), encoded.size(), actual));
        assert(actual.locale == expected.locale);
        assert(actual.flags == expected.flags);
        assert(actual.defaultView == expected.defaultView);
        assert(actual.toolbarButtons == expected.toolbarButtons);
        assert(actual.shortcuts == expected.shortcuts);
        assert(actual.previewProvider == expected.previewProvider);
        assert(actual.startLocation == expected.startLocation);

        const wchar_t unknownFlags[] = L"FXS1\0" L"0\0" L"2147483648\0" L"0\0\0";
        assert(!filesxp::core::SettingsCodec::decode(unknownFlags, std::size(unknownFlags), actual));
        const wchar_t invalidView[] = L"FXS1\0" L"0\0" L"0\0" L"99\0\0";
        assert(!filesxp::core::SettingsCodec::decode(invalidView, std::size(invalidView), actual));

        filesxp::core::AppSettings defaults;
        const auto encodedDefaults = filesxp::core::SettingsCodec::encode(defaults);
        assert(filesxp::core::SettingsCodec::decode(encodedDefaults.data(), encodedDefaults.size(), actual));
        assert(actual.startLocation.empty());
        const wchar_t previousVersion[] = L"FXS1\0" L"0\0" L"123\0" L"2\0" L"C:\\\0\0";
        assert(filesxp::core::SettingsCodec::decode(previousVersion, std::size(previousVersion), actual));
        assert(actual.toolbarButtons == filesxp::core::knownToolbarButtons);
        const wchar_t versionTwo[] = L"FXS2\0" L"0\0" L"123\0" L"2\0" L"21\0" L"-\0\0";
        assert(filesxp::core::SettingsCodec::decode(versionTwo, std::size(versionTwo), actual));
        assert(actual.shortcuts == filesxp::core::defaultShortcuts);
        const wchar_t invalidProvider[] = L"FXS4\0" L"0\0" L"123\0" L"2\0" L"21\0"
            L"65620,65623,65612,65606,196688,196686\0" L"99\0" L"-\0\0";
        assert(!filesxp::core::SettingsCodec::decode(invalidProvider,
            std::size(invalidProvider), actual));
    }

    void system_locale_resolves_chinese_scripts_correctly()
    {
        using filesxp::core::Locale;
        using filesxp::core::resolveLocale;
        assert(resolveLocale(Locale::system, 0x0404) == Locale::traditionalChinese);
        assert(resolveLocale(Locale::system, 0x0c04) == Locale::traditionalChinese);
        assert(resolveLocale(Locale::system, 0x1404) == Locale::traditionalChinese);
        assert(resolveLocale(Locale::system, 0x0804) == Locale::simplifiedChinese);
        assert(resolveLocale(Locale::system, 0x1004) == Locale::simplifiedChinese);
        assert(resolveLocale(Locale::system, 0x0409) == Locale::english);
        assert(resolveLocale(Locale::english, 0x0404) == Locale::english);
    }

    void locale_packs_are_bounded_versioned_and_reject_duplicate_keys()
    {
        std::vector<filesxp::core::LocaleOverride> overrides;
        assert(filesxp::core::parseLocalePack(
            u"FXL1\n# comment\n0=Files XP\n2=Line one\\nLine two\n3=A\\=B\\\\C\n",
            4, overrides));
        assert(overrides.size() == 3);
        assert(overrides[1].index == 2);
        assert(overrides[1].value == u"Line one\r\nLine two");
        assert(overrides[2].value == u"A=B\\C");
        assert(!filesxp::core::parseLocalePack(u"FXL2\n0=no\n", 4, overrides));
        assert(!filesxp::core::parseLocalePack(u"FXL1\n0=one\n0=two\n", 4, overrides));
        assert(!filesxp::core::parseLocalePack(u"FXL1\n4=outside\n", 4, overrides));
        assert(!filesxp::core::parseLocalePack(u"FXL1\n1=bad\\qescape\n", 4, overrides));
    }

    void command_match_is_case_insensitive_and_ordered()
    {
        using filesxp::core::commandMatchScore;
        using filesxp::core::noCommandMatch;
        assert(commandMatchScore(L"New Folder", L"nf") < commandMatchScore(L"New Text File", L"nf"));
        assert(commandMatchScore(L"Git Status", L"GIT") != noCommandMatch);
        assert(commandMatchScore(L"Properties", L"xyz") == noCommandMatch);
        assert(commandMatchScore(L"Anything", L"") == 0);
    }

    void tags_are_trimmed_deduplicated_and_bounded()
    {
        std::vector<std::wstring> tags;
        assert(filesxp::core::normalizeTags(L" Work ; urgent;work ; 2026 ", tags));
        assert((tags == std::vector<std::wstring>{L"Work", L"urgent", L"2026"}));
        assert(filesxp::core::joinTags(tags) == L"Work; urgent; 2026");
        assert(!filesxp::core::normalizeTags(std::wstring(65, L'x'), tags));
        assert(!filesxp::core::normalizeTags(L"bad\"tag", tags));
        std::wstring tooMany;
        for (int index = 0; index < 17; ++index) tooMany += L"t" + std::to_wstring(index) + L';';
        assert(!filesxp::core::normalizeTags(tooMany, tags));
    }

    void shortcut_map_rejects_conflicts_and_unsafe_bare_keys()
    {
        auto shortcuts = filesxp::core::defaultShortcuts;
        assert(filesxp::core::validShortcutMap(shortcuts));
        shortcuts[1] = shortcuts[0];
        assert(!filesxp::core::validShortcutMap(shortcuts));
        shortcuts = filesxp::core::defaultShortcuts;
        shortcuts[0] = 'Q';
        assert(!filesxp::core::validShortcutMap(shortcuts));
        shortcuts[0] = 0x70;
        assert(filesxp::core::validShortcutMap(shortcuts));
        shortcuts = filesxp::core::defaultShortcuts;
        shortcuts[0] = filesxp::core::shortcutControl | 'Y';
        assert(!filesxp::core::validShortcutMap(shortcuts));
        shortcuts[0] = filesxp::core::shortcutControl | filesxp::core::shortcutShift | 0x09;
        assert(!filesxp::core::validShortcutMap(shortcuts));
    }

    void filename_policy_preserves_extensions_and_rejects_windows_devices()
    {
        using filesxp::core::bulkRenameTarget;
        using filesxp::core::validWindowsFilename;
        assert(validWindowsFilename(L"Project"));
        assert(validWindowsFilename(L"Project.v2"));
        assert(!validWindowsFilename(L"CON"));
        assert(!validWindowsFilename(L"con.txt"));
        assert(!validWindowsFilename(L"bad/name"));
        assert(!validWindowsFilename(L"trailing."));
        assert(!validWindowsFilename(L"a.b", false));
        assert(bulkRenameTarget(L"Photo", L"IMG_001.JPG", false) == L"Photo.JPG");
        assert(bulkRenameTarget(L"Photo", L"archive.tar.gz", false) == L"Photo.gz");
        assert(bulkRenameTarget(L"Folder", L"old.name", true) == L"Folder");
        assert(bulkRenameTarget(L"bad.name", L"x.txt", false).empty());
        assert(filesxp::core::validAlternateStreamName(L"notes.txt"));
        assert(!filesxp::core::validAlternateStreamName(L"bad:stream"));
        assert(!filesxp::core::validAlternateStreamName(L"$DATA"));
        assert(!filesxp::core::validAlternateStreamName(L"$data"));
    }

    void flatten_policy_never_descends_into_reparse_points()
    {
        using filesxp::core::FlattenAction;
        using filesxp::core::flattenAction;
        assert(flattenAction(false, false, false, 0) == FlattenAction::ignore);
        assert(flattenAction(true, false, false, 2) == FlattenAction::move);
        assert(flattenAction(false, true, false, 0) == FlattenAction::descend);
        assert(flattenAction(true, true, true, 2) == FlattenAction::move);
        assert(flattenAction(false, true, true, 0) == FlattenAction::ignore);
        assert(flattenAction(true, true, false, filesxp::core::maxFlattenDepth) ==
            FlattenAction::move);
    }

    void git_inputs_are_bounded_and_refs_reject_option_and_revision_syntax()
    {
        using filesxp::core::validGitBranchName;
        using filesxp::core::validGitRepositoryInput;
        assert(validGitBranchName(L"feature/native-ui"));
        assert(!validGitBranchName(L"-danger"));
        assert(!validGitBranchName(L"main..next"));
        assert(!validGitBranchName(L"name@{1}"));
        assert(!validGitBranchName(L"two words"));
        assert(validGitRepositoryInput(L"https://example.invalid/repo.git"));
        assert(validGitRepositoryInput(L"git@example.invalid:team/repo.git"));
        assert(!validGitRepositoryInput(std::wstring(4097, L'x')));

        using filesxp::core::GitOperation;
        const auto status = filesxp::core::gitArguments(GitOperation::status);
        assert(status.size() == 10);
        assert(std::wstring_view(status[0]) == L"-c");
        assert(std::wstring_view(status[3]) == L"status.relativePaths=true");
        assert(std::wstring_view(status[4]) == L"status");
        assert(std::wstring_view(status[6]) == L"-z");
        assert(filesxp::core::gitArguments(GitOperation::pull).size() == 3);
        assert(filesxp::core::gitArguments(GitOperation::push).size() == 2);
        assert(filesxp::core::gitArguments(GitOperation::sync).empty());

        std::wstring porcelain;
        for (std::wstring_view record : {L"## main", L" M src/main.cpp", L"?? New File.txt",
                 L"A  nested/add.cpp"})
        {
            porcelain.append(record);
            porcelain.push_back(L'\0');
        }
        const auto entries = filesxp::core::parseGitStatus(porcelain);
        assert(entries.size() == 3);
        assert(entries[0].code == L'M' && entries[0].path == L"src\\main.cpp");
        assert(entries[1].code == L'?' && entries[1].path == L"new file.txt");
        assert(entries[2].code == L'A' && entries[2].path == L"nested\\add.cpp");

        const auto decorations = filesxp::core::buildGitDecorations(L"C:/Work/Repo/", porcelain);
        assert(decorations.at(L"c:\\work\\repo\\src\\main.cpp") == L'M');
        assert(decorations.at(L"c:\\work\\repo\\src") == L'M');
        assert(decorations.at(L"c:\\work\\repo\\new file.txt") == L'?');
        assert(decorations.at(L"c:\\work\\repo\\nested\\add.cpp") == L'A');
        assert(decorations.at(L"c:\\work\\repo\\nested") == L'M');
        assert(decorations.find(L"c:\\work\\repo") == decorations.end());
        filesxp::core::GitDecorationBuilder cooperative;
        assert(cooperative.start(L"C:/Work/Repo/", porcelain));
        cooperative.next(0);
        assert(cooperative.active());
        std::size_t dispatches{};
        while (cooperative.active())
        {
            cooperative.next(1);
            ++dispatches;
            assert(dispatches < 100);
        }
        assert(cooperative.take() == decorations);

        std::wstring conflicting;
        for (std::wstring_view record : {L"?? folder", L"D  folder/file.txt", L" M ../escape.txt"})
        {
            conflicting.append(record);
            conflicting.push_back(L'\0');
        }
        const auto merged = filesxp::core::buildGitDecorations(L"C:\\Repo", conflicting);
        assert(merged.at(L"c:\\repo\\folder") == L'M');
        assert(merged.at(L"c:\\repo\\folder\\file.txt") == L'D');
        assert(merged.find(L"c:\\escape.txt") == merged.end());
    }

    void archive_requests_round_trip_without_exposing_unbounded_fields()
    {
        filesxp::core::ArchiveRequest request;
        request.operation = filesxp::core::ArchiveOperation::create7z;
        request.collision = filesxp::core::ArchiveCollision::rename;
        request.workingDirectory = u"C:\\Work";
        request.target = u"C:\\Work\\Archive.7z";
        request.password = u"correct horse battery staple";
        request.paths = {u"C:\\Work\\one.txt", u"C:\\Work\\folder"};
        const auto encoded = filesxp::core::encodeArchiveRequest(request);
        assert(!encoded.empty());
        filesxp::core::ArchiveRequest decoded;
        assert(filesxp::core::decodeArchiveRequest(encoded.data(), encoded.size(), decoded));
        assert(decoded.operation == request.operation);
        assert(decoded.collision == request.collision);
        assert(decoded.password == request.password);
        assert(decoded.paths == request.paths);

        auto corrupt = encoded;
        corrupt[8] ^= 1;
        assert(!filesxp::core::decodeArchiveRequest(corrupt.data(), corrupt.size(), decoded));
        request.password = std::u16string(filesxp::core::maxArchivePasswordCharacters + 1, u'x');
        assert(filesxp::core::encodeArchiveRequest(request).empty());
        request.password = u"line\nbreak";
        assert(filesxp::core::encodeArchiveRequest(request).empty());
        request.password = std::u16string(1, static_cast<char16_t>(0xd800));
        assert(filesxp::core::encodeArchiveRequest(request).empty());
        request.password = u"密碼";
        request.operation = filesxp::core::ArchiveOperation::createZip;
        assert(filesxp::core::encodeArchiveRequest(request).empty());
        request.password = u"ascii-password";
        assert(!filesxp::core::encodeArchiveRequest(request).empty());
        request.operation = filesxp::core::ArchiveOperation::createTar;
        assert(filesxp::core::encodeArchiveRequest(request).empty());
    }

    void selection_inversion_is_bounded_and_preserves_the_original_snapshot()
    {
        filesxp::core::SelectionInversionCursor cursor;
        assert(cursor.start(6, {4, 1, 4}));
        const auto first = cursor.next(2);
        assert(first.size() == 2);
        assert(first[0].index == 0 && first[0].select);
        assert(first[1].index == 1 && !first[1].select);
        assert(cursor.active());
        const auto second = cursor.next(16);
        assert(second.size() == 4);
        assert(second[0].index == 2 && second[0].select);
        assert(second[1].index == 3 && second[1].select);
        assert(second[2].index == 4 && !second[2].select);
        assert(second[3].index == 5 && second[3].select);
        assert(!cursor.active());
        assert(cursor.beginSnapshot(6));
        assert(cursor.snapshotActive());
        assert(cursor.addSelected(1));
        assert(cursor.addSelected(4));
        assert(!cursor.addSelected(4));
        assert(cursor.finishSnapshot());
        const auto prepared = cursor.next(6);
        assert(prepared.size() == 6);
        assert(prepared[1].index == 1 && !prepared[1].select);
        assert(prepared[4].index == 4 && !prepared[4].select);
        assert(!cursor.beginSnapshot(filesxp::core::maxSelectionInversionItems + 1));
        assert(cursor.next(4).empty());
        assert(!cursor.start(2, {2}));
        assert(cursor.start(0, {}));
        assert(!cursor.active());
    }

    void bulk_rename_requests_are_versioned_bounded_and_utf16_safe()
    {
        filesxp::core::BulkRenameRequest request{
            .baseName = u"Photo",
            .paths = {u"C:\\Work\\one.jpg", u"C:\\Work\\二.jpg"}};
        const auto encoded = filesxp::core::encodeBulkRenameRequest(request);
        assert(!encoded.empty());
        filesxp::core::BulkRenameRequest decoded;
        assert(filesxp::core::decodeBulkRenameRequest(encoded.data(), encoded.size(), decoded));
        assert(decoded.baseName == request.baseName);
        assert(decoded.paths == request.paths);
        auto corrupt = encoded;
        corrupt[0] ^= 1;
        assert(!filesxp::core::decodeBulkRenameRequest(corrupt.data(), corrupt.size(), decoded));
        request.paths.resize(1);
        assert(filesxp::core::encodeBulkRenameRequest(request).empty());
        request.paths.push_back(u"C:\\Work\\two.jpg");
        request.baseName = std::u16string(1, static_cast<char16_t>(0xd800));
        assert(filesxp::core::encodeBulkRenameRequest(request).empty());
        request.baseName = std::u16string(filesxp::core::maxBulkRenameBaseCharacters + 1, u'x');
        assert(filesxp::core::encodeBulkRenameRequest(request).empty());
    }

    void folder_selection_requests_reject_truncation_and_invalid_utf16()
    {
        filesxp::core::FolderSelectionRequest request{
            .workingDirectory = u"C:\\Work",
            .folderName = u"Grouped",
            .paths = {u"C:\\Work\\one.txt", u"C:\\Work\\two.txt"}};
        const auto encoded = filesxp::core::encodeFolderSelectionRequest(request);
        assert(!encoded.empty());
        filesxp::core::FolderSelectionRequest decoded;
        assert(filesxp::core::decodeFolderSelectionRequest(encoded.data(), encoded.size(), decoded));
        assert(decoded.workingDirectory == request.workingDirectory);
        assert(decoded.folderName == request.folderName);
        assert(decoded.paths == request.paths);
        assert(!filesxp::core::decodeFolderSelectionRequest(
            encoded.data(), encoded.size() - 1, decoded));
        request.paths[0] = std::u16string(1, static_cast<char16_t>(0xdc00));
        assert(filesxp::core::encodeFolderSelectionRequest(request).empty());
    }

    void tag_requests_are_versioned_bounded_and_allow_removal()
    {
        filesxp::core::TagRequest request{
            .tags = {u"Work", u"Urgent"},
            .paths = {u"C:\\Work\\one.txt", u"C:\\Work\\two.txt"}};
        const auto encoded = filesxp::core::encodeTagRequest(request);
        assert(!encoded.empty());
        filesxp::core::TagRequest decoded;
        assert(filesxp::core::decodeTagRequest(encoded.data(), encoded.size(), decoded));
        assert(decoded.tags == request.tags);
        assert(decoded.paths == request.paths);
        assert(!filesxp::core::decodeTagRequest(encoded.data(), encoded.size() - 1, decoded));
        request.tags.clear();
        assert(!filesxp::core::encodeTagRequest(request).empty());
        request.tags = {u"bad;tag"};
        assert(filesxp::core::encodeTagRequest(request).empty());
        request.tags = {std::u16string(1, static_cast<char16_t>(0xd800))};
        assert(filesxp::core::encodeTagRequest(request).empty());
        request.tags.clear();
        request.paths.resize(filesxp::core::maxTagRequestItems + 1, u"C:\\x");
        assert(filesxp::core::encodeTagRequest(request).empty());
    }

    void shell_operation_requests_validate_each_operation_shape()
    {
        using filesxp::core::ShellOperation;
        filesxp::core::ShellOperationRequest request{
            .operation = ShellOperation::move,
            .destination = u"C:\\Destination",
            .newName = {},
            .items = {u"C:\\Source\\one.txt", u"shell:::{031E4825-7B94-4dc3-B131-E946B44C8DD5}"}};
        const auto encoded = filesxp::core::encodeShellOperationRequest(request);
        assert(!encoded.empty());
        filesxp::core::ShellOperationRequest decoded;
        assert(filesxp::core::decodeShellOperationRequest(encoded.data(), encoded.size(), decoded));
        assert(decoded.operation == request.operation);
        assert(decoded.destination == request.destination);
        assert(decoded.items == request.items);
        assert(!filesxp::core::decodeShellOperationRequest(
            encoded.data(), encoded.size() - 1, decoded));

        request.operation = ShellOperation::createFolder;
        request.newName = u"Grouped";
        assert(filesxp::core::encodeShellOperationRequest(request).empty());
        request.items.clear();
        assert(!filesxp::core::encodeShellOperationRequest(request).empty());
        request.operation = ShellOperation::deleteRecycle;
        assert(filesxp::core::encodeShellOperationRequest(request).empty());
        request.destination.clear();
        request.newName.clear();
        request.operation = ShellOperation::emptyRecycleBin;
        request.items.clear();
        assert(!filesxp::core::encodeShellOperationRequest(request).empty());
        request.items.push_back(u"C:\\Source\\one.txt");
        assert(filesxp::core::encodeShellOperationRequest(request).empty());
        request.items.clear();
        request.operation = ShellOperation::restoreRecycleBin;
        assert(!filesxp::core::encodeShellOperationRequest(request).empty());
        request.destination = u"C:\\Unexpected";
        assert(filesxp::core::encodeShellOperationRequest(request).empty());
        request.destination.clear();
        request.operation = ShellOperation::deleteRecycle;
        request.items.push_back(u"C:\\Source\\one.txt");
        assert(!filesxp::core::encodeShellOperationRequest(request).empty());
    }

    void shell_artifact_requests_are_versioned_and_shape_checked()
    {
        using filesxp::core::ShellArtifactOperation;
        filesxp::core::ShellArtifactRequest shortcut{
            .operation = ShellArtifactOperation::createShortcut,
            .destinationFolder = u"C:\\Work",
            .name = u"Editor.lnk",
            .target = u"C:\\Windows\\notepad.exe",
            .arguments = u"notes.txt",
            .workingDirectory = u"C:\\Work",
            .icon = {}};
        const auto encoded = filesxp::core::encodeShellArtifactRequest(shortcut);
        assert(!encoded.empty());
        filesxp::core::ShellArtifactRequest decoded;
        assert(filesxp::core::decodeShellArtifactRequest(encoded.data(), encoded.size(), decoded));
        assert(decoded.operation == shortcut.operation && decoded.name == shortcut.name &&
            decoded.target == shortcut.target);
        assert(!filesxp::core::decodeShellArtifactRequest(
            encoded.data(), encoded.size() - 1, decoded));
        shortcut.target = std::u16string(1, static_cast<char16_t>(0xd800));
        assert(filesxp::core::encodeShellArtifactRequest(shortcut).empty());

        filesxp::core::ShellArtifactRequest library{
            .operation = ShellArtifactOperation::createLibrary,
            .destinationFolder = {},
            .name = u"Projects",
            .target = {},
            .arguments = {},
            .workingDirectory = {},
            .icon = {}};
        assert(!filesxp::core::encodeShellArtifactRequest(library).empty());
        library.destinationFolder = u"C:\\Unexpected";
        assert(filesxp::core::encodeShellArtifactRequest(library).empty());
    }

    void batch_cursor_yields_without_skipping_or_overrunning()
    {
        filesxp::core::BatchCursor cursor;
        assert(!cursor.start(0, 4096));
        assert(!cursor.start(4097, 4096));
        assert(cursor.start(35, 4096));
        const auto first = cursor.next(16);
        const auto second = cursor.next(16);
        const auto third = cursor.next(16);
        assert(first.first == 0 && first.count == 16);
        assert(second.first == 16 && second.count == 16);
        assert(third.first == 32 && third.count == 3);
        assert(cursor.processed() == 35 && cursor.total() == 35 && !cursor.active());
        assert(cursor.next(16).count == 0);
        assert(cursor.start(1, 1));
        assert(cursor.next(0).count == 0 && cursor.active());
        cursor.cancel();
        assert(cursor.processed() == 0 && cursor.total() == 0 && !cursor.active());
    }

    void clipboard_paths_are_quoted_joined_and_bounded()
    {
        std::wstring plain;
        assert(filesxp::core::appendClipboardPath(plain, L"C:\\one.txt", false, 30));
        assert(filesxp::core::appendClipboardPath(plain, L"D:\\two.txt", false, 30));
        assert(plain == L"C:\\one.txt\r\nD:\\two.txt");
        const std::wstring unchanged = plain;
        assert(!filesxp::core::appendClipboardPath(plain, L"E:\\overflow.txt", false, 30));
        assert(plain == unchanged);

        std::wstring quoted;
        assert(filesxp::core::appendClipboardPath(quoted, L"C:\\has space.txt", true, 24));
        assert(quoted == L"\"C:\\has space.txt\"");
        assert(!filesxp::core::appendClipboardPath(quoted,
            std::wstring_view(L"bad\0path", 8), true, 100));
    }

    void notification_gate_coalesces_until_consumed()
    {
        filesxp::core::CoalescingGate gate;
        assert(gate.request());
        assert(gate.pending());
        assert(!gate.request());
        gate.consume();
        assert(!gate.pending());
        assert(gate.request());
        gate.reset();
        assert(!gate.pending());
    }

    void tag_results_round_trip_and_match_without_case()
    {
        const std::vector<std::wstring> tags{L"Work", L"Urgent"};
        assert(filesxp::core::containsTag(tags, L"work"));
        assert(!filesxp::core::containsTag(tags, L"later"));
        const std::vector<std::wstring> expected{L"C:\\one.txt", L"D:\\folder\\two.bin"};
        const auto encoded = filesxp::core::encodeTagResults(expected);
        std::vector<std::wstring> decoded;
        assert(filesxp::core::decodeTagResults(encoded.data(), encoded.size(), decoded));
        assert(decoded == expected);
        filesxp::core::TagResultCursor cursor;
        assert(cursor.start(encoded.data(), encoded.size()));
        assert(cursor.expectedCount() == expected.size());
        std::wstring_view path;
        assert(cursor.next(path) == filesxp::core::TagResultStep::item && path == expected[0]);
        assert(cursor.next(path) == filesxp::core::TagResultStep::item && path == expected[1]);
        assert(cursor.next(path) == filesxp::core::TagResultStep::complete &&
            cursor.count() == expected.size());
        auto malformed = encoded;
        malformed.back() = L'x';
        assert(!filesxp::core::decodeTagResults(malformed.data(), malformed.size(), decoded));
        malformed = encoded;
        malformed.insert(malformed.end() - 1, L'\0');
        assert(cursor.start(malformed.data(), malformed.size()));
        assert(cursor.next(path) == filesxp::core::TagResultStep::item);
        assert(cursor.next(path) == filesxp::core::TagResultStep::item);
        assert(cursor.next(path) == filesxp::core::TagResultStep::invalid);
        malformed = encoded;
        malformed[0] = L'x';
        assert(!cursor.start(malformed.data(), malformed.size()));
        malformed = encoded;
        malformed[3] = 1;
        assert(cursor.start(malformed.data(), malformed.size()));
        assert(cursor.next(path) == filesxp::core::TagResultStep::item);
        assert(cursor.next(path) == filesxp::core::TagResultStep::invalid);
        const auto empty = filesxp::core::encodeTagResults({});
        assert(filesxp::core::decodeTagResults(empty.data(), empty.size(), decoded));
        assert(decoded.empty());
    }

    void preview_policy_recognizes_text_without_misreading_folder_dots()
    {
        static_assert(filesxp::core::maxTextPreviewBytes == 256 * 1024);
        assert(filesxp::core::textPreviewTruncationNotice.find(L"256 KiB") !=
            std::wstring_view::npos);
        assert(filesxp::core::supportsTextPreview(L"C:\\work\\README.MD"));
        assert(filesxp::core::supportsTextPreview(L"script.rs"));
        assert(filesxp::core::supportsTextPreview(L".gitignore"));
        assert(!filesxp::core::supportsTextPreview(L"C:\\v1.2\\photo.png"));
        assert(!filesxp::core::supportsTextPreview(L"video.mp4"));
    }

    void preview_queue_accepts_one_in_flight_request()
    {
        filesxp::core::PreviewQueue queue;
        assert(queue.submit({false, false, L"C:\\one.txt"}));
        assert(queue.current() != nullptr);
        assert(queue.current()->path == L"C:\\one.txt");
        assert(!queue.hasPending());
    }

    void preview_queue_rejects_empty_paths_without_changing_state()
    {
        filesxp::core::PreviewQueue queue;
        assert(!queue.submit({false, false, {}}));
        assert(queue.current() == nullptr);
        assert(!queue.hasPending());
    }

    void preview_queue_bounds_busy_work_to_one_pending_request()
    {
        filesxp::core::PreviewQueue queue;
        assert(queue.submit({false, false, L"C:\\one.txt"}));
        assert(!queue.submit({true, false, L"C:\\two.txt"}));
        assert(queue.hasPending());
        assert(queue.current()->path == L"C:\\one.txt");
    }

    void preview_queue_keeps_only_the_latest_selection()
    {
        filesxp::core::PreviewQueue queue;
        assert(queue.submit({false, false, L"C:\\one.txt"}));
        assert(!queue.submit({true, false, L"C:\\two.txt"}));
        assert(!queue.submit({true, false, L"C:\\three.txt"}));
        const auto first = queue.complete();
        assert(first.recognized && first.launchNext);
        assert(queue.current() != nullptr);
        assert(queue.current()->path == L"C:\\three.txt");
    }

    void preview_queue_completion_preserves_request_semantics()
    {
        filesxp::core::PreviewQueue queue;
        assert(queue.submit({true, false, L"C:\\one.txt"}));
        const auto completion = queue.complete();
        assert(completion.recognized);
        assert(completion.switchSelection);
        assert(!completion.closing);
        assert(!completion.launchNext);
    }

    void preview_queue_advances_pending_work_exactly_once()
    {
        filesxp::core::PreviewQueue queue;
        assert(queue.submit({false, false, L"C:\\one.txt"}));
        assert(!queue.submit({true, false, L"C:\\two.txt"}));
        assert(queue.complete().launchNext);
        const auto second = queue.complete();
        assert(second.recognized && second.switchSelection && !second.launchNext);
        assert(queue.current() == nullptr);
    }

    void preview_queue_ignores_duplicate_completion_messages()
    {
        filesxp::core::PreviewQueue queue;
        assert(queue.submit({false, false, L"C:\\one.txt"}));
        assert(queue.complete().recognized);
        assert(!queue.complete().recognized);
        assert(queue.current() == nullptr);
    }

    void preview_queue_clear_drops_in_flight_and_pending_work()
    {
        filesxp::core::PreviewQueue queue;
        assert(queue.submit({false, false, L"C:\\one.txt"}));
        assert(!queue.submit({true, false, L"C:\\two.txt"}));
        queue.clear();
        assert(queue.current() == nullptr);
        assert(!queue.hasPending());
        assert(!queue.complete().recognized);
    }

    void preview_queue_preserves_close_requests_during_selection_churn()
    {
        filesxp::core::PreviewQueue queue;
        assert(queue.submit({true, false, L"C:\\one.txt"}));
        assert(!queue.submit({false, true, L"C:\\one.txt"}));
        assert(queue.complete().launchNext);
        const auto closing = queue.complete();
        assert(closing.recognized && !closing.switchSelection && closing.closing);
    }

    void preview_queue_allows_new_work_after_becoming_idle()
    {
        filesxp::core::PreviewQueue queue;
        assert(queue.submit({false, false, L"C:\\one.txt"}));
        assert(queue.complete().recognized);
        assert(queue.submit({false, false, L"C:\\two.txt"}));
        assert(queue.current() != nullptr && queue.current()->path == L"C:\\two.txt");
    }

    void tag_identity_parses_only_the_bounded_sidecar_key_shape()
    {
        filesxp::core::TagFileIdentity identity;
        assert(filesxp::core::parseTagFileIdentity(L"V1234ABCD-F0123456789ABCDEF", identity));
        assert(identity.volumeSerial == 0x1234abcdU);
        assert(identity.fileId == 0x0123456789abcdefULL);
        assert(!filesxp::core::parseTagFileIdentity(L"P0123456789ABCDEF", identity));
        assert(!filesxp::core::parseTagFileIdentity(L"V1234ABCG-F0123456789ABCDEF", identity));
        assert(!filesxp::core::parseTagFileIdentity(L"V1234ABCD-F0123", identity));
    }

    void tag_color_rejects_unbounded_registry_values()
    {
        assert(filesxp::core::validTagColor(0));
        assert(filesxp::core::validTagColor(7));
        assert(!filesxp::core::validTagColor(8));
        assert(!filesxp::core::validTagColor(0xffffffffU));
    }

    void git_status_refreshes_once_per_directory_burst()
    {
        using filesxp::core::shouldRefreshGitStatus;
        assert(!shouldRefreshGitStatus(L"", L"", 0, 100));
        assert(shouldRefreshGitStatus(L"", L"C:\\repo", 0, 100));
        assert(!shouldRefreshGitStatus(L"C:\\repo", L"C:\\repo", 100, 1599));
        assert(shouldRefreshGitStatus(L"C:\\repo", L"C:\\repo", 100, 1600));
        assert(shouldRefreshGitStatus(L"C:\\repo", L"D:\\repo", 1500, 1501));
        assert(shouldRefreshGitStatus(L"C:\\repo", L"C:\\repo", 2000, 100));
    }

    void fallback_search_requests_are_versioned_and_bounded()
    {
        filesxp::core::SearchRequest expected{
            .root = u"C:\\work",
            .query = u"report",
            .includeHidden = false};
        const auto encoded = filesxp::core::encodeSearchRequest(expected);
        assert(!encoded.empty());
        filesxp::core::SearchRequest actual;
        assert(filesxp::core::decodeSearchRequest(encoded.data(), encoded.size(), actual));
        assert(actual.root == expected.root);
        assert(actual.query == expected.query);
        assert(!actual.includeHidden);

        auto corrupt = encoded;
        corrupt[4] = 2;
        assert(!filesxp::core::decodeSearchRequest(corrupt.data(), corrupt.size(), actual));
        expected.query.assign(filesxp::core::maxSearchQueryCharacters + 1, u'x');
        assert(filesxp::core::encodeSearchRequest(expected).empty());
        expected.query = u"bad\nquery";
        assert(filesxp::core::encodeSearchRequest(expected).empty());
    }

    void shelf_order_rejects_duplicates_and_stale_indices()
    {
        const std::vector<std::uint32_t> order{3, 0, 2};
        const std::vector<std::uint32_t> duplicate{3, 0, 3};
        const std::vector<std::uint32_t> stale{3, 4};
        const std::vector<std::uint32_t> empty;
        assert(filesxp::core::validShelfOrder(order, 4, 4));
        assert(!filesxp::core::validShelfOrder(empty, 4, 4));
        assert(!filesxp::core::validShelfOrder(duplicate, 4, 4));
        assert(!filesxp::core::validShelfOrder(stale, 4, 4));
        assert(!filesxp::core::validShelfOrder(order, 4, 2));
        assert(!filesxp::core::validShelfOrder(order, 4,
            filesxp::core::maxShelfItems + 1));
    }

    void ftp_requests_keep_credentials_bounded_and_names_traversal_safe()
    {
        filesxp::core::FtpRequest expected;
        expected.operation = filesxp::core::FtpOperation::download;
        expected.requireTls = true;
        expected.url = u"ftp://example.test/pub/";
        expected.username = u"alice";
        expected.password = u"secret:with spaces";
        expected.localPath = u"C:\\Downloads\\file 2.txt";
        expected.remoteName = u"file 2.txt";
        auto encoded = filesxp::core::encodeFtpRequest(expected);
        assert(!encoded.empty());
        filesxp::core::FtpRequest actual;
        assert(filesxp::core::decodeFtpRequest(encoded.data(), encoded.size(), actual));
        assert(actual.operation == expected.operation);
        assert(actual.url == expected.url);
        assert(actual.username == expected.username);
        assert(actual.password == expected.password);
        assert(actual.localPath == expected.localPath);
        assert(actual.remoteName == expected.remoteName);

        expected.url = u"ftp://alice:secret@example.test/";
        assert(filesxp::core::encodeFtpRequest(expected).empty());
        expected.url = u"ftp://alice%40example.test/";
        assert(filesxp::core::encodeFtpRequest(expected).empty());
        expected.url = u"https://example.test/";
        assert(filesxp::core::encodeFtpRequest(expected).empty());
        expected.url = u"ftps://example.test/";
        expected.username = u"bad:user";
        assert(filesxp::core::encodeFtpRequest(expected).empty());
        expected.username = u"alice";
        expected.remoteName = u"..";
        assert(filesxp::core::encodeFtpRequest(expected).empty());
        expected.remoteName = u"file.txt";
        expected.password = u"line\nbreak";
        assert(filesxp::core::encodeFtpRequest(expected).empty());
        expected.password.clear();
        expected.username.clear();
        expected.url = u"ftp://example.test/bad path/";
        assert(filesxp::core::encodeFtpRequest(expected).empty());
        expected.url = u"ftp://example.test/bad%2/";
        assert(filesxp::core::encodeFtpRequest(expected).empty());

        assert(filesxp::core::quoteCurlConfigValue("alice:pa\\\"ss") ==
            "\"alice:pa\\\\\\\"ss\"");
        assert(filesxp::core::quoteCurlConfigValue("injected\nurl = evil") ==
            "\"injected\\nurl = evil\"");
        assert(filesxp::core::quoteCurlConfigValue(std::string_view("bad\0value", 9)).empty());
        assert(filesxp::core::percentEncodeFtpSegment("file name+#.txt") ==
            "file%20name%2B%23.txt");
        assert(filesxp::core::percentEncodeFtpSegment("\xc3\xa9.txt") == "%C3%A9.txt");
        assert(filesxp::core::parentFtpDirectoryUrl(u"ftp://example.test/a/b/") ==
            u"ftp://example.test/a/");
        assert(filesxp::core::parentFtpDirectoryUrl(u"ftps://example.test/") ==
            u"ftps://example.test/");
        assert(filesxp::core::parentFtpDirectoryUrl(u"https://example.test/a/").empty());

        const auto names = filesxp::core::parseFtpNameList(
            L"file10.txt\r\n../\r\nfolder\\escape\r\nFile2.txt\r\n");
        assert((names == std::vector<std::wstring>{L"File2.txt", L"file10.txt"}));
        const auto presorted = filesxp::core::parsePresortedFtpNameList(
            L"file10.txt\r\n../\r\nFile2.txt\r\n");
        assert((presorted == std::vector<std::wstring>{L"file10.txt", L"File2.txt"}));

        filesxp::core::FtpNameListCursor cursor;
        assert(!cursor.start({}));
        assert(cursor.start(L"one.txt\r\n../\r\ntwo.txt"));
        std::wstring_view name;
        assert(cursor.next(name) == filesxp::core::FtpNameListStep::item &&
            name == L"one.txt");
        assert(cursor.next(name) == filesxp::core::FtpNameListStep::skipped &&
            name == L"../");
        assert(cursor.next(name) == filesxp::core::FtpNameListStep::item &&
            name == L"two.txt");
        assert(!cursor.active() && cursor.inspected() == 3 && cursor.accepted() == 2);
        assert(cursor.next(name) == filesxp::core::FtpNameListStep::done && name.empty());
        cursor.cancel();
        assert(cursor.inspected() == 0 && cursor.accepted() == 0);

        std::wstring cappedListing;
        cappedListing.reserve((filesxp::core::maxFtpListingItems + 1) * 2);
        for (std::size_t index = 0; index <= filesxp::core::maxFtpListingItems; ++index)
            cappedListing += L"x\n";
        assert(cursor.start(cappedListing));
        while (cursor.active())
            assert(cursor.next(name) == filesxp::core::FtpNameListStep::item);
        assert(cursor.inspected() == filesxp::core::maxFtpListingItems);
        assert(cursor.accepted() == filesxp::core::maxFtpListingItems);
        assert(cursor.next(name) == filesxp::core::FtpNameListStep::done);

        encoded[0] = 'B';
        assert(!filesxp::core::decodeFtpRequest(encoded.data(), encoded.size(), actual));
    }
}

int main()
{
    generation_gate_rejects_stale_work();
    bounded_queue_applies_backpressure();
    bounded_queue_survives_concurrent_producers();
    natural_order_handles_numbers_and_case();
    session_codec_round_trips_and_rejects_malformed_state();
    windows_arguments_are_quoted_without_shell_interpretation();
    copydata_paths_reject_ambiguous_embedded_nulls();
    settings_codec_is_bounded_and_rejects_unknown_flags();
    system_locale_resolves_chinese_scripts_correctly();
    locale_packs_are_bounded_versioned_and_reject_duplicate_keys();
    command_match_is_case_insensitive_and_ordered();
    tags_are_trimmed_deduplicated_and_bounded();
    shortcut_map_rejects_conflicts_and_unsafe_bare_keys();
    filename_policy_preserves_extensions_and_rejects_windows_devices();
    flatten_policy_never_descends_into_reparse_points();
    git_inputs_are_bounded_and_refs_reject_option_and_revision_syntax();
    archive_requests_round_trip_without_exposing_unbounded_fields();
    selection_inversion_is_bounded_and_preserves_the_original_snapshot();
    bulk_rename_requests_are_versioned_bounded_and_utf16_safe();
    folder_selection_requests_reject_truncation_and_invalid_utf16();
    tag_requests_are_versioned_bounded_and_allow_removal();
    shell_operation_requests_validate_each_operation_shape();
    shell_artifact_requests_are_versioned_and_shape_checked();
    batch_cursor_yields_without_skipping_or_overrunning();
    clipboard_paths_are_quoted_joined_and_bounded();
    notification_gate_coalesces_until_consumed();
    tag_results_round_trip_and_match_without_case();
    preview_policy_recognizes_text_without_misreading_folder_dots();
    preview_queue_accepts_one_in_flight_request();
    preview_queue_rejects_empty_paths_without_changing_state();
    preview_queue_bounds_busy_work_to_one_pending_request();
    preview_queue_keeps_only_the_latest_selection();
    preview_queue_completion_preserves_request_semantics();
    preview_queue_advances_pending_work_exactly_once();
    preview_queue_ignores_duplicate_completion_messages();
    preview_queue_clear_drops_in_flight_and_pending_work();
    preview_queue_preserves_close_requests_during_selection_churn();
    preview_queue_allows_new_work_after_becoming_idle();
    tag_identity_parses_only_the_bounded_sidecar_key_shape();
    tag_color_rejects_unbounded_registry_values();
    git_status_refreshes_once_per_directory_burst();
    fallback_search_requests_are_versioned_and_bounded();
    shelf_order_rejects_duplicates_and_stale_indices();
    ftp_requests_keep_credentials_bounded_and_names_traversal_safe();
    std::cout << "All portable core tests passed.\n";
}

