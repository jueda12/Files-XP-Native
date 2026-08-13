#include "session_store.h"

#include <windows.h>

#include <cstddef>
#include <vector>

namespace filesxp::app
{
    namespace
    {
        constexpr wchar_t registryPath[] = L"Software\\FilesXPNative";
        constexpr wchar_t sessionValue[] = L"Session";
    }

    core::SessionSnapshot SessionStore::load() noexcept
    {
        try
        {
            DWORD bytes{};
            if (RegGetValueW(HKEY_CURRENT_USER, registryPath, sessionValue, RRF_RT_REG_MULTI_SZ,
                    nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes < 3 * sizeof(wchar_t) ||
                bytes > core::SessionCodec::maxEncodedCharacters * sizeof(wchar_t) ||
                bytes % sizeof(wchar_t) != 0)
            {
                return {};
            }

            std::vector<wchar_t> encoded(bytes / sizeof(wchar_t));
            if (RegGetValueW(HKEY_CURRENT_USER, registryPath, sessionValue, RRF_RT_REG_MULTI_SZ,
                    nullptr, encoded.data(), &bytes) != ERROR_SUCCESS)
            {
                return {};
            }
            core::SessionSnapshot snapshot;
            return core::SessionCodec::decode(encoded.data(), bytes / sizeof(wchar_t), snapshot)
                ? snapshot : core::SessionSnapshot{};
        }
        catch (...)
        {
            return {};
        }
    }

    void SessionStore::save(const core::SessionSnapshot& snapshot) noexcept
    {
        try
        {
            const auto encoded = core::SessionCodec::encode(snapshot);
            if (encoded.empty())
            {
                return;
            }
            HKEY key{};
            if (RegCreateKeyExW(HKEY_CURRENT_USER, registryPath, 0, nullptr, 0, KEY_SET_VALUE,
                    nullptr, &key, nullptr) != ERROR_SUCCESS)
            {
                return;
            }
            RegSetValueExW(key, sessionValue, 0, REG_MULTI_SZ,
                reinterpret_cast<const BYTE*>(encoded.data()),
                static_cast<DWORD>(encoded.size() * sizeof(wchar_t)));
            RegCloseKey(key);
        }
        catch (...)
        {
            // Session persistence is best-effort; shutdown must remain reliable.
        }
    }
}
