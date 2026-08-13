#include "settings_store.h"

#include <windows.h>

#include <vector>

namespace filesxp::app
{
    namespace
    {
        constexpr wchar_t registryPath[] = L"Software\\FilesXPNative";
        constexpr wchar_t settingsValue[] = L"Settings";
    }

    core::AppSettings SettingsStore::load() noexcept
    {
        try
        {
            DWORD bytes{};
            if (RegGetValueW(HKEY_CURRENT_USER, registryPath, settingsValue, RRF_RT_REG_MULTI_SZ,
                    nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes < 6 * sizeof(wchar_t) ||
                bytes > core::SettingsCodec::maxEncodedCharacters * sizeof(wchar_t) ||
                bytes % sizeof(wchar_t) != 0)
            {
                return {};
            }
            std::vector<wchar_t> encoded(bytes / sizeof(wchar_t));
            if (RegGetValueW(HKEY_CURRENT_USER, registryPath, settingsValue, RRF_RT_REG_MULTI_SZ,
                    nullptr, encoded.data(), &bytes) != ERROR_SUCCESS)
            {
                return {};
            }
            core::AppSettings settings;
            return core::SettingsCodec::decode(encoded.data(), bytes / sizeof(wchar_t), settings)
                ? settings : core::AppSettings{};
        }
        catch (...)
        {
            return {};
        }
    }

    bool SettingsStore::save(const core::AppSettings& settings) noexcept
    {
        try
        {
            const auto encoded = core::SettingsCodec::encode(settings);
            if (encoded.empty())
            {
                return false;
            }
            HKEY key{};
            if (RegCreateKeyExW(HKEY_CURRENT_USER, registryPath, 0, nullptr, 0, KEY_SET_VALUE,
                    nullptr, &key, nullptr) != ERROR_SUCCESS)
            {
                return false;
            }
            const LSTATUS result = RegSetValueExW(key, settingsValue, 0, REG_MULTI_SZ,
                reinterpret_cast<const BYTE*>(encoded.data()),
                static_cast<DWORD>(encoded.size() * sizeof(wchar_t)));
            RegCloseKey(key);
            return result == ERROR_SUCCESS;
        }
        catch (...)
        {
            return false;
        }
    }

    bool SettingsStore::reset() noexcept
    {
        const LSTATUS result = RegDeleteKeyValueW(HKEY_CURRENT_USER, registryPath, settingsValue);
        return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
    }
}
