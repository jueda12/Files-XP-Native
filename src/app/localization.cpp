#include "localization.h"
#include "../core/locale_pack.h"

#include <string>
#include <vector>

namespace filesxp::app
{
    namespace
    {
        using Row = std::array<const wchar_t*, 3>;
        constexpr std::array<Row, static_cast<std::size_t>(Text::count)> strings{{
            {L"Files XP Native", L"Files XP Native", L"Files XP Native"},
            {L"Back", L"上一頁", L"后退"}, {L"Forward", L"下一頁", L"前進"},
            {L"Up", L"上一層", L"向上"}, {L"Refresh", L"重新整理", L"刷新"},
            {L"New Folder", L"新增資料夾", L"新建文件夾"}, {L"View", L"檢視", L"查看"},
            {L"Address", L"位址", L"地址"}, {L"Go", L"前往", L"轉到"},
            {L"Search this folder", L"搜尋此資料夾", L"搜索此文件夾"},
            {L"File and Folder Tasks", L"檔案及資料夾工作", L"文件和文件夾任務"},
            {L"Quick Access", L"快速存取", L"快速訪問"}, {L"Desktop", L"桌面", L"桌面"},
            {L"My Documents", L"我的文件", L"我的文檔"}, {L"Downloads", L"下載", L"下載"},
            {L"My Pictures", L"我的圖片", L"我的圖片"}, {L"My Music", L"我的音樂", L"我的音樂"},
            {L"My Videos", L"我的影片", L"我的視頻"}, {L"This PC", L"本機", L"此電腦"},
            {L"Libraries", L"媒體櫃", L"庫"}, {L"Network", L"網路", L"網絡"},
            {L"WSL", L"WSL", L"WSL"}, {L"Recycle Bin", L"資源回收筒", L"回收站"},
            {L"File", L"檔案", L"文件"}, {L"Edit", L"編輯", L"編輯"},
            {L"View", L"檢視", L"查看"}, {L"Go", L"前往", L"轉到"},
            {L"Tools", L"工具", L"工具"}, {L"New Tab", L"新增索引標籤", L"新建標籤頁"},
            {L"New Window", L"新增視窗", L"新建窗口"}, {L"Duplicate Tab", L"複製索引標籤", L"複製標籤頁"},
            {L"Reopen Closed Tab", L"重新開啟已關閉索引標籤", L"重新打開已關閉標籤頁"},
            {L"Close Tab", L"關閉索引標籤", L"關閉標籤頁"},
            {L"Close Other Tabs", L"關閉其他索引標籤", L"關閉其他標籤頁"},
            {L"Move Tab Left", L"向左移動索引標籤", L"向左移動標籤頁"},
            {L"Move Tab Right", L"向右移動索引標籤", L"向右移動標籤頁"},
            {L"New Text Document", L"新增文字文件", L"新建文本文檔"},
            {L"Create Shortcut...", L"建立快捷方式...", L"创建快捷方式..."},
            {L"Create Library...", L"建立媒體櫃...", L"创建库..."},
            {L"Exit", L"結束", L"退出"},
            {L"Undo", L"復原", L"撤消"}, {L"Redo", L"重做", L"重做"},
            {L"Cut", L"剪下", L"剪切"}, {L"Copy", L"複製", L"複製"},
            {L"Paste", L"貼上", L"粘貼"}, {L"Copy Path", L"複製路徑", L"複製路徑"},
            {L"Copy Path with Quotes", L"複製帶引號路徑", L"複製帶引號路徑"},
            {L"Rename", L"重新命名", L"重命名"},
            {L"Bulk Rename...", L"批次重新命名...", L"批量重命名..."},
            {L"Create Folder from Selection...", L"由選取項目建立資料夾...", L"从所选项创建文件夹..."},
            {L"Enter one base name. Extensions are preserved and duplicates are numbered.",
                L"輸入一個基礎名稱；副檔名會保留，重複項會自動編號。",
                L"输入一个基础名称；扩展名会保留，重复项会自动编号。"},
            {L"Enter a new folder name. Selected items will be moved into it.",
                L"輸入新資料夾名稱；選取項目會移入其中。",
                L"输入新文件夹名称；所选项会移入其中。"},
            {L"The name is invalid, reserved, or too long.",
                L"名稱無效、已保留或過長。", L"名称无效、已保留或过长。"},
            {L"Flatten Folder", L"展平資料夾", L"展平文件夹"},
            {L"Move every item from all subfolders into the selected folder? Reparse points are not traversed. Windows Undo is available.",
                L"要將所有子資料夾內的項目移到選取的資料夾嗎？不會穿越重解析點，並可使用 Windows 復原。",
                L"要将所有子文件夹内的项目移到所选文件夹吗？不会穿越重解析点，并可使用 Windows 撤销。"},
            {L"Edit Alternate Stream...", L"編輯替代資料流...", L"编辑备用数据流..."},
            {L"Enter the stream name. It will open in the system text editor.",
                L"輸入資料流名稱；它會在系統文字編輯器開啟。",
                L"输入数据流名称；它会在系统文本编辑器中打开。"},
            {L"Delete", L"刪除", L"刪除"},
            {L"Delete Permanently", L"永久刪除", L"永久刪除"}, {L"Properties", L"內容", L"屬性"},
            {L"Edit Shortcut...", L"編輯快捷方式...", L"编辑快捷方式..."},
            {L"Edit Library...", L"編輯媒體櫃...", L"编辑库..."},
            {L"Name", L"名稱", L"名称"}, {L"Target", L"目標", L"目标"},
            {L"Arguments", L"參數", L"参数"}, {L"Start in", L"開始位置", L"起始位置"},
            {L"Icon path", L"圖示路徑", L"图标路径"},
            {L"A valid name and target are required.",
                L"必須提供有效名稱及目標。", L"必须提供有效名称和目标。"},
            {L"Enter a name for the new Windows Library.",
                L"輸入新 Windows 媒體櫃的名稱。", L"输入新 Windows 库的名称。"},
            {L"Select All", L"全選", L"全選"}, {L"Add Copy to Shelf", L"加入複製暫存架", L"添加複製到暫存架"},
            {L"Add Move to Shelf", L"加入移動暫存架", L"添加移動到暫存架"},
            {L"Paste Shelf Here", L"在此貼上暫存架", L"在此粘貼暫存架"},
            {L"Clear Shelf", L"清除暫存架", L"清空暫存架"}, {L"Details", L"詳細資料", L"詳細信息"},
            {L"List", L"清單", L"列表"}, {L"Small Icons", L"小圖示", L"小圖標"},
            {L"Medium Icons", L"中圖示", L"中等圖標"}, {L"Large Icons", L"大圖示", L"大圖標"},
            {L"Extra Large Icons", L"特大圖示", L"超大圖標"}, {L"Tiles", L"並排", L"平鋪"},
            {L"Content", L"內容", L"內容"}, {L"Quick Preview Pane", L"快速預覽窗格", L"快速預覽窗格"},
            {L"Loading preview...", L"正在載入預覽...", L"正在加載預覽..."},
            {L"No built-in text preview for this selection.",
                L"此選取項目沒有內建文字預覽。", L"此所选项没有内置文本预览。"},
            {L"Details Pane", L"詳細資料窗格", L"詳細信息窗格"},
            {L"Split Vertically", L"垂直分割", L"垂直拆分"}, {L"Split Horizontally", L"水平分割", L"水平拆分"},
            {L"Focus Other Pane", L"焦點移至另一窗格", L"聚焦另一窗格"},
            {L"Close Active Pane", L"關閉目前窗格", L"關閉當前窗格"},
            {L"Auto-size All Columns", L"自動調整所有欄寬", L"自動調整所有列寬"},
            {L"Address", L"位址", L"地址"}, {L"Search", L"搜尋", L"搜索"},
            {L"Settings...", L"設定...", L"設置..."},
            {L"Command Palette", L"命令選擇器", L"命令面板"},
            {L"Keyboard Shortcuts...", L"鍵盤快捷鍵...", L"键盘快捷键..."},
            {L"Select a field, then press the new shortcut.",
                L"選取欄位，然後按下新的快捷鍵。", L"选择字段，然后按下新的快捷键。"},
            {L"That shortcut is duplicated, reserved, or unsafe.",
                L"該快捷鍵重複、已保留或不安全。", L"该快捷键重复、已保留或不安全。"},
            {L"Run", L"執行", L"運行"},
            {L"Edit Tags...", L"編輯標籤...", L"編輯標籤..."},
            {L"Filter by Tag...", L"按標籤篩選...", L"按標籤篩選..."},
            {L"Manage Tag Color...", L"管理標籤顏色...", L"管理標籤顏色..."},
            {L"Tag Color", L"標籤顏色", L"標籤顏色"},
            {L"Separate tags with semicolons. Blank removes tags.",
                L"以分號分隔標籤；留空會移除標籤。", L"用分號分隔標籤；留空會移除標籤。"},
            {L"Use up to 16 tags of 64 characters each; quotes are not allowed.",
                L"最多 16 個標籤，每個 64 字元；不可使用引號。",
                L"最多 16 個標籤，每個 64 字符；不能使用引號。"},
            {L"None", L"無", L"無"}, {L"Red", L"紅色", L"紅色"},
            {L"Orange", L"橙色", L"橙色"}, {L"Yellow", L"黃色", L"黃色"},
            {L"Green", L"綠色", L"綠色"}, {L"Blue", L"藍色", L"藍色"},
            {L"Purple", L"紫色", L"紫色"}, {L"Gray", L"灰色", L"灰色"},
            {L"Map Network Drive...", L"連線網路磁碟機...", L"映射網絡驅動器..."},
            {L"Disconnect Network Drive...", L"中斷網路磁碟機...", L"斷開網絡驅動器..."},
            {L"Calculate SHA-256", L"計算 SHA-256", L"計算 SHA-256"},
            {L"Show Alternate Streams", L"顯示替代資料流", L"顯示備用數據流"},
            {L"Verify Digital Signature", L"驗證數碼簽章", L"驗證數字簽名"},
            {L"Create Archive...", L"建立壓縮檔...", L"創建壓縮包..."},
            {L"Extract Archive...", L"解壓縮檔...", L"解壓壓縮包..."},
            {L"Archive name", L"壓縮檔名稱", L"壓縮包名稱"},
            {L"Destination folder", L"目的地資料夾", L"目標文件夾"},
            {L"Format", L"格式", L"格式"}, {L"Password (optional)", L"密碼（可留空）", L"密碼（可留空）"},
            {L"Existing items", L"現有項目", L"現有項目"},
            {L"Keep both / rename", L"保留兩者／重新命名", L"保留兩者／重命名"},
            {L"Overwrite", L"覆寫", L"覆蓋"}, {L"Skip", L"略過", L"跳過"},
            {L"Enter a valid name. TAR has no password support; ZIP passwords must use printable ASCII.",
                L"請輸入有效名稱；TAR 不支援密碼，ZIP 密碼只可使用可列印 ASCII。",
                L"請輸入有效名稱；TAR 不支持密碼，ZIP 密碼只可使用可打印 ASCII。"},
            {L"Git Init", L"Git 初始化", L"Git 初始化"},
            {L"Git Clone...", L"Git 複製...", L"Git 克隆..."},
            {L"Git Create Branch...", L"Git 建立分支...", L"Git 创建分支..."},
            {L"Git Switch Branch...", L"Git 切換分支...", L"Git 切换分支..."},
            {L"Enter a repository URL or path, then choose its parent folder.",
                L"輸入 repository URL 或路徑，然後選擇其父資料夾。",
                L"输入 repository URL 或路径，然后选择其父文件夹。"},
            {L"Enter a valid Git branch name.", L"輸入有效的 Git 分支名稱。",
                L"输入有效的 Git 分支名称。"},
            {L"Git Status", L"Git 狀態", L"Git 狀態"},
            {L"Git Fetch", L"Git 擷取", L"Git 獲取"}, {L"Git Pull", L"Git 拉取", L"Git 拉取"},
            {L"Git Push", L"Git 推送", L"Git 推送"}, {L"Git Sync", L"Git 同步", L"Git 同步"},
            {L"Operation in progress...", L"操作進行中...", L"操作進行中..."},
            {L"Operation completed.", L"操作已完成。", L"操作已完成。"},
            {L"Operation canceled.", L"操作已取消。", L"操作已取消。"},
            {L"Operation failed. Exit code: ", L"操作失敗。結束代碼：", L"操作失敗。退出代碼："},
            {L"Close", L"關閉", L"關閉"},
            {L"Language", L"語言", L"語言"}, {L"Default view", L"預設檢視", L"默認視圖"},
            {L"New tab location", L"新索引標籤位置", L"新標籤頁位置"},
            {L"Restore tabs after restart", L"重啟後還原索引標籤", L"重啟後恢復標籤頁"},
            {L"Show places pane", L"顯示位置窗格", L"顯示位置窗格"},
            {L"Use compact toolbar", L"使用精簡工具列", L"使用緊湊工具欄"},
            {L"Toolbar buttons", L"工具列按鈕", L"工具欄按鈕"},
            {L"Confirm permanent deletion", L"確認永久刪除", L"確認永久刪除"},
            {L"Enable Git commands", L"啟用 Git 命令", L"啟用 Git 命令"},
            {L"Enable archive adapter", L"啟用壓縮檔整合", L"啟用壓縮包集成"},
            {L"Space opens Quick Preview", L"空白鍵開啟快速預覽", L"空格鍵打開快速預覽"},
            {L"Preview provider", L"預覽提供者", L"預覽提供者"},
            {L"Automatic", L"自動", L"自動"},
            {L"Windows / Built-in", L"Windows / 內建", L"Windows / 內置"},
            {L"QuickLook", L"QuickLook", L"QuickLook"}, {L"Seer", L"Seer", L"Seer"},
            {L"PowerToys Peek", L"PowerToys Peek", L"PowerToys Peek"},
            {L"Reset", L"重設", L"重置"}, {L"OK", L"確定", L"確定"}, {L"Cancel", L"取消", L"取消"},
            {L"System", L"系統", L"系統"}, {L"English (United States)", L"English (United States)", L"English (United States)"},
            {L"繁體中文", L"繁體中文", L"繁體中文"}, {L"简体中文", L"简体中文", L"简体中文"},
            {L"object", L"個物件", L"個對象"}, {L"objects", L"個物件", L"個對象"},
            {L"selected", L"已選取", L"已選擇"}, {L"Search results", L"搜尋結果", L"搜索結果"},
            {L"Shelf", L"暫存架", L"暫存架"}, {L"to move", L"待移動", L"待移動"},
            {L"to copy", L"待複製", L"待複製"},
            {L"Folder tabs", L"資料夾索引標籤", L"文件夾標籤頁"},
            {L"Status", L"狀態", L"狀態"},
            {L"Background task output", L"背景工作輸出", L"後台任務輸出"},
            {L"Invert Selection", L"反向選取", L"反向選擇"},
            {L"Paste Shortcut", L"貼上快捷方式", L"粘貼快捷方式"},
            {L"Close Tabs to the Left", L"關閉左側索引標籤", L"關閉左側標籤頁"},
            {L"Close Tabs to the Right", L"關閉右側索引標籤", L"關閉右側標籤頁"},
            {L"Close All Tabs", L"關閉所有索引標籤", L"關閉所有標籤頁"},
            {L"Open in Terminal", L"在終端機開啟", L"在終端中打開"},
            {L"Open in Terminal as Administrator", L"以系統管理員身分在終端機開啟",
                L"以管理員身份在終端中打開"},
            {L"Show Sidebar", L"顯示側邊欄", L"顯示側邊欄"},
            {L"Full Screen", L"全螢幕", L"全屏"},
            {L"Empty Recycle Bin...", L"清空資源回收筒...", L"清空回收站..."},
            {L"Home", L"首頁", L"主頁"},
            {L"Pin to Quick Access", L"釘選到快速存取", L"固定到快速訪問"},
            {L"Unpin from Quick Access", L"從快速存取取消釘選", L"從快速訪問取消固定"},
            {L"Paste into Selected Folder", L"貼到選取的資料夾",
                L"粘貼到所選文件夾"},
            {L"Hidden items", L"隱藏的項目", L"隱藏的項目"},
            {L"File name extensions", L"副檔名", L"文件擴展名"},
            {L"Clear Selection", L"清除選取", L"清除選擇"},
            {L"Restore", L"還原", L"還原"},
            {L"Open in New Tab", L"在新索引標籤開啟", L"在新標籤頁中打開"},
            {L"Open in New Window", L"在新視窗開啟", L"在新窗口中打開"},
            {L"Open in Other Pane", L"在另一窗格開啟", L"在另一窗格中打開"},
            {L"Open Current Folder in Other Pane", L"在另一窗格開啟目前資料夾",
                L"在另一窗格中打開當前文件夾"},
            {L"Open File Location", L"開啟檔案位置", L"打開文件所在位置"},
            {L"Restore All from Recycle Bin", L"從資源回收筒還原全部項目",
                L"從回收站還原所有項目"},
            {L"Manage Shelf...", L"管理暫存架...", L"管理暫存架..."},
            {L"Remove", L"移除", L"移除"},
            {L"Move Up", L"上移", L"上移"},
            {L"Move Down", L"下移", L"下移"},
            {L"(Unavailable item)", L"（無法使用的項目）", L"（不可用的項目）"},
            {L"Actions", L"動作", L"操作"},
            {L"Play Selected", L"播放選取項目", L"播放所選項目"},
            {L"Run as Administrator", L"以系統管理員身分執行", L"以管理員身份運行"},
            {L"Run as Different User", L"以其他使用者身分執行", L"以其他用戶身份運行"},
            {L"Run with PowerShell", L"使用 PowerShell 執行", L"使用 PowerShell 運行"},
            {L"Rotate Left", L"向左旋轉", L"向左旋轉"},
            {L"Rotate Right", L"向右旋轉", L"向右旋轉"},
            {L"Install Selected", L"安裝選取項目", L"安裝所選項目"},
            {L"Install Certificate", L"安裝憑證", L"安裝證書"},
            {L"Set as Desktop Background", L"設為桌面背景", L"設為桌面背景"},
            {L"Set as Desktop Slideshow", L"設為桌面投影片", L"設為桌面幻燈片"},
            {L"Storage Sense", L"儲存空間感知器", L"存儲感知"},
            {L"FTP / FTPS Manager...", L"FTP／FTPS 管理員...", L"FTP／FTPS 管理器..."},
            {L"Server URL", L"伺服器網址", L"伺服器網址"},
            {L"Username", L"使用者名稱", L"用戶名"},
            {L"Require TLS", L"要求 TLS", L"要求 TLS"},
            {L"Connect", L"連線", L"連接"},
            {L"Open Folder", L"開啟資料夾", L"打開文件夾"},
            {L"Download Here", L"下載到目前位置", L"下載到當前位置"},
            {L"Upload File...", L"上載檔案...", L"上傳文件..."},
            {L"Delete File", L"刪除檔案", L"刪除文件"},
            {L"Delete Folder", L"刪除資料夾", L"刪除文件夾"},
            {L"Cancel Transfer", L"取消傳輸", L"取消傳輸"},
            {L"Ready", L"就緒", L"就緒"},
            {L"Loading...", L"正在載入...", L"正在加載..."},
            {L"Plain FTP sends credentials and data without encryption. Continue?",
                L"純 FTP 會以未加密方式傳送憑證及資料。要繼續嗎？",
                L"純 FTP 會以未加密方式傳送憑證和數據。是否繼續？"},
            {L"Enter an ftp:// or ftps:// folder URL without embedded credentials. Spaces must be percent-encoded.",
                L"請輸入不含內嵌憑證的 ftp:// 或 ftps:// 資料夾網址；空格必須以百分號編碼。",
                L"請輸入不含內嵌憑證的 ftp:// 或 ftps:// 文件夾網址；空格必須使用百分號編碼。"}
        }};

        [[nodiscard]] std::wstring moduleDirectory()
        {
            std::wstring path(32768, L'\0');
            const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
            if (length == 0 || length >= path.size()) return {};
            path.resize(length);
            const std::size_t separator = path.find_last_of(L"\\/");
            return separator == std::wstring::npos ? std::wstring{} : path.substr(0, separator);
        }

        [[nodiscard]] const wchar_t* localeFilename(core::Locale locale) noexcept
        {
            switch (locale)
            {
            case core::Locale::traditionalChinese: return L"zh-Hant.lang";
            case core::Locale::simplifiedChinese: return L"zh-Hans.lang";
            case core::Locale::english: return L"en-US.lang";
            case core::Locale::system: break;
            }
            return nullptr;
        }

        [[nodiscard]] bool readLocalePack(core::Locale locale,
            std::vector<core::LocaleOverride>& parsed)
        {
            const wchar_t* filename = localeFilename(locale);
            const std::wstring directory = moduleDirectory();
            if (filename == nullptr || directory.empty()) return false;
            const std::wstring path = directory + L"\\Locales\\" + filename;
            HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (file == INVALID_HANDLE_VALUE) return false;
            LARGE_INTEGER size{};
            bool valid = GetFileSizeEx(file, &size) != FALSE && size.QuadPart > 0 &&
                size.QuadPart <= static_cast<LONGLONG>(core::maxLocalePackBytes);
            std::string bytes;
            if (valid)
            {
                bytes.resize(static_cast<std::size_t>(size.QuadPart));
                DWORD read{};
                valid = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) != FALSE &&
                    read == static_cast<DWORD>(bytes.size());
            }
            CloseHandle(file);
            if (!valid) return false;
            std::size_t offset{};
            if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xef &&
                static_cast<unsigned char>(bytes[1]) == 0xbb &&
                static_cast<unsigned char>(bytes[2]) == 0xbf)
                offset = 3;
            const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                bytes.data() + offset, static_cast<int>(bytes.size() - offset), nullptr, 0);
            if (required <= 0) return false;
            std::wstring decoded(static_cast<std::size_t>(required), L'\0');
            if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data() + offset,
                    static_cast<int>(bytes.size() - offset), decoded.data(), required) != required)
                return false;
            static_assert(sizeof(wchar_t) == sizeof(char16_t));
            const std::u16string_view source(reinterpret_cast<const char16_t*>(decoded.data()),
                decoded.size());
            return core::parseLocalePack(source, static_cast<std::size_t>(Text::count), parsed);
        }
    }

    void Localizer::setLocale(core::Locale locale) noexcept
    {
        locale_ = core::resolveLocale(locale, GetUserDefaultUILanguage());
        for (auto& value : overrides_) value.clear();
        try
        {
            std::vector<core::LocaleOverride> parsed;
            if (!readLocalePack(locale_, parsed)) return;
            for (auto& entry : parsed)
            {
                static_assert(sizeof(wchar_t) == sizeof(char16_t));
                overrides_[entry.index].assign(
                    reinterpret_cast<const wchar_t*>(entry.value.data()), entry.value.size());
            }
        }
        catch (...)
        {
            for (auto& value : overrides_) value.clear();
        }
    }

    const wchar_t* Localizer::operator()(Text text) const noexcept
    {
        const std::size_t index = static_cast<std::size_t>(text);
        if (!overrides_[index].empty()) return overrides_[index].c_str();
        const std::size_t language = locale_ == core::Locale::traditionalChinese ? 1 :
            locale_ == core::Locale::simplifiedChinese ? 2 : 0;
        return strings[index][language];
    }
}
