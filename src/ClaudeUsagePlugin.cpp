// Claude 用量 TrafficMonitor 插件
// 任务栏上显示两行：5h / 7d 的用量百分比。
//
// 数据来源跟 claude code 自己用的一样：从 ~/.claude/.credentials.json 拿
// access token，再打 /api/oauth/usage 拿 five_hour / seven_day 的 utilization。
// 打 usage 之前先看一眼 claude.ai/cdn-cgi/trace 的 colo/loc，不是预期的出口
// 节点就不发请求（免得 token 在奇怪的地方裸奔），顺便弹个通知。

#include <windows.h>
#include <winhttp.h>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <fstream>
#include <sstream>
#include <cctype>

#include "PluginInterface.h"
#include "json.hpp"

#pragma comment(lib, "winhttp.lib")

using json = nlohmann::json;

static constexpr int     DEFAULT_INTERVAL_MIN = 3;           // 默认 3 分钟刷新，太勤会 429
static constexpr wchar_t API_HOST[]    = L"api.anthropic.com";
static constexpr wchar_t API_PATH[]    = L"/api/oauth/usage";
static constexpr wchar_t TRACE_HOST[]  = L"claude.ai";
static constexpr wchar_t TRACE_PATH[]  = L"/cdn-cgi/trace";

#define ID_EDIT_COLO     1001
#define ID_EDIT_LOC      1002
#define ID_EDIT_CRED     1003
#define ID_EDIT_INTERVAL 1004
#define ID_CHECK_MANUAL  1005

HINSTANCE g_hModule = nullptr;

// 宽窄字符串互转（统一走 UTF-8）
static std::string W2A(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring A2W(const std::string& a)
{
    if (a.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, a.c_str(), (int)a.size(), nullptr, 0);
    std::wstring s(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, a.c_str(), (int)a.size(), s.data(), n);
    return s;
}

static bool IEquals(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::toupper((unsigned char)a[i]) != std::toupper((unsigned char)b[i]))
            return false;
    return true;
}

static std::string Trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// 一个够用的 HTTPS GET。返回是否拿到 200，body / status 带出去
static bool HttpsGet(const wchar_t* host, const wchar_t* path,
                     const std::wstring& extraHeaders,
                     std::string& body, DWORD& status)
{
    body.clear();
    status = 0;

    HINTERNET hSession = WinHttpOpen(L"claude-limit-widget/1.0",
                                     WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    HINTERNET hConnect = WinHttpConnect(hSession, host,
                                        INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path,
                                            nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    bool ok = false;
    bool headerOk = true;
    if (!extraHeaders.empty())
        headerOk = !!WinHttpAddRequestHeaders(hRequest, extraHeaders.c_str(), (DWORD)-1,
                                              WINHTTP_ADDREQ_FLAG_ADD);

    if (headerOk &&
        WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, nullptr))
    {
        DWORD szStatus = sizeof(status);
        WinHttpQueryHeaders(hRequest,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &szStatus,
                            WINHTTP_NO_HEADER_INDEX);

        DWORD avail = 0;
        do
        {
            avail = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0) break;
            std::vector<char> chunk(avail);
            DWORD read = 0;
            if (!WinHttpReadData(hRequest, chunk.data(), avail, &read) || read == 0) break;
            body.append(chunk.data(), read);
        } while (avail > 0);

        ok = (status == 200);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok;
}

static std::string ReadFileUtf8(const std::wstring& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::vector<std::wstring> CredentialPaths(const std::wstring& customPath)
{
    std::vector<std::wstring> paths;

    // 设置里手动指定的路径优先
    if (!customPath.empty())
        paths.push_back(customPath);

    wchar_t buf[MAX_PATH];

    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
        paths.push_back(std::wstring(buf) + L"\\.claude\\.credentials.json");

    n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
        paths.push_back(std::wstring(buf) + L"\\.claude\\.credentials.json");

    return paths;
}

// 自动查找时的默认路径：取第一个存在的候选；都不存在则取第一个候选
static std::wstring DefaultCredPath()
{
    auto cands = CredentialPaths(L"");
    for (const auto& p : cands)
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES)
            return p;
    return cands.empty() ? L"" : cands.front();
}

static std::string LoadAccessToken(const std::wstring& customPath)
{
    for (const auto& path : CredentialPaths(customPath))
    {
        std::string raw = ReadFileUtf8(path);
        if (raw.empty()) continue;
        try
        {
            json data = json::parse(raw);
            const json* oauth = &data;
            if (data.contains("claudeAiOauth") && data["claudeAiOauth"].is_object())
                oauth = &data["claudeAiOauth"];

            if (oauth->contains("accessToken") && (*oauth)["accessToken"].is_string())
                return (*oauth)["accessToken"].get<std::string>();
            if (oauth->contains("access_token") && (*oauth)["access_token"].is_string())
                return (*oauth)["access_token"].get<std::string>();
        }
        catch (...) {}
    }
    return {};
}

static int ExtractUtilization(const json& data, const char* key)
{
    if (!data.contains(key) || !data[key].is_object()) return -1;
    const json& obj = data[key];
    if (!obj.contains("utilization")) return -1;
    const json& u = obj["utilization"];
    if (u.is_number())
        return (int)(u.get<double>() + 0.5);   // 四舍五入成整数百分比
    return -1;
}

// 解析 cdn-cgi/trace 的 key=value 文本，取 colo / loc
static void ParseTrace(const std::string& body, std::string& colo, std::string& loc)
{
    std::istringstream ss(body);
    std::string line;
    while (std::getline(ss, line))
    {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = Trim(line.substr(0, eq));
        std::string v = Trim(line.substr(eq + 1));
        if (k == "colo")     colo = v;
        else if (k == "loc") loc  = v;
    }
}

class ClaudeUsagePlugin;   // 下面 OnMouseEvent 要回调单例

// 一个显示项目（5h 或 7d）
class UsageItem : public IPluginItem
{
public:
    // prefix 比如 "5h: "。注意整串（含前缀）都塞进 value、label 返回空：
    // TrafficMonitor 会把 label 尾部空格吃掉，分开放会变成 "5h:6 %"。
    UsageItem(const wchar_t* name, const wchar_t* id, const wchar_t* prefix)
        : m_name(name), m_id(id), m_prefix(prefix)
        , m_value(std::wstring(prefix) + L"-- %")
        , m_sample(std::wstring(prefix) + L"100 %") {}

    const wchar_t* GetItemName()            const override { return m_name.c_str(); }
    const wchar_t* GetItemId()              const override { return m_id.c_str(); }
    const wchar_t* GetItemLableText()       const override { return L""; }
    const wchar_t* GetItemValueText()       const override { return m_value.c_str(); }
    const wchar_t* GetItemValueSampleText() const override { return m_sample.c_str(); }

    void SetPercent(int pct)   // 由主线程（DataRequired）调用，-1 表示未知
    {
        std::wstring num = (pct < 0) ? L"--" : std::to_wstring(pct);
        while (num.size() < 2) num = L" " + num;   // 右对齐到宽度 2，让单位列对齐
        m_value = m_prefix + num + L" %";
    }

    // 手动刷新进行中：显示一个滚动的字符动画，让用户看到「正在刷新」
    void SetRefreshing(int frame)
    {
        static const wchar_t sp[] = { L'|', L'/', L'-', L'\\' };
        wchar_t c = sp[(unsigned)frame & 3];
        m_value = m_prefix + L"  " + std::wstring(1, c);
    }

    // 左键点击任一项 → 立刻手动刷新一次（定义在插件类之后）
    int OnMouseEvent(MouseEventType type, int x, int y, void* hWnd, int flag) override;

private:
    std::wstring m_name, m_id, m_prefix, m_value, m_sample;
};

class ClaudeUsagePlugin : public ITMPlugin
{
public:
    static ClaudeUsagePlugin& Instance()
    {
        static ClaudeUsagePlugin inst;
        return inst;
    }

    IPluginItem* GetItem(int index) override
    {
        if (index == 0) return &m_fiveHour;
        if (index == 1) return &m_sevenDay;
        return nullptr;
    }

    // 主程序定时调用：刷新显示项目 + 在主线程派发待处理的通知
    void DataRequired() override
    {
        // 手动刷新中（含点击后至少 MIN_SPIN_MS 的最短显示窗口）走滚动动画，
        // 否则正常显示百分比。最短窗口保证就算刷新很快也能看到动画。
        bool spin = m_refreshing.load() ||
                    (GetTickCount64() - m_refreshStart.load() < MIN_SPIN_MS);
        if (spin)
        {
            m_fiveHour.SetRefreshing(m_animFrame);
            m_sevenDay.SetRefreshing(m_animFrame);
            ++m_animFrame;
        }
        else
        {
            m_fiveHour.SetPercent(m_fivePct.load());
            m_sevenDay.SetPercent(m_sevenPct.load());
        }

        if (m_hasPendingNotify.exchange(false) && m_pApp)
        {
            std::wstring msg;
            { std::lock_guard<std::mutex> lk(m_mutex); msg = m_pendingNotifyMsg; }
            m_pApp->ShowNotifyMessage(msg.c_str());
        }
    }

    const wchar_t* GetInfo(PluginInfoIndex index) override
    {
        switch (index)
        {
        case TMI_NAME:        return L"Claude 用量";
        case TMI_DESCRIPTION: return L"在任务栏显示 Claude 订阅用量（5h / 7d），请求前校验出口节点";
        case TMI_AUTHOR:      return L"LangYa";
        case TMI_COPYRIGHT:   return L"MIT License";
        case TMI_VERSION:     return L"1.2.0";
        case TMI_URL:         return L"https://github.com/LangYa466/trafficmonitor-claude-usage";
        default:              return L"";
        }
    }

    const wchar_t* GetTooltipInfo() override
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_tooltip.c_str();
    }

    void OnInitialize(ITrafficMonitor* pApp) override
    {
        m_pApp = pApp;
        if (pApp)
        {
            const wchar_t* dir = pApp->GetPluginConfigDir();
            if (dir && *dir) SetPaths(dir);
        }
        LoadConfig();
        Log(L"=== OnInitialize，插件启动 ===");

        if (!m_started.exchange(true))
            std::thread(&ClaudeUsagePlugin::Worker, this).detach();
    }

    void OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data) override
    {
        if (index == EI_CONFIG_DIR && data && *data && m_iniPath.empty())
        {
            SetPaths(data);
            LoadConfig();
        }
    }

    OptionReturn ShowOptionsDialog(void* hParent) override;

    // 下面两个给设置对话框读写配置用
    void GetConfigW(std::wstring& colo, std::wstring& loc, std::wstring& credPath,
                    std::wstring& intervalMin, bool& manualFirst)
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        colo        = A2W(m_cfgColo);
        loc         = A2W(m_cfgLoc);
        // 未手动指定时，对话框里预填自动查找的默认路径
        credPath    = m_cfgCredPath.empty() ? DefaultCredPath() : m_cfgCredPath;
        intervalMin = std::to_wstring(m_cfgIntervalMin);
        manualFirst = m_cfgManualFirst;
    }

    void SaveConfig(const std::wstring& colo, const std::wstring& loc,
                    const std::wstring& credPath, int intervalMin, bool manualFirst)
    {
        if (intervalMin < 1) intervalMin = 1;
        if (!m_iniPath.empty())
        {
            WritePrivateProfileStringW(L"geo", L"colo", colo.c_str(), m_iniPath.c_str());
            WritePrivateProfileStringW(L"geo", L"loc",  loc.c_str(),  m_iniPath.c_str());
            WritePrivateProfileStringW(L"token", L"credentials_path",
                                       credPath.c_str(), m_iniPath.c_str());
            WritePrivateProfileStringW(L"refresh", L"interval_min",
                                       std::to_wstring(intervalMin).c_str(), m_iniPath.c_str());
            WritePrivateProfileStringW(L"refresh", L"manual_first",
                                       manualFirst ? L"1" : L"0", m_iniPath.c_str());
        }
        std::lock_guard<std::mutex> lk(m_mutex);
        m_cfgColo        = W2A(colo);
        m_cfgLoc         = W2A(loc);
        m_cfgCredPath    = credPath;
        m_cfgIntervalMin = intervalMin;
        m_cfgManualFirst = manualFirst;
        m_lastGeo.store(GEO_UNKNOWN);   // 配置变了，重新评估匹配状态
    }

    // 点击显示项触发：唤醒 worker 立刻刷新一次，失败会走系统通知
    void TriggerManualRefresh()
    {
        m_refreshStart.store(GetTickCount64());   // 动画最短显示窗口起点
        m_refreshing.store(true);
        m_manualPending.store(true);
        m_wakeCv.notify_one();
    }

private:
    enum GeoState { GEO_UNKNOWN, GEO_OK, GEO_MISMATCH, GEO_ERROR };

    ClaudeUsagePlugin()
        // ID 换新（claude5h→claude_5h_v2），让 TrafficMonitor 不再套用旧的缓存 label
        : m_fiveHour(L"Claude 5h 用量", L"claude_5h_v2", L"5h: ")
        , m_sevenDay(L"Claude 7d 用量", L"claude_7d_v2", L"7d: ")
        , m_tooltip(L"Claude 用量加载中...")
        , m_cfgColo("NRT")
        , m_cfgLoc("JP")
        , m_cfgIntervalMin(DEFAULT_INTERVAL_MIN)
        , m_cfgManualFirst(false)
    {}

    // 由配置目录推导 ini / log 路径
    void SetPaths(const std::wstring& dir)
    {
        m_iniPath = dir + L"\\ClaudeUsage.ini";
        m_logPath = dir + L"\\ClaudeUsage.log";
    }

    // 写一行带时间戳的日志（UTF-8）。文件过大时自动重置
    void Log(const std::wstring& msg)
    {
        if (m_logPath.empty()) return;
        std::ios::openmode mode = std::ios::app | std::ios::binary;
        std::ifstream chk(m_logPath, std::ios::binary | std::ios::ate);
        if (chk && chk.tellg() > 256 * 1024) mode = std::ios::trunc | std::ios::binary;
        chk.close();

        std::ofstream f(m_logPath, mode);
        if (!f) return;
        SYSTEMTIME st; GetLocalTime(&st);
        wchar_t ts[40];
        swprintf(ts, 40, L"[%04d-%02d-%02d %02d:%02d:%02d] ",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        std::string line = W2A(std::wstring(ts) + msg) + "\r\n";
        f.write(line.data(), (std::streamsize)line.size());
    }

    void LoadConfig()
    {
        std::wstring colo = L"NRT", loc = L"JP", credPath;
        if (!m_iniPath.empty())
        {
            wchar_t buf[128];
            GetPrivateProfileStringW(L"geo", L"colo", L"NRT", buf, 128, m_iniPath.c_str());
            colo = buf;
            GetPrivateProfileStringW(L"geo", L"loc", L"JP", buf, 128, m_iniPath.c_str());
            loc = buf;

            wchar_t pbuf[MAX_PATH];
            GetPrivateProfileStringW(L"token", L"credentials_path", L"",
                                     pbuf, MAX_PATH, m_iniPath.c_str());
            credPath = pbuf;
        }
        int interval = DEFAULT_INTERVAL_MIN;
        bool manualFirst = false;
        if (!m_iniPath.empty())
        {
            interval = (int)GetPrivateProfileIntW(L"refresh", L"interval_min",
                                                  DEFAULT_INTERVAL_MIN, m_iniPath.c_str());
            manualFirst = GetPrivateProfileIntW(L"refresh", L"manual_first", 0,
                                                m_iniPath.c_str()) != 0;
        }
        if (interval < 1) interval = 1;

        std::lock_guard<std::mutex> lk(m_mutex);
        m_cfgColo        = W2A(colo);
        m_cfgLoc         = W2A(loc);
        m_cfgCredPath    = credPath;
        m_cfgIntervalMin = interval;
        m_cfgManualFirst = manualFirst;
    }

    void Worker()
    {
        bool firstDone = false;          // 是否已经成功跑过一次（含手动触发的首次）
        for (;;)
        {
            bool manual = m_manualPending.exchange(false);

            bool manualFirst;
            { std::lock_guard<std::mutex> lk(m_mutex); manualFirst = m_cfgManualFirst; }

            // 「首次需手动」开启时：在第一次手动点击之前不自动拉，
            // 避免开机后出口/token 没就绪时误报失败、让人摸不着头脑。
            if (manualFirst && !firstDone && !manual)
            {
                SetTooltip(L"已开启“首次需手动刷新”，请点击任务栏 5h/7d 触发第一次获取");
                m_refreshing.store(false);
            }
            else
            {
                Fetch(manual);
                if (manual) m_refreshing.store(false);   // 刷新结束，停止动画
                firstDone = true;
            }

            int mins;
            { std::lock_guard<std::mutex> lk(m_mutex); mins = m_cfgIntervalMin; }
            if (mins < 1) mins = 1;

            // 睡到下一个间隔，期间被 TriggerManualRefresh 唤醒就提前返回
            std::unique_lock<std::mutex> lk(m_wakeMutex);
            m_wakeCv.wait_for(lk, std::chrono::seconds(mins * 60),
                              [this] { return m_manualPending.load(); });
        }
    }

    void SetTooltip(const std::wstring& s)
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_tooltip = s;
    }

    // 排队一条系统通知，由 DataRequired 在主线程派发
    void QueueNotify(const std::wstring& msg)
    {
        { std::lock_guard<std::mutex> lk(m_mutex); m_pendingNotifyMsg = msg; }
        m_hasPendingNotify.store(true);
    }

    // 进入“不匹配”状态时通知一次（不重复）
    void OnGeoState(GeoState s, const std::string& colo, const std::string& loc,
                    const std::string& cfgColo, const std::string& cfgLoc)
    {
        int prev = m_lastGeo.exchange(s);
        if (s == GEO_MISMATCH && prev != GEO_MISMATCH)
        {
            std::wstring msg =
                L"Claude 出口节点不匹配，已暂停 API 请求\n"
                L"当前 colo=" + A2W(colo) + L"  loc=" + A2W(loc) + L"\n"
                L"需要 colo=" + A2W(cfgColo) + L"  loc=" + A2W(cfgLoc);
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                m_pendingNotifyMsg = msg;
            }
            m_hasPendingNotify.store(true);   // 由 DataRequired 在主线程派发
        }
    }

    // manual=true 表示用户点击触发的手动刷新；失败时额外弹系统通知说明原因
    void Fetch(bool manual)
    {
        // 失败时统一出口：写 tooltip，手动刷新还要弹通知
        auto fail = [&](const std::wstring& reason)
        {
            SetTooltip(reason);
            if (manual) QueueNotify(L"Claude 用量刷新失败：\n" + reason);
        };

        std::string cfgColo, cfgLoc;
        std::wstring cfgCredPath;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            cfgColo = m_cfgColo; cfgLoc = m_cfgLoc; cfgCredPath = m_cfgCredPath;
        }

        Log(std::wstring(L"--- Fetch 开始 (") + (manual ? L"手动 " : L"自动 ") +
            L"期望 colo=" + A2W(cfgColo) + L" loc=" + A2W(cfgLoc) + L") ---");

        // ── 1. 出口节点检查 ──────────────────────────────────────────
        std::string traceBody; DWORD st = 0;
        if (!HttpsGet(TRACE_HOST, TRACE_PATH, L"", traceBody, st))
        {
            m_lastGeo.store(GEO_ERROR);
            Log(L"trace 请求失败 (status=" + std::to_wstring(st) + L")，跳过");
            fail(L"无法连接 claude.ai 校验出口节点");
            return;
        }

        std::string colo, loc;
        ParseTrace(traceBody, colo, loc);
        Log(L"trace 结果: colo=" + A2W(colo) + L" loc=" + A2W(loc));

        bool match = IEquals(colo, cfgColo) && IEquals(loc, cfgLoc);
        if (!match)
        {
            Log(L"节点不匹配，暂停 API 请求");
            OnGeoState(GEO_MISMATCH, colo, loc, cfgColo, cfgLoc);
            fail(L"出口节点不匹配：colo=" + A2W(colo) + L" loc=" + A2W(loc) +
                 L"（需 " + A2W(cfgColo) + L"/" + A2W(cfgLoc) + L"），已暂停请求");
            return;   // 不请求 API
        }
        Log(L"节点匹配，继续请求用量");
        OnGeoState(GEO_OK, colo, loc, cfgColo, cfgLoc);

        // ── 2. 请求用量（节点匹配才执行）─────────────────────────────
        std::string token = LoadAccessToken(cfgCredPath);
        if (token.empty())
        {
            Log(L"未找到 token（cred 路径=" + (cfgCredPath.empty() ? L"自动" : cfgCredPath) + L"）");
            fail(L"找不到 OAuth token，请先登录 Claude Code 或在设置里指定 json 路径");
            return;
        }
        Log(L"已读取 token，长度=" + std::to_wstring(token.size()));

        std::wstring headers =
            L"Authorization: Bearer " + std::wstring(token.begin(), token.end()) + L"\r\n"
            L"anthropic-beta: oauth-2025-04-20\r\n"
            L"Content-Type: application/json\r\n"
            L"Accept: application/json";

        std::string body; DWORD status = 0;
        if (!HttpsGet(API_HOST, API_PATH, headers, body, status))
        {
            Log(L"用量请求失败 (HTTP status=" + std::to_wstring(status) + L")");
            if (status == 429)      fail(L"API 被限流（429），稍后重试");
            else if (status == 401) fail(L"Token 已过期，请重新登录");
            else if (status != 0)   fail(L"HTTP 错误：" + std::to_wstring(status));
            else                    fail(L"网络错误");
            return;
        }

        try
        {
            json data = json::parse(body);
            int five  = ExtractUtilization(data, "five_hour");
            int seven = ExtractUtilization(data, "seven_day");
            if (five  >= 0) m_fivePct.store(five);
            if (seven >= 0) m_sevenPct.store(seven);
            Log(L"用量更新成功: 5h=" + std::to_wstring(five) + L"% 7d=" + std::to_wstring(seven) + L"%");

            std::wstring fv = m_fivePct.load() < 0 ? L"-- %" : std::to_wstring(m_fivePct.load()) + L" %";
            std::wstring sv = m_sevenPct.load() < 0 ? L"-- %" : std::to_wstring(m_sevenPct.load()) + L" %";
            SetTooltip(L"5h: " + fv + L"   7d: " + sv + L"   (" + A2W(colo) + L"/" + A2W(loc) + L")");
        }
        catch (...)
        {
            Log(L"解析用量 JSON 失败，body 前 200 字符: " + A2W(body.substr(0, 200)));
            fail(L"解析用量数据失败");
        }
    }

    UsageItem m_fiveHour;
    UsageItem m_sevenDay;

    std::atomic<int>  m_fivePct{ -1 };
    std::atomic<int>  m_sevenPct{ -1 };
    std::atomic<bool> m_started{ false };
    std::atomic<int>  m_lastGeo{ GEO_UNKNOWN };
    std::atomic<bool> m_hasPendingNotify{ false };
    std::atomic<bool> m_manualPending{ false };   // 点击触发的手动刷新待处理

    // 手动刷新动画：m_refreshing 标记刷新中，m_refreshStart 给一个最短显示窗口，
    // m_animFrame 只在 DataRequired（主线程）里读写。
    static constexpr unsigned long long MIN_SPIN_MS = 600;
    std::atomic<bool>               m_refreshing{ false };
    std::atomic<unsigned long long> m_refreshStart{ 0 };
    int                             m_animFrame{ 0 };

    std::mutex              m_wakeMutex;           // 配合 m_wakeCv 唤醒 worker
    std::condition_variable m_wakeCv;

    ITrafficMonitor* m_pApp{ nullptr };
    std::wstring      m_iniPath;
    std::wstring      m_logPath;

    std::mutex   m_mutex;            // 保护下面这些
    std::wstring m_tooltip;
    std::wstring m_pendingNotifyMsg;
    std::string  m_cfgColo;
    std::string  m_cfgLoc;
    std::wstring m_cfgCredPath;      // 手动指定的 credentials.json 路径（空=自动查找）
    int          m_cfgIntervalMin;   // 刷新间隔（分钟）
    bool         m_cfgManualFirst;   // 开机后首次必须手动触发，不自动拉
};

// 左键点击 5h/7d 任一项，立刻强制刷新一次
int UsageItem::OnMouseEvent(MouseEventType type, int, int, void*, int)
{
    if (type == MT_LCLICKED || type == MT_DBCLICKED)
    {
        ClaudeUsagePlugin::Instance().TriggerManualRefresh();
        return 1;
    }
    return 0;
}

// 设置对话框。懒得弄 .rc 资源文件，直接在内存里拼 DLGTEMPLATE
namespace {

struct DlgTpl
{
    std::vector<BYTE> b;
    void Align()              { while (b.size() & 3) b.push_back(0); }
    void W16(WORD v)          { b.push_back((BYTE)(v & 0xFF)); b.push_back((BYTE)(v >> 8)); }
    void W32(DWORD v)         { W16((WORD)(v & 0xFFFF)); W16((WORD)(v >> 16)); }
    void Str(const wchar_t* s){ while (*s) W16((WORD)*s++); W16(0); }

    void Item(DWORD style, short x, short y, short cx, short cy,
              WORD id, WORD cls, const wchar_t* text)
    {
        Align();
        W32(style);
        W32(0);                      // exStyle
        W16((WORD)x); W16((WORD)y); W16((WORD)cx); W16((WORD)cy);
        W16(id);
        W16(0xFFFF); W16(cls);       // 预定义控件类
        Str(text);
        W16(0);                      // 无 creation data
    }
};

INT_PTR CALLBACK OptionsDlgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_INITDIALOG)
    {
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)lParam);
        auto* self = reinterpret_cast<ClaudeUsagePlugin*>(lParam);
        if (self)
        {
            std::wstring colo, loc, cred, interval;
            bool manualFirst = false;
            self->GetConfigW(colo, loc, cred, interval, manualFirst);
            SetDlgItemTextW(hWnd, ID_EDIT_COLO,     colo.c_str());
            SetDlgItemTextW(hWnd, ID_EDIT_LOC,      loc.c_str());
            SetDlgItemTextW(hWnd, ID_EDIT_CRED,     cred.c_str());
            SetDlgItemTextW(hWnd, ID_EDIT_INTERVAL, interval.c_str());
            CheckDlgButton(hWnd, ID_CHECK_MANUAL,
                           manualFirst ? BST_CHECKED : BST_UNCHECKED);
        }
        return TRUE;
    }
    if (msg == WM_COMMAND)
    {
        WORD id = LOWORD(wParam);
        if (id == IDOK)
        {
            auto* self = reinterpret_cast<ClaudeUsagePlugin*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
            wchar_t bColo[128] = {0}, bLoc[128] = {0}, bCred[MAX_PATH] = {0}, bInt[32] = {0};
            GetDlgItemTextW(hWnd, ID_EDIT_COLO,     bColo, 128);
            GetDlgItemTextW(hWnd, ID_EDIT_LOC,      bLoc, 128);
            GetDlgItemTextW(hWnd, ID_EDIT_CRED,     bCred, MAX_PATH);
            GetDlgItemTextW(hWnd, ID_EDIT_INTERVAL, bInt, 32);
            bool manualFirst = (IsDlgButtonChecked(hWnd, ID_CHECK_MANUAL) == BST_CHECKED);

            int interval = _wtoi(bInt);
            if (interval < 1) interval = 1;
            // 低于默认值时警告：容易被速率限制
            if (interval < DEFAULT_INTERVAL_MIN)
            {
                std::wstring warn = L"刷新间隔设为 " + std::to_wstring(interval) +
                    L" 分钟，低于默认的 " + std::to_wstring(DEFAULT_INTERVAL_MIN) +
                    L" 分钟，容易触发 API 速率限制（HTTP 429）。\n\n仍要使用此值吗？";
                if (MessageBoxW(hWnd, warn.c_str(), L"间隔过短警告",
                                MB_ICONWARNING | MB_YESNO) != IDYES)
                    return TRUE;   // 用户取消，留在对话框继续编辑
            }
            if (self) self->SaveConfig(bColo, bLoc, bCred, interval, manualFirst);
            EndDialog(hWnd, IDOK);
            return TRUE;
        }
        if (id == IDCANCEL)
        {
            EndDialog(hWnd, IDCANCEL);
            return TRUE;
        }
    }
    return FALSE;
}

} // namespace

ITMPlugin::OptionReturn ClaudeUsagePlugin::ShowOptionsDialog(void* hParent)
{
    DlgTpl t;
    DWORD style = DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    t.W32(style);
    t.W32(0);                       // exStyle
    t.W16(13);                      // 控件数量
    t.W16(0); t.W16(0);             // x, y
    t.W16(300); t.W16(200);         // cx, cy
    t.W16(0);                       // 无菜单
    t.W16(0);                       // 默认窗口类
    t.Str(L"Claude 用量设置");      // 标题
    t.W16(9);                       // DS_SETFONT: 字号
    t.Str(L"Segoe UI");             // 字体

    t.Item(WS_CHILD | WS_VISIBLE,                                            12, 14, 46, 10, 0xFFFF, 0x0082, L"colo:");
    t.Item(WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,  62, 12, 226, 12, ID_EDIT_COLO, 0x0081, L"");
    t.Item(WS_CHILD | WS_VISIBLE,                                            12, 36, 46, 10, 0xFFFF, 0x0082, L"loc:");
    t.Item(WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,  62, 34, 226, 12, ID_EDIT_LOC, 0x0081, L"");
    t.Item(WS_CHILD | WS_VISIBLE,                                            12, 58, 46, 10, 0xFFFF, 0x0082, L"jsonpath:");
    t.Item(WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,  62, 56, 226, 12, ID_EDIT_CRED, 0x0081, L"");
    t.Item(WS_CHILD | WS_VISIBLE,                                            12, 80, 46, 10, 0xFFFF, 0x0082, L"间隔(分):");
    t.Item(WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER, 62, 78, 40, 12, ID_EDIT_INTERVAL, 0x0081, L"");
    t.Item(WS_CHILD | WS_VISIBLE,                                            12, 98, 276, 18, 0xFFFF, 0x0082, L"默认 3 分钟；小于默认值容易被 API 速率限制 (HTTP 429)。");
    t.Item(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,             12, 120, 276, 12, ID_CHECK_MANUAL, 0x0080, L"开机后首次必须手动刷新（不自动获取）");
    t.Item(WS_CHILD | WS_VISIBLE,                                            12, 136, 276, 36, 0xFFFF, 0x0082, L"勾选后开机不自动拉，需手动点一次 5h/7d 触发首次获取。观测到：开机后若没用 Claude Code，出口节点/token 常常还没就绪，自动获取会失败且看不出原因。（仍需出口校验通过后才会拉取用量）");
    t.Item(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,           176, 178, 52, 14, IDOK, 0x0080, L"确定");
    t.Item(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,              236, 178, 52, 14, IDCANCEL, 0x0080, L"取消");

    INT_PTR r = DialogBoxIndirectParamW(g_hModule,
                                        (LPCDLGTEMPLATEW)t.b.data(),
                                        (HWND)hParent,
                                        OptionsDlgProc,
                                        (LPARAM)this);
    return (r == IDOK) ? OR_OPTION_CHANGED : OR_OPTION_UNCHANGED;
}

extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance()
{
    return &ClaudeUsagePlugin::Instance();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_hModule = (HINSTANCE)hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}
