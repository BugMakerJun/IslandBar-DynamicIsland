// =====================================================================
//  WindowsFileSearch.cpp -- see the header.
// =====================================================================
#include "services/WindowsFileSearch.h"

#include <msdasc.h>
#include <oledb.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstddef>
#include <cwchar>

#include "core/Log.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "oledb.lib")
#pragma comment(lib, "msdasc.lib")

namespace island {
namespace {

using Microsoft::WRL::ComPtr;

bool IsApplicationShortcut(const std::wstring& path) {
    const wchar_t* name = path.c_str();
    const size_t length = std::wcslen(name);
    return (length >= 4 && _wcsicmp(name + length - 4, L".lnk") == 0) ||
           (length >= 4 && _wcsicmp(name + length - 4, L".url") == 0);
}

std::wstring EscapeSql(std::wstring query) {
    std::wstring out;
    out.reserve(query.size() + 8);
    for (wchar_t c : query) {
        if (c == L'\'') out += L"''";
        else if (c == L'%') out += L"[%]";
        else if (c == L'_') out += L"[_]";
        else if (c == L'[') out += L"[[]";
        else out += c;
    }
    return out;
}

std::wstring ShellPathFromUrl(const wchar_t* url) {
    if (!url || !url[0]) return {};
    ComPtr<IShellItem> item;
    if (SUCCEEDED(SHCreateItemFromParsingName(url, nullptr, IID_PPV_ARGS(&item))) && item) {
        LPWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &path)) && path) {
            std::wstring result(path);
            CoTaskMemFree(path);
            return result;
        }
    }
    if (_wcsnicmp(url, L"file:", 5) == 0) {
        std::wstring result(url + 5);
        std::replace(result.begin(), result.end(), L'/', L'\\');
        return result;
    }
    return {};
}

struct SearchBindings {
    wchar_t title[512] = {};
    DBLENGTH titleLength = 0;
    DBSTATUS titleStatus = 0;
    wchar_t url[2048] = {};
    DBLENGTH urlLength = 0;
    DBSTATUS urlStatus = 0;
    wchar_t type[128] = {};
    DBLENGTH typeLength = 0;
    DBSTATUS typeStatus = 0;
};

std::vector<StartSearchResult> QueryIndex(const std::wstring& query, int limit) {
    std::vector<StartSearchResult> results;
    if (query.empty() || limit <= 0) return results;

    wchar_t profile[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH) == 0) return results;
    std::wstring profileUrl = profile;
    std::replace(profileUrl.begin(), profileUrl.end(), L'\\', L'/');
    const std::wstring sql =
        L"SELECT TOP 24 System.ItemNameDisplay, System.ItemUrl, System.ItemTypeText "
        L"FROM SystemIndex WHERE System.FileName LIKE '%" +
        EscapeSql(query) + L"%' AND System.ItemUrl LIKE 'file:" + profileUrl + L"/%'";

    ComPtr<IDataInitialize> dataInitialize;
    if (FAILED(CoCreateInstance(CLSID_MSDAINITIALIZE, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dataInitialize))) ||
        !dataInitialize) {
        return results;
    }
    IUnknown* dataSourceUnknown = nullptr;
    const wchar_t* connection =
        L"Provider=Search.CollatorDSO;Extended Properties='Application=Windows';";
    if (FAILED(dataInitialize->GetDataSource(nullptr, CLSCTX_INPROC_SERVER, connection,
                                             IID_IDBInitialize, &dataSourceUnknown)) ||
        !dataSourceUnknown) {
        return results;
    }
    ComPtr<IDBInitialize> database;
    dataSourceUnknown->QueryInterface(IID_PPV_ARGS(&database));
    dataSourceUnknown->Release();
    if (!database || FAILED(database->Initialize())) return results;

    ComPtr<IDBCreateSession> sessions;
    if (FAILED(database->QueryInterface(IID_PPV_ARGS(&sessions))) || !sessions) return results;
    ComPtr<IDBCreateCommand> commandFactory;
    if (FAILED(sessions->CreateSession(nullptr, IID_IDBCreateCommand,
                                       reinterpret_cast<IUnknown**>(commandFactory.GetAddressOf()))) ||
        !commandFactory) {
        return results;
    }
    ComPtr<ICommandText> command;
    if (FAILED(commandFactory->CreateCommand(nullptr, IID_ICommandText,
                                             reinterpret_cast<IUnknown**>(command.GetAddressOf()))) ||
        !command ||
        FAILED(command->SetCommandText(DBGUID_DEFAULT, sql.c_str()))) {
        return results;
    }

    IUnknown* rowsetUnknown = nullptr;
    DBROWCOUNT affected = 0;
    if (FAILED(command->Execute(nullptr, IID_IRowset, nullptr, &affected, &rowsetUnknown)) ||
        !rowsetUnknown) {
        database->Uninitialize();
        return results;
    }
    ComPtr<IRowset> rowset;
    rowsetUnknown->QueryInterface(IID_PPV_ARGS(&rowset));
    rowsetUnknown->Release();
    if (!rowset) {
        database->Uninitialize();
        return results;
    }

    DBBINDING bindings[3] = {};
    const auto bind = [&](int index, DBORDINAL ordinal, DBBYTEOFFSET value,
                          DBBYTEOFFSET length, DBBYTEOFFSET status, DBLENGTH capacity) {
        DBBINDING& binding = bindings[index];
        binding.iOrdinal = ordinal;
        binding.obValue = value;
        binding.obLength = length;
        binding.obStatus = status;
        binding.dwPart = DBPART_VALUE | DBPART_LENGTH | DBPART_STATUS;
        binding.dwMemOwner = DBMEMOWNER_CLIENTOWNED;
        binding.eParamIO = DBPARAMIO_NOTPARAM;
        binding.cbMaxLen = capacity;
        binding.dwFlags = 0;
        binding.wType = DBTYPE_WSTR;
    };
    bind(0, 1, offsetof(SearchBindings, title), offsetof(SearchBindings, titleLength),
         offsetof(SearchBindings, titleStatus), sizeof(SearchBindings::title));
    bind(1, 2, offsetof(SearchBindings, url), offsetof(SearchBindings, urlLength),
         offsetof(SearchBindings, urlStatus), sizeof(SearchBindings::url));
    bind(2, 3, offsetof(SearchBindings, type), offsetof(SearchBindings, typeLength),
         offsetof(SearchBindings, typeStatus), sizeof(SearchBindings::type));

    ComPtr<IAccessor> accessor;
    if (FAILED(rowset->QueryInterface(IID_PPV_ARGS(&accessor))) || !accessor) {
        database->Uninitialize();
        return results;
    }
    HACCESSOR handle = DB_NULL_HACCESSOR;
    DBBINDSTATUS bindingStatus[3] = {};
    if (FAILED(accessor->CreateAccessor(DBACCESSOR_ROWDATA, 3, bindings, sizeof(SearchBindings),
                                        &handle, bindingStatus))) {
        database->Uninitialize();
        return results;
    }

    HROW row = DB_NULL_HROW;
    HROW* rows = &row;
    DBCOUNTITEM obtained = 0;
    while (static_cast<int>(results.size()) < limit &&
           SUCCEEDED(rowset->GetNextRows(DB_NULL_HCHAPTER, 0, 1, &obtained, &rows)) &&
           obtained == 1) {
        SearchBindings values;
        if (SUCCEEDED(rowset->GetData(row, handle, &values)) &&
            values.titleStatus == DBSTATUS_S_OK && values.urlStatus == DBSTATUS_S_OK) {
            std::wstring path = ShellPathFromUrl(values.url);
            if (!path.empty() && !IsApplicationShortcut(path)) {
                const DWORD attributes = GetFileAttributesW(path.c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES ||
                    (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                    rowset->ReleaseRows(1, &row, nullptr, nullptr, nullptr);
                    row = DB_NULL_HROW;
                    continue;
                }
                StartSearchResult result;
                result.kind = SearchResultKind::File;
                result.title = values.title;
                result.target = std::move(path);
                const size_t slash = result.target.find_last_of(L'\\');
                result.subtitle =
                    slash != std::wstring::npos && slash > 0
                        ? result.target.substr(0, slash)
                        : (values.typeStatus == DBSTATUS_S_OK && values.type[0]
                               ? values.type
                               : L"\u6587\u4EF6");
                result.score = 50;
                results.push_back(std::move(result));
            }
        }
        rowset->ReleaseRows(1, &row, nullptr, nullptr, nullptr);
        row = DB_NULL_HROW;
    }
    accessor->ReleaseAccessor(handle, nullptr);
    database->Uninitialize();
    return results;
}

} // namespace

WindowsFileSearch::~WindowsFileSearch() {
    Stop();
}

bool WindowsFileSearch::Start() {
    if (running_) return true;
    wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!worker_.Prepare() || !wakeEvent_) {
        Stop();
        return false;
    }
    DWORD threadId = 0;
    HANDLE thread = CreateThread(nullptr, 0, &WindowsFileSearch::ThreadEntry, this, 0, &threadId);
    if (!thread) {
        Stop();
        return false;
    }
    worker_.Attach(thread);
    running_ = true;
    return true;
}

bool WindowsFileSearch::Request(const wchar_t* query, std::uint64_t sequence) {
    if (!query || !query[0]) return false;
    if (!Start()) return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingQuery_ = query;
        pendingSequence_ = sequence;
        requestedSequence_ = sequence;
    }
    LogLine(L"  start.search.files request seq=%llu query='%ls'", sequence, query);
    SetEvent(wakeEvent_);
    return true;
}

bool WindowsFileSearch::Take(std::uint64_t* sequence, std::vector<StartSearchResult>* results,
                             bool* completed) {
    if (sequence) *sequence = 0;
    if (completed) *completed = false;
    if (results) results->clear();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!published_ || publishedSequence_ <= consumedSequence_) return false;
    consumedSequence_ = publishedSequence_;
    if (sequence) *sequence = publishedSequence_;
    if (results) *results = publishedResults_;
    if (completed) *completed = true;
    return true;
}

void WindowsFileSearch::Stop() {
    const bool wasRunning = running_;
    worker_.Stop();
    if (wakeEvent_) {
        CloseHandle(wakeEvent_);
        wakeEvent_ = nullptr;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    pendingQuery_.clear();
    pendingSequence_ = 0;
    requestedSequence_ = 0;
    published_ = false;
    running_ = false;
    if (wasRunning) {
        LogLine(L"  start.search.closed workers=0 pending_search=0 pending_icons=0");
    }
}

unsigned long __stdcall WindowsFileSearch::ThreadEntry(void* self) {
    static_cast<WindowsFileSearch*>(self)->Run();
    return 0;
}

void WindowsFileSearch::Run() {
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool haveApartment = SUCCEEDED(apartment) || apartment == RPC_E_CHANGED_MODE;
    while (haveApartment) {
        HANDLE waits[2] = {worker_.StopEvent(), wakeEvent_};
        const DWORD signalled = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (signalled == WAIT_OBJECT_0) break;
        if (signalled == WAIT_FAILED) break;

        std::wstring query;
        std::uint64_t sequence = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            query = pendingQuery_;
            sequence = pendingSequence_;
            pendingQuery_.clear();
            pendingSequence_ = 0;
        }
        if (query.empty() || sequence == 0) continue;

        std::vector<StartSearchResult> results = QueryIndex(query, 6);
        LogLine(L"  start.search.files result seq=%llu count=%zu", sequence, results.size());
        std::lock_guard<std::mutex> lock(mutex_);
        if (sequence == requestedSequence_) {
            publishedResults_ = std::move(results);
            publishedSequence_ = sequence;
            published_ = true;
        }
    }
    if (haveApartment) CoUninitialize();
    running_ = false;
}

} // namespace island
