#pragma once

#include <windows.h>
#include <shobjidl.h>

#include <atomic>
#include <cstdint>

namespace filesxp::app
{
    class FileOperationProgressSink final : public IFileOperationProgressSink
    {
    public:
        FileOperationProgressSink(HANDLE cancel, HWND window, UINT progressMessage,
            std::uint32_t generation) noexcept
            : cancel_(cancel), window_(window), progressMessage_(progressMessage),
              generation_(generation)
        {
        }

        IFACEMETHODIMP QueryInterface(REFIID id, void** result) noexcept override
        {
            if (result == nullptr) return E_POINTER;
            *result = nullptr;
            if (IsEqualIID(id, IID_IUnknown) || IsEqualIID(id, IID_IFileOperationProgressSink))
            {
                *result = static_cast<IFileOperationProgressSink*>(this);
                AddRef();
                return S_OK;
            }
            return E_NOINTERFACE;
        }
        IFACEMETHODIMP_(ULONG) AddRef() noexcept override { return ++references_; }
        IFACEMETHODIMP_(ULONG) Release() noexcept override
        {
            const ULONG remaining = --references_;
            if (remaining == 0) delete this;
            return remaining;
        }
        IFACEMETHODIMP StartOperations() noexcept override { return check(); }
        IFACEMETHODIMP FinishOperations(HRESULT result) noexcept override
        {
            record(result);
            return check();
        }
        IFACEMETHODIMP PreRenameItem(DWORD, IShellItem*, LPCWSTR) noexcept override { return check(); }
        IFACEMETHODIMP PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT result, IShellItem*) noexcept override
        {
            record(result);
            return check();
        }
        IFACEMETHODIMP PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) noexcept override { return check(); }
        IFACEMETHODIMP PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT result, IShellItem*) noexcept override
        {
            record(result);
            return check();
        }
        IFACEMETHODIMP PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) noexcept override { return check(); }
        IFACEMETHODIMP PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT result, IShellItem*) noexcept override
        {
            record(result);
            return check();
        }
        IFACEMETHODIMP PreDeleteItem(DWORD, IShellItem*) noexcept override { return check(); }
        IFACEMETHODIMP PostDeleteItem(DWORD, IShellItem*, HRESULT result, IShellItem*) noexcept override
        {
            record(result);
            return check();
        }
        IFACEMETHODIMP PreNewItem(DWORD, IShellItem*, LPCWSTR) noexcept override { return check(); }
        IFACEMETHODIMP PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT result, IShellItem*) noexcept override
        {
            record(result);
            return check();
        }
        IFACEMETHODIMP UpdateProgress(UINT total, UINT completed) noexcept override
        {
            if (total != 0 && progressMessage_ != 0)
            {
                const UINT percent = static_cast<UINT>(
                    (static_cast<unsigned long long>(completed) * 100ULL) / total);
                PostMessageW(window_, progressMessage_, generation_, percent > 100 ? 100 : percent);
            }
            return check();
        }
        IFACEMETHODIMP ResetTimer() noexcept override { return check(); }
        IFACEMETHODIMP PauseTimer() noexcept override { return check(); }
        IFACEMETHODIMP ResumeTimer() noexcept override { return check(); }

        [[nodiscard]] ULONG failures() const noexcept { return failures_.load(); }

    private:
        [[nodiscard]] HRESULT check() const noexcept
        {
            return cancel_ != nullptr && WaitForSingleObject(cancel_, 0) == WAIT_OBJECT_0 ?
                HRESULT_FROM_WIN32(ERROR_CANCELLED) : S_OK;
        }
        void record(HRESULT result) noexcept
        {
            if (FAILED(result)) ++failures_;
        }

        std::atomic_ulong references_{1};
        std::atomic_ulong failures_{};
        HANDLE cancel_{};
        HWND window_{};
        UINT progressMessage_{};
        std::uint32_t generation_{};
    };
}
