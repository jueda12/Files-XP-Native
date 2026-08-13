#include <windows.h>
#include <oleacc.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <string_view>

namespace
{
    constexpr std::size_t maximumInputCharacters = 32;
    constexpr std::size_t maximumOutputNameCharacters = 256;

    [[nodiscard]] bool parseHandle(std::wstring_view value, HWND& handle) noexcept
    {
        if (value.empty() || value.size() > maximumInputCharacters) return false;
        wchar_t* end{};
        const unsigned long long parsed = std::wcstoull(value.data(), &end, 0);
        if (end == value.data() || *end != L'\0' || parsed == 0) return false;
        handle = reinterpret_cast<HWND>(static_cast<UINT_PTR>(parsed));
        return true;
    }

    void appendJsonEscaped(std::wstring_view value, std::wstring& output)
    {
        for (const wchar_t character : value)
        {
            switch (character)
            {
            case L'"': output += L"\\\""; break;
            case L'\\': output += L"\\\\"; break;
            case L'\b': output += L"\\b"; break;
            case L'\f': output += L"\\f"; break;
            case L'\n': output += L"\\n"; break;
            case L'\r': output += L"\\r"; break;
            case L'\t': output += L"\\t"; break;
            default:
                if (character < 0x20)
                {
                    wchar_t escaped[7]{};
                    (void)swprintf_s(escaped, L"\\u%04x", static_cast<unsigned int>(character));
                    output += escaped;
                }
                else
                {
                    output += character;
                }
                break;
            }
        }
    }

    void writeError(HRESULT status) noexcept
    {
        std::wprintf(L"{\"ok\":false,\"hresult\":\"0x%08lX\"}\n",
            static_cast<unsigned long>(static_cast<DWORD>(status)));
    }
}

int wmain(int argumentCount, wchar_t* arguments[])
{
    if (argumentCount != 2)
    {
        writeError(E_INVALIDARG);
        return 2;
    }

    HWND window{};
    if (!parseHandle(arguments[1], window) || !IsWindow(window))
    {
        writeError(E_INVALIDARG);
        return 2;
    }

    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized))
    {
        writeError(initialized);
        return 3;
    }

    IAccessible* accessible{};
    const HRESULT status = AccessibleObjectFromWindow(window, static_cast<DWORD>(OBJID_CLIENT),
        IID_IAccessible, reinterpret_cast<void**>(&accessible));
    if (FAILED(status) || accessible == nullptr)
    {
        if (SUCCEEDED(initialized)) CoUninitialize();
        writeError(FAILED(status) ? status : E_NOINTERFACE);
        return 4;
    }

    VARIANT self{};
    self.vt = VT_I4;
    self.lVal = CHILDID_SELF;
    VARIANT role{};
    VariantInit(&role);
    BSTR name{};
    HRESULT result = accessible->get_accRole(self, &role);
    if (SUCCEEDED(result) && role.vt != VT_I4)
    {
        result = DISP_E_TYPEMISMATCH;
    }
    if (SUCCEEDED(result))
    {
        result = accessible->get_accName(self, &name);
        if (result == S_FALSE) result = S_OK;
    }

    if (SUCCEEDED(result))
    {
        std::wstring output = L"{\"ok\":true,\"hwnd\":\"";
        wchar_t handleText[2 + sizeof(UINT_PTR) * 2 + 1]{};
        (void)swprintf_s(handleText, L"%p", static_cast<void*>(window));
        output += handleText;
        output += L"\",\"role\":" + std::to_wstring(role.lVal) + L",\"name\":\"";
        if (name != nullptr)
        {
            const UINT length = SysStringLen(name);
            appendJsonEscaped(std::wstring_view(name,
                length > maximumOutputNameCharacters ? maximumOutputNameCharacters : length), output);
        }
        output += L"\"}\n";
        std::wprintf(L"%ls", output.c_str());
    }
    else
    {
        writeError(result);
    }

    if (name != nullptr) SysFreeString(name);
    VariantClear(&role);
    accessible->Release();
    CoUninitialize();
    return SUCCEEDED(result) ? 0 : 5;
}
