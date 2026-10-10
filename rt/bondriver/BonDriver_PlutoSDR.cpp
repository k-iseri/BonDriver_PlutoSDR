// BonDriver_PlutoSDR.cpp: isdbt-sdr の BonDriver（IBonDriver2）。動作モードは 2 つ:
//
// 1) デーモンモード（既定）: 復調は別プロセス（isdbtd.exe / tools/demod_server.py）が行い、
//    この DLL は docs/daemon-protocol.md のプロトコルで
//      - 制御接続（JSON Lines）: hello / tune / status / quit
//      - TS ストリーム接続: 生の 188 バイト TSP 列
//    を扱うだけ。
// 2) スタンドアロンモード（ini の [SDR] Enable=1）: この DLL が rt/ の復調コアと libiio で
//    Pluto SDR を直接開き、DLL 内で復調する。isdbtd は不要で、DLL と ini を TVTest の
//    BonDriver フォルダに置くだけで受信できる（libiio は遅延ロード。ADI のインストーラで
//    入っているか、DLL の隣 / LibiioDir に libiio の DLL 一式があればよい）。
//
// 設定は DLL と同じ場所の同名 .ini（BonDriver_PlutoSDR.ini）から読む。
#define BONDRIVER_EXPORTS
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX            // CMake 側でも定義している
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../demod/live_demod.h"
#include "IBonDriver2.h"

#pragma comment(lib, "ws2_32.lib")

namespace {

constexpr DWORD kTsPacket = 188;
constexpr size_t kChunkBytes = kTsPacket * 256;          // GetTsStream で返す 1 チャンクの上限（48128 バイト）
constexpr size_t kMinPushBytes = kTsPacket * 32;         // 受信スレッドがキューに積む最小単位
constexpr size_t kMaxQueueBytes = 16u << 20;             // 受信キュー上限（約 8 秒 @16 Mbps）。超えたら古い方を捨てる
constexpr DWORD kFirstChannel = 13;                      // 地デジ物理チャンネル
constexpr DWORD kLastChannel = 52;
constexpr DWORD kNumChannels = kLastChannel - kFirstChannel + 1;
constexpr DWORD kControlTimeoutMs = 2000;                // 制御コマンドの応答待ち
constexpr ULONGLONG kSignalCacheMs = 500;                // GetSignalLevel の status 問い合わせ間隔

HMODULE g_hModule = nullptr;

struct Config {
	std::string host = "127.0.0.1";
	int port = 47188;
	bool spawn = false;
	std::wstring command;
	std::wstring workDir;
	DWORD connectTimeoutMs = 15000;
	bool quitOnClose = true;
	bool log = false;
	std::wstring tunerName = L"ISDB-T SDR";
	std::wstring iniPath;
	std::wstring logPath;
	std::wstring dllDir;

	// [SDR] スタンドアロンモード（デーモン無しで DLL が直接 Pluto SDR を開く）
	struct {
		bool enable = false;
		std::string uri = "usb:";
		double fs = 0.0;            // 0 = SdrConfig の既定（7,111,111 Hz = 公称の 7/8）
		double bw = 0.0;            // 0 = SdrConfig の既定（5.6 MHz）
		bool agc = true;
		double gainDb = 0.0;
		int mode = 3;
		std::string gi = "1/8";
		int winFrames = 16;
		int ctxFrames = 4;
		int workers = 2;
		std::wstring libiioDir;     // libiio1.dll のあるフォルダ（空なら DLL の隣 → PATH）
	} sdr;
};

std::string Narrow(const std::wstring &w)
{
	if (w.empty()) return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s((size_t)n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
	return s;
}

std::wstring ReadIniString(const wchar_t *section, const wchar_t *key, const wchar_t *def, const std::wstring &ini)
{
	wchar_t buf[2048];
	GetPrivateProfileStringW(section, key, def, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])), ini.c_str());
	return buf;
}

double ReadIniDouble(const wchar_t *section, const wchar_t *key, double def, const std::wstring &ini)
{
	std::wstring s = ReadIniString(section, key, L"", ini);
	if (s.empty()) return def;
	wchar_t *end = nullptr;
	double v = wcstod(s.c_str(), &end);
	return end != s.c_str() ? v : def;
}

Config LoadConfig()
{
	Config c;
	wchar_t path[MAX_PATH] = {};
	GetModuleFileNameW(g_hModule, path, MAX_PATH);
	std::wstring p = path;
	size_t dot = p.find_last_of(L'.');
	size_t slash = p.find_last_of(L"\\/");
	std::wstring base = (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash)) ? p.substr(0, dot) : p;
	c.iniPath = base + L".ini";
	c.logPath = base + L".log";
	std::wstring dir = (slash != std::wstring::npos) ? p.substr(0, slash) : L".";

	c.host = Narrow(ReadIniString(L"Daemon", L"Host", L"127.0.0.1", c.iniPath));
	c.port = (int)GetPrivateProfileIntW(L"Daemon", L"Port", 47188, c.iniPath.c_str());
	c.spawn = GetPrivateProfileIntW(L"Daemon", L"Spawn", 0, c.iniPath.c_str()) != 0;
	c.command = ReadIniString(L"Daemon", L"Command", L"", c.iniPath);
	c.workDir = ReadIniString(L"Daemon", L"WorkDir", dir.c_str(), c.iniPath);
	c.connectTimeoutMs = (DWORD)GetPrivateProfileIntW(L"Daemon", L"ConnectTimeoutMs", 15000, c.iniPath.c_str());
	c.quitOnClose = GetPrivateProfileIntW(L"Daemon", L"QuitOnClose", 1, c.iniPath.c_str()) != 0;
	c.log = GetPrivateProfileIntW(L"Debug", L"Log", 0, c.iniPath.c_str()) != 0;
	c.tunerName = ReadIniString(L"Tuner", L"Name", L"ISDB-T SDR", c.iniPath);
	c.dllDir = dir;

	c.sdr.enable = GetPrivateProfileIntW(L"SDR", L"Enable", 0, c.iniPath.c_str()) != 0;
	c.sdr.uri = Narrow(ReadIniString(L"SDR", L"Uri", L"usb:", c.iniPath));
	c.sdr.fs = ReadIniDouble(L"SDR", L"Fs", 0.0, c.iniPath);
	c.sdr.bw = ReadIniDouble(L"SDR", L"Bw", 0.0, c.iniPath);
	std::wstring gain = ReadIniString(L"SDR", L"Gain", L"", c.iniPath);
	if (!gain.empty()) {            // 空なら slow_attack AGC
		c.sdr.agc = false;
		c.sdr.gainDb = ReadIniDouble(L"SDR", L"Gain", 0.0, c.iniPath);
	}
	c.sdr.mode = (int)GetPrivateProfileIntW(L"SDR", L"Mode", 3, c.iniPath.c_str());
	c.sdr.gi = Narrow(ReadIniString(L"SDR", L"GI", L"1/8", c.iniPath));
	c.sdr.winFrames = (int)GetPrivateProfileIntW(L"SDR", L"WinFrames", 16, c.iniPath.c_str());
	c.sdr.ctxFrames = (int)GetPrivateProfileIntW(L"SDR", L"CtxFrames", 4, c.iniPath.c_str());
	c.sdr.workers = (int)GetPrivateProfileIntW(L"SDR", L"Workers", 2, c.iniPath.c_str());
	c.sdr.libiioDir = ReadIniString(L"SDR", L"LibiioDir", L"", c.iniPath);
	return c;
}

// 相対パスを DLL のフォルダ基準で絶対化する（ini の LibiioDir 用）
std::wstring ResolveDir(const std::wstring &dir, const std::wstring &base)
{
	if (dir.empty()) return dir;
	const bool absolute = (dir.size() >= 2 && dir[1] == L':') || dir[0] == L'\\' || dir[0] == L'/';
	return (absolute || base.empty()) ? dir : base + L"\\" + dir;
}

#ifdef ISDBT_WITH_LIBIIO
// libiio1.dll は /DELAYLOAD なので、最初の libiio 呼び出しまでに見つかればよい。
// ここで明示的に探してロードしておく（遅延ロードの失敗は SEH 例外になり握りつぶせないため、
// 先にロード可否を確認して分かりやすいエラーにする）。
// 探す順: ini の LibiioDir（相対は DLL 基準）→ DLL と同じフォルダ → 既定の検索パス
// （ADI インストーラの PATH 等）。
bool PreloadLibiio(const Config &c, std::string &where)
{
	if (GetModuleHandleW(L"libiio1.dll")) { where = "(already loaded)"; return true; }
	std::vector<std::wstring> dirs;
	if (!c.sdr.libiioDir.empty()) dirs.push_back(ResolveDir(c.sdr.libiioDir, c.dllDir));
	if (!c.dllDir.empty()) dirs.push_back(c.dllDir);
	for (const std::wstring &d : dirs) {
		std::wstring p = d + L"\\libiio1.dll";
		// ALTERED_SEARCH_PATH: libusb-1.0.dll などの依存 DLL も同じフォルダから解決させる
		if (LoadLibraryExW(p.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) {
			where = Narrow(p);
			return true;
		}
	}
	if (LoadLibraryW(L"libiio1.dll")) { where = "(search path)"; return true; }
	return false;
}
#endif

// ---- 最小限の JSON 読み取り（応答は 1 階層・既知キーだけ見る） ----
const char *FindValue(const std::string &json, const char *key)
{
	std::string pat = std::string("\"") + key + "\"";
	size_t pos = json.find(pat);
	if (pos == std::string::npos) return nullptr;
	pos = json.find(':', pos + pat.size());
	if (pos == std::string::npos) return nullptr;
	++pos;
	while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
	return json.c_str() + pos;
}

bool JsonBool(const std::string &json, const char *key, bool def = false)
{
	const char *v = FindValue(json, key);
	if (!v) return def;
	if (std::strncmp(v, "true", 4) == 0) return true;
	if (std::strncmp(v, "false", 5) == 0) return false;
	return def;
}

bool JsonNumber(const std::string &json, const char *key, double &out)
{
	const char *v = FindValue(json, key);
	if (!v) return false;
	char *end = nullptr;
	double d = std::strtod(v, &end);
	if (end == v) return false;       // null など
	out = d;
	return true;
}

// ---- ソケット ----
SOCKET ConnectTcp(const std::string &host, int port, DWORD timeoutMs)
{
	addrinfo hints = {};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	addrinfo *res = nullptr;
	if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) return INVALID_SOCKET;
	SOCKET s = INVALID_SOCKET;
	for (addrinfo *ai = res; ai; ai = ai->ai_next) {
		s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (s == INVALID_SOCKET) continue;
		u_long nb = 1;
		ioctlsocket(s, FIONBIO, &nb);
		int r = connect(s, ai->ai_addr, (int)ai->ai_addrlen);
		bool ok = false;
		if (r == 0) {
			ok = true;
		} else if (WSAGetLastError() == WSAEWOULDBLOCK) {
			fd_set wf, ef;
			FD_ZERO(&wf); FD_ZERO(&ef);
			FD_SET(s, &wf); FD_SET(s, &ef);
			timeval tv = { (long)(timeoutMs / 1000), (long)((timeoutMs % 1000) * 1000) };
			if (select(0, nullptr, &wf, &ef, &tv) > 0 && FD_ISSET(s, &wf)) {
				int err = 0, len = sizeof(err);
				getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
				ok = (err == 0);
			}
		}
		if (ok) {
			nb = 0;
			ioctlsocket(s, FIONBIO, &nb);
			break;
		}
		closesocket(s);
		s = INVALID_SOCKET;
	}
	freeaddrinfo(res);
	return s;
}

bool SendAll(SOCKET s, const char *p, size_t n)
{
	while (n > 0) {
		int r = send(s, p, (int)n, 0);
		if (r <= 0) return false;
		p += r;
		n -= (size_t)r;
	}
	return true;
}

// 1 行受信。制御応答は短く 1 要求 1 応答なので 1 バイトずつ読む。
bool RecvLine(SOCKET s, std::string &line)
{
	line.clear();
	char c;
	while (line.size() < 65536) {
		int r = recv(s, &c, 1, 0);
		if (r <= 0) return false;
		if (c == '\n') return true;
		line.push_back(c);
	}
	return false;
}

// ---------------------------------------------------------------------------
class CBonDriverPlutoSDR : public IBonDriver2
{
public:
	CBonDriverPlutoSDR();
	virtual ~CBonDriverPlutoSDR();

	// IBonDriver
	const BOOL OpenTuner(void) override;
	void CloseTuner(void) override;
	const BOOL SetChannel(const BYTE bCh) override;
	const float GetSignalLevel(void) override;
	const DWORD WaitTsStream(const DWORD dwTimeOut) override;
	const DWORD GetReadyCount(void) override;
	const BOOL GetTsStream(BYTE *pDst, DWORD *pdwSize, DWORD *pdwRemain) override;
	const BOOL GetTsStream(BYTE **ppDst, DWORD *pdwSize, DWORD *pdwRemain) override;
	void PurgeTsStream(void) override;
	void Release(void) override;

	// IBonDriver2
	LPCTSTR GetTunerName(void) override;
	const BOOL IsTunerOpening(void) override;
	LPCTSTR EnumTuningSpace(const DWORD dwSpace) override;
	LPCTSTR EnumChannelName(const DWORD dwSpace, const DWORD dwChannel) override;
	const BOOL SetChannel(const DWORD dwSpace, const DWORD dwChannel) override;
	const DWORD GetCurSpace(void) override;
	const DWORD GetCurChannel(void) override;

private:
	void Log(const char *fmt, ...);
	bool Control(const std::string &request, std::string &response);
	SOCKET ConnectWithRetry(DWORD timeoutMs);
	bool StartDaemon();
	void StopDaemon();
	bool OpenStream();
	void ReaderThread();
	void PushChunk(std::vector<BYTE> &&chunk);
	BOOL OpenTunerSdr();
	void PullerThread();

	Config m_cfg;
	bool m_wsa = false;
	bool m_open = false;

	std::mutex m_ctrlMutex;
	SOCKET m_ctrl = INVALID_SOCKET;
	SOCKET m_stream = INVALID_SOCKET;
	std::thread m_reader;
	std::atomic<bool> m_stop{false};

	std::mutex m_qMutex;
	std::deque<std::vector<BYTE>> m_queue;
	size_t m_queueBytes = 0;
	size_t m_droppedBytes = 0;
	std::vector<BYTE> m_current;      // 直近に GetTsStream で返したバッファ（次の呼び出しまで有効）
	HANDLE m_event = nullptr;         // 手動リセット。キューにデータがある間シグナル

	PROCESS_INFORMATION m_proc = {};
	bool m_spawned = false;

	// スタンドアロンモード（[SDR] Enable=1）。非 null なら m_ctrl / m_stream は使わない
	isdbt::LiveDemod *m_live = nullptr;
	std::shared_ptr<isdbt::ClientQueue> m_liveQ;

	DWORD m_curSpace = 0xFFFFFFFF;
	DWORD m_curChannel = 0xFFFFFFFF;
	float m_level = 0.0f;
	ULONGLONG m_levelTick = 0;
	wchar_t m_chName[16] = {};
	FILE *m_logFile = nullptr;
};

CBonDriverPlutoSDR *g_instance = nullptr;

CBonDriverPlutoSDR::CBonDriverPlutoSDR()
{
	WSADATA wsa;
	m_wsa = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
	m_cfg = LoadConfig();
	m_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (m_cfg.log) m_logFile = _wfopen(m_cfg.logPath.c_str(), L"a");
	Log("loaded; ini=%s host=%s port=%d spawn=%d", Narrow(m_cfg.iniPath).c_str(), m_cfg.host.c_str(), m_cfg.port, (int)m_cfg.spawn);
}

CBonDriverPlutoSDR::~CBonDriverPlutoSDR()
{
	CloseTuner();
	if (m_event) CloseHandle(m_event);
	if (m_logFile) fclose(m_logFile);
	if (m_wsa) WSACleanup();
}

void CBonDriverPlutoSDR::Log(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	std::string line = std::string("[BonDriver_PlutoSDR] ") + buf + "\n";
	OutputDebugStringA(line.c_str());
	if (m_logFile) {
		SYSTEMTIME st;
		GetLocalTime(&st);
		fprintf(m_logFile, "%02d:%02d:%02d.%03d %s", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line.c_str());
		fflush(m_logFile);
	}
}

// ---- デーモン接続 ----
SOCKET CBonDriverPlutoSDR::ConnectWithRetry(DWORD timeoutMs)
{
	ULONGLONG deadline = GetTickCount64() + timeoutMs;
	for (;;) {
		SOCKET s = ConnectTcp(m_cfg.host, m_cfg.port, 1000);
		if (s != INVALID_SOCKET) return s;
		if (GetTickCount64() >= deadline) return INVALID_SOCKET;
		if (m_spawned) {
			// 起動したデーモンが落ちていたら待っても無駄
			if (WaitForSingleObject(m_proc.hProcess, 0) == WAIT_OBJECT_0) {
				Log("daemon process exited before accepting connections");
				return INVALID_SOCKET;
			}
		}
		Sleep(200);
	}
}

bool CBonDriverPlutoSDR::StartDaemon()
{
	if (m_cfg.command.empty()) {
		Log("Spawn=1 but [Daemon] Command is empty");
		return false;
	}
	std::wstring cmd = m_cfg.command;   // CreateProcess は書き込み可能なバッファを要求する
	STARTUPINFOW si = {};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi = {};
	const wchar_t *wd = m_cfg.workDir.empty() ? nullptr : m_cfg.workDir.c_str();
	if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, wd, &si, &pi)) {
		Log("CreateProcess failed (%lu): %s", GetLastError(), Narrow(m_cfg.command).c_str());
		return false;
	}
	CloseHandle(pi.hThread);
	pi.hThread = nullptr;
	m_proc = pi;
	m_spawned = true;
	Log("spawned daemon pid %lu", pi.dwProcessId);
	return true;
}

void CBonDriverPlutoSDR::StopDaemon()
{
	if (!m_spawned) return;
	if (m_cfg.quitOnClose) {
		std::string resp;
		Control("{\"cmd\":\"quit\"}", resp);
		if (WaitForSingleObject(m_proc.hProcess, 3000) != WAIT_OBJECT_0) {
			Log("daemon did not exit after quit; terminating");
			TerminateProcess(m_proc.hProcess, 1);
		}
	}
	CloseHandle(m_proc.hProcess);
	m_proc = {};
	m_spawned = false;
}

bool CBonDriverPlutoSDR::Control(const std::string &request, std::string &response)
{
	std::lock_guard<std::mutex> lk(m_ctrlMutex);
	if (m_ctrl == INVALID_SOCKET) {
		if (!m_open) return false;
		m_ctrl = ConnectTcp(m_cfg.host, m_cfg.port, 1000);   // 切れていたら 1 回だけ張り直す
		if (m_ctrl == INVALID_SOCKET) return false;
		DWORD to = kControlTimeoutMs;
		setsockopt(m_ctrl, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof(to));
	}
	std::string line = request + "\n";
	if (!SendAll(m_ctrl, line.data(), line.size()) || !RecvLine(m_ctrl, response)) {
		Log("control I/O failed for %s", request.c_str());
		closesocket(m_ctrl);
		m_ctrl = INVALID_SOCKET;
		return false;
	}
	return true;
}

bool CBonDriverPlutoSDR::OpenStream()
{
	m_stream = ConnectTcp(m_cfg.host, m_cfg.port, 2000);
	if (m_stream == INVALID_SOCKET) return false;
	int rcvbuf = 1 << 20;
	setsockopt(m_stream, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof(rcvbuf));
	DWORD to = kControlTimeoutMs;
	setsockopt(m_stream, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof(to));
	const char req[] = "{\"cmd\":\"stream\"}\n";
	std::string resp;
	if (!SendAll(m_stream, req, sizeof(req) - 1) || !RecvLine(m_stream, resp) || !JsonBool(resp, "ok")) {
		Log("stream request rejected: %s", resp.c_str());
		closesocket(m_stream);
		m_stream = INVALID_SOCKET;
		return false;
	}
	to = 0;   // 以降は受信スレッドがブロック受信する（closesocket で起こす）
	setsockopt(m_stream, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof(to));
	return true;
}

void CBonDriverPlutoSDR::PushChunk(std::vector<BYTE> &&chunk)
{
	std::lock_guard<std::mutex> lk(m_qMutex);
	while (!m_queue.empty() && m_queueBytes + chunk.size() > kMaxQueueBytes) {
		m_queueBytes -= m_queue.front().size();
		m_droppedBytes += m_queue.front().size();
		m_queue.pop_front();
		if ((m_droppedBytes / kChunkBytes) % 64 == 1) Log("receive queue full; dropped %zu bytes so far", m_droppedBytes);
	}
	m_queueBytes += chunk.size();
	m_queue.push_back(std::move(chunk));
	SetEvent(m_event);
}

void CBonDriverPlutoSDR::ReaderThread()
{
	std::vector<BYTE> staging;
	staging.reserve(kChunkBytes * 2);
	std::vector<BYTE> buf(65536);
	while (!m_stop) {
		int n = recv(m_stream, (char *)buf.data(), (int)buf.size(), 0);
		if (n <= 0) {
			if (!m_stop) Log("stream closed by daemon (recv=%d err=%d)", n, WSAGetLastError());
			break;
		}
		staging.insert(staging.end(), buf.begin(), buf.begin() + n);
		size_t whole = (staging.size() / kTsPacket) * kTsPacket;
		if (whole < kMinPushBytes) continue;
		size_t off = 0;
		while (off < whole) {
			size_t len = (whole - off > kChunkBytes) ? kChunkBytes : (whole - off);
			PushChunk(std::vector<BYTE>(staging.begin() + off, staging.begin() + off + len));
			off += len;
		}
		staging.erase(staging.begin(), staging.begin() + whole);
	}
}

// ---- スタンドアロンモード（[SDR] Enable=1） ----
BOOL CBonDriverPlutoSDR::OpenTunerSdr()
{
#ifdef ISDBT_WITH_LIBIIO
	std::string where;
	if (!PreloadLibiio(m_cfg, where)) {
		Log("libiio1.dll not found: ADI の libiio インストーラを入れるか、"
		    "libiio の DLL 一式を DLL の隣か [SDR] LibiioDir に置く");
		return FALSE;
	}
	Log("libiio loaded from %s", where.c_str());
#endif
	try {
		isdbt::SdrConfig sc;            // fs / bw の既定値（USB 2.0 向け 7/8 レート等）はここから
		sc.uri = m_cfg.sdr.uri;
		if (m_cfg.sdr.fs > 0) sc.fs = m_cfg.sdr.fs;
		if (m_cfg.sdr.bw > 0) sc.bw = m_cfg.sdr.bw;
		sc.agc = m_cfg.sdr.agc;
		sc.gain_db = m_cfg.sdr.gainDb;
		sc.freq_hz = isdbt::ChannelToHz(27);    // SetChannel までの仮周波数（isdbtd --sdr と同じ）
		isdbt::Gi gi{8};
		if (!isdbt::ParseGi(m_cfg.sdr.gi, &gi)) {
			Log("bad [SDR] GI: %s", m_cfg.sdr.gi.c_str());
			return FALSE;
		}
		m_live = new isdbt::LiveDemod(sc, (isdbt::Mode)m_cfg.sdr.mode, gi,
		                              m_cfg.sdr.winFrames, m_cfg.sdr.ctxFrames, m_cfg.sdr.workers,
		                              false);
		m_live->Start();
	} catch (const std::exception &e) {
		Log("SDR open failed: %s", e.what());
		if (m_live) {
			m_live->Stop();
			delete m_live;
			m_live = nullptr;
		}
		return FALSE;
	}
	m_liveQ = m_live->AddClient();
	m_stop = false;
	m_droppedBytes = 0;
	ResetEvent(m_event);
	m_reader = std::thread(&CBonDriverPlutoSDR::PullerThread, this);
	m_open = true;
	m_levelTick = 0;
	Log("SDR standalone open: uri=%s mode=%d gi=%s win=%d+%d workers=%d",
	    m_cfg.sdr.uri.c_str(), m_cfg.sdr.mode, m_cfg.sdr.gi.c_str(),
	    m_cfg.sdr.ctxFrames, m_cfg.sdr.winFrames, m_cfg.sdr.workers);
	return TRUE;
}

// LiveDemod は 1 OFDM フレーム（~0.5 MB）単位で出すので、GetTsStream 向けに
// kChunkBytes 以下（188 の倍数）へ割って受信キューに積む。
// [Debug] Log=1 のときは 10 秒ごとに復調統計も書く（ドロップ原因の切り分け用:
// rs_uncorrectable が伸びる = C/N 不足、underruns / frames_lost / windows_failed が
// 伸びる = 取り込み・処理落ち、dropped = クライアント側の滞留）。
void CBonDriverPlutoSDR::PullerThread()
{
	ULONGLONG statTick = GetTickCount64();
	while (!m_stop) {
		if (m_logFile) {
			ULONGLONG now = GetTickCount64();
			if (now - statTick >= 10000) {
				statTick = now;
				Log("live: %s", m_live->StatusJson().c_str());
			}
		}
		std::vector<BYTE> chunk;
		{
			std::unique_lock<std::mutex> lk(m_liveQ->mu);
			if (!m_liveQ->cv.wait_for(lk, std::chrono::milliseconds(200),
			                          [&] { return !m_liveQ->chunks.empty(); })) {
				continue;
			}
			chunk = std::move(m_liveQ->chunks.front());
			m_liveQ->chunks.pop_front();
		}
		size_t off = 0;
		while (off < chunk.size()) {
			size_t len = (chunk.size() - off > kChunkBytes) ? kChunkBytes : (chunk.size() - off);
			PushChunk(std::vector<BYTE>(chunk.begin() + off, chunk.begin() + off + len));
			off += len;
		}
	}
}

// ---- IBonDriver ----
const BOOL CBonDriverPlutoSDR::OpenTuner(void)
{
	if (m_open) return TRUE;
	if (!m_wsa || !m_event) return FALSE;
	m_cfg = LoadConfig();   // TVTest を開いたまま ini を直せるように毎回読む
	if (m_cfg.sdr.enable) return OpenTunerSdr();

	std::lock_guard<std::mutex> lk(m_ctrlMutex);
	m_ctrl = ConnectTcp(m_cfg.host, m_cfg.port, 1000);
	if (m_ctrl == INVALID_SOCKET && m_cfg.spawn) {
		if (StartDaemon()) m_ctrl = ConnectWithRetry(m_cfg.connectTimeoutMs);
	}
	if (m_ctrl == INVALID_SOCKET) {
		Log("cannot connect to daemon at %s:%d", m_cfg.host.c_str(), m_cfg.port);
		if (m_spawned) {
			TerminateProcess(m_proc.hProcess, 1);
			CloseHandle(m_proc.hProcess);
			m_proc = {};
			m_spawned = false;
		}
		return FALSE;
	}
	DWORD to = kControlTimeoutMs;
	setsockopt(m_ctrl, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof(to));

	std::string hello = "{\"cmd\":\"hello\"}\n", resp;
	if (!SendAll(m_ctrl, hello.data(), hello.size()) || !RecvLine(m_ctrl, resp) || !JsonBool(resp, "ok")) {
		Log("hello failed: %s", resp.c_str());
		closesocket(m_ctrl);
		m_ctrl = INVALID_SOCKET;
		return FALSE;
	}
	Log("hello: %s", resp.c_str());

	if (!OpenStream()) {
		closesocket(m_ctrl);
		m_ctrl = INVALID_SOCKET;
		return FALSE;
	}
	m_stop = false;
	m_droppedBytes = 0;
	ResetEvent(m_event);
	m_reader = std::thread(&CBonDriverPlutoSDR::ReaderThread, this);
	m_open = true;
	m_levelTick = 0;
	return TRUE;
}

void CBonDriverPlutoSDR::CloseTuner(void)
{
	if (!m_open && !m_spawned) return;
	m_stop = true;
	if (m_live) {
		if (m_reader.joinable()) m_reader.join();
		m_live->RemoveClient(m_liveQ);
		m_liveQ.reset();
		m_live->Stop();
		delete m_live;
		m_live = nullptr;
		PurgeTsStream();
		m_open = false;
		m_curSpace = m_curChannel = 0xFFFFFFFF;
		m_level = 0.0f;
		Log("closed (standalone)");
		return;
	}
	if (m_stream != INVALID_SOCKET) {
		shutdown(m_stream, SD_BOTH);
		closesocket(m_stream);
		m_stream = INVALID_SOCKET;
	}
	if (m_reader.joinable()) m_reader.join();
	StopDaemon();
	{
		std::lock_guard<std::mutex> lk(m_ctrlMutex);
		if (m_ctrl != INVALID_SOCKET) {
			closesocket(m_ctrl);
			m_ctrl = INVALID_SOCKET;
		}
	}
	PurgeTsStream();
	m_open = false;
	m_curSpace = m_curChannel = 0xFFFFFFFF;
	m_level = 0.0f;
	Log("closed");
}

const BOOL CBonDriverPlutoSDR::SetChannel(const BYTE bCh)
{
	return SetChannel((DWORD)0, (DWORD)bCh);
}

const float CBonDriverPlutoSDR::GetSignalLevel(void)
{
	if (!m_open) return 0.0f;
	if (m_live) return (float)m_live->signal_db();   // 最悪階層の MER [dB]（未ロック時 0）
	ULONGLONG now = GetTickCount64();
	if (m_levelTick == 0 || now - m_levelTick >= kSignalCacheMs) {
		m_levelTick = now;
		std::string resp;
		double v = 0.0;
		if (Control("{\"cmd\":\"status\"}", resp) && JsonBool(resp, "ok") && JsonNumber(resp, "signal_db", v)) {
			m_level = (float)v;
		} else {
			m_level = 0.0f;
		}
	}
	return m_level;
}

const DWORD CBonDriverPlutoSDR::WaitTsStream(const DWORD dwTimeOut)
{
	if (!m_open) return WAIT_ABANDONED;
	return WaitForSingleObject(m_event, dwTimeOut);
}

const DWORD CBonDriverPlutoSDR::GetReadyCount(void)
{
	std::lock_guard<std::mutex> lk(m_qMutex);
	return (DWORD)m_queue.size();
}

const BOOL CBonDriverPlutoSDR::GetTsStream(BYTE *pDst, DWORD *pdwSize, DWORD *pdwRemain)
{
	BYTE *src = nullptr;
	if (!GetTsStream(&src, pdwSize, pdwRemain)) return FALSE;
	if (pDst && src && *pdwSize) std::memcpy(pDst, src, *pdwSize);
	return TRUE;
}

const BOOL CBonDriverPlutoSDR::GetTsStream(BYTE **ppDst, DWORD *pdwSize, DWORD *pdwRemain)
{
	std::lock_guard<std::mutex> lk(m_qMutex);
	if (m_queue.empty()) {
		if (pdwSize) *pdwSize = 0;
		if (pdwRemain) *pdwRemain = 0;
		ResetEvent(m_event);
		return FALSE;
	}
	m_current.swap(m_queue.front());
	m_queue.pop_front();
	m_queueBytes -= m_current.size();
	if (ppDst) *ppDst = m_current.data();
	if (pdwSize) *pdwSize = (DWORD)m_current.size();
	if (pdwRemain) *pdwRemain = (DWORD)m_queue.size();
	if (m_queue.empty()) ResetEvent(m_event);
	return TRUE;
}

void CBonDriverPlutoSDR::PurgeTsStream(void)
{
	std::lock_guard<std::mutex> lk(m_qMutex);
	m_queue.clear();
	m_queueBytes = 0;
	ResetEvent(m_event);
}

void CBonDriverPlutoSDR::Release(void)
{
	if (g_instance == this) g_instance = nullptr;
	delete this;
}

// ---- IBonDriver2 ----
LPCTSTR CBonDriverPlutoSDR::GetTunerName(void)
{
	return m_cfg.tunerName.c_str();
}

const BOOL CBonDriverPlutoSDR::IsTunerOpening(void)
{
	return m_open ? TRUE : FALSE;
}

LPCTSTR CBonDriverPlutoSDR::EnumTuningSpace(const DWORD dwSpace)
{
	return dwSpace == 0 ? L"UHF" : nullptr;
}

LPCTSTR CBonDriverPlutoSDR::EnumChannelName(const DWORD dwSpace, const DWORD dwChannel)
{
	if (dwSpace != 0 || dwChannel >= kNumChannels) return nullptr;
	swprintf_s(m_chName, L"%uch", (unsigned)(kFirstChannel + dwChannel));
	return m_chName;
}

const BOOL CBonDriverPlutoSDR::SetChannel(const DWORD dwSpace, const DWORD dwChannel)
{
	if (!m_open) return FALSE;
	if (dwSpace != 0 || dwChannel >= kNumChannels) return FALSE;
	DWORD phys = kFirstChannel + dwChannel;
	if (m_live) {
		m_live->Tune(true, (int)phys, isdbt::ChannelToHz((int)phys));
		Log("tune %luch (standalone)", phys);
		m_curSpace = dwSpace;
		m_curChannel = dwChannel;
		PurgeTsStream();
		m_levelTick = 0;
		return TRUE;
	}
	std::string req = "{\"cmd\":\"tune\",\"channel\":" + std::to_string(phys) + "}";
	std::string resp;
	if (!Control(req, resp) || !JsonBool(resp, "ok")) {
		Log("tune %luch failed: %s", phys, resp.c_str());
		return FALSE;
	}
	Log("tune %luch: %s", phys, resp.c_str());
	m_curSpace = dwSpace;
	m_curChannel = dwChannel;
	PurgeTsStream();
	m_levelTick = 0;
	return TRUE;
}

const DWORD CBonDriverPlutoSDR::GetCurSpace(void)
{
	return m_curSpace;
}

const DWORD CBonDriverPlutoSDR::GetCurChannel(void)
{
	return m_curChannel;
}

} // namespace

// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport) IBonDriver *CreateBonDriver()
{
	if (!g_instance) g_instance = new CBonDriverPlutoSDR();
	return g_instance;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID)
{
	if (fdwReason == DLL_PROCESS_ATTACH) {
		g_hModule = hinstDLL;
		DisableThreadLibraryCalls(hinstDLL);
	}
	return TRUE;
}
