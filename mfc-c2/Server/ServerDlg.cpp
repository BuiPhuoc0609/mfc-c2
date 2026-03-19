#include "pch.h"
#include "framework.h"
#include "Server.h"
#include "ServerDlg.h"
#include "afxdialogex.h"
#include <string>
#include <memory>
#include <vector>
#include <tlhelp32.h>
#include <algorithm>
#include <map>

#pragma comment(lib, "Ws2_32.lib")

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

namespace
{
	constexpr UINT WM_APP_WORKER_LOG = WM_APP + 2;
	constexpr UINT kServerPort = 5050;

	enum class FrameType : unsigned short
	{
		HELLO = 1,
		EXEC_REQ = 2,
		LIST_PROC_REQ = 3,
		GET_FILE_REQ = 4,
		PUT_FILE_REQ = 5,

		EXEC_RESP = 100,
		LIST_PROC_RESP = 101,
		GET_FILE_RESP = 102,
		PUT_FILE_RESP = 103,
		ERROR_RESP = 199,
	};

	struct ClientSessionPayload
	{
		CServerDlg* pDlg;
		SOCKET socket;
		CString remoteAddress;
	};

	struct ServerEvent
	{
		CString clientId;
		CString logLine;
		CString lastResponse;
		CString processSnapshot;
		CString execOutput;
	};

	CString TrimCopy(CString value)
	{
		value.Trim();
		return value;
	}

	CString CStringFromUtf8(const std::string& value)
	{
		if (value.empty())
		{
			return CString();
		}

		int count = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
		CString result;
		LPWSTR buffer = result.GetBuffer(count);
		MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), buffer, count);
		result.ReleaseBuffer(count);
		return result;
	}

	std::string Utf8FromCString(const CString& value)
	{
#ifdef UNICODE
		int count = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
		std::string result(static_cast<size_t>(count > 0 ? count - 1 : 0), '\0');
		if (count > 1)
		{
			WideCharToMultiByte(CP_UTF8, 0, value, -1, &result[0], count - 1, nullptr, nullptr);
		}
		return result;
#else
		return std::string(value.GetString());
#endif
	}

	bool SendAll(SOCKET socketHandle, const void* data, int size)
	{
		int total = 0;
		const char* bytes = static_cast<const char*>(data);
		while (total < size)
		{
			int sent = send(socketHandle, bytes + total, size - total, 0);
			if (sent == SOCKET_ERROR)
			{
				return false;
			}
			total += sent;
		}
		return true;
	}

	bool SendAll(SOCKET socketHandle, const std::string& payload)
	{
		return SendAll(socketHandle, payload.data(), static_cast<int>(payload.size()));
	}

	struct Frame
	{
		FrameType type;
		unsigned int requestId;
		std::vector<char> payload;
	};

	bool RecvExact(SOCKET socketHandle, void* outData, int size)
	{
		char* bytes = static_cast<char*>(outData);
		int total = 0;
		while (total < size)
		{
			int got = recv(socketHandle, bytes + total, size - total, 0);
			if (got <= 0)
			{
				return false;
			}
			total += got;
		}
		return true;
	}

	bool RecvFrame(SOCKET socketHandle, Frame& outFrame)
	{
		unsigned int payloadLen = 0;
		unsigned short type = 0;
		unsigned int requestId = 0;
		if (!RecvExact(socketHandle, &payloadLen, sizeof(payloadLen)))
		{
			return false;
		}
		if (!RecvExact(socketHandle, &type, sizeof(type)))
		{
			return false;
		}
		if (!RecvExact(socketHandle, &requestId, sizeof(requestId)))
		{
			return false;
		}
		if (payloadLen > (60U * 1024U * 1024U))
		{
			return false;
		}
		outFrame.type = static_cast<FrameType>(type);
		outFrame.requestId = requestId;
		outFrame.payload.assign(payloadLen, 0);
		if (payloadLen > 0 && !RecvExact(socketHandle, outFrame.payload.data(), static_cast<int>(payloadLen)))
		{
			return false;
		}
		return true;
	}

	bool SendFrame(SOCKET socketHandle, FrameType type, unsigned int requestId, const void* payload, unsigned int payloadLen)
	{
		unsigned short typeValue = static_cast<unsigned short>(type);
		if (!SendAll(socketHandle, &payloadLen, sizeof(payloadLen)))
		{
			return false;
		}
		if (!SendAll(socketHandle, &typeValue, sizeof(typeValue)))
		{
			return false;
		}
		if (!SendAll(socketHandle, &requestId, sizeof(requestId)))
		{
			return false;
		}
		if (payloadLen > 0)
		{
			return SendAll(socketHandle, payload, static_cast<int>(payloadLen));
		}
		return true;
	}

	CString MakePeerLabel(const sockaddr_in& address)
	{
		wchar_t host[NI_MAXHOST] = {};
		InetNtop(AF_INET, const_cast<IN_ADDR*>(&address.sin_addr), host, NI_MAXHOST);
		CString label;
		label.Format(_T("%s:%u"), host, ntohs(address.sin_port));
		return label;
	}

	std::wstring TrimCopyW(std::wstring value)
	{
		auto notSpace = [](wchar_t ch)
			{
				return ch != L' ' && ch != L'\t' && ch != L'\r' && ch != L'\n';
			};
		value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
		value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
		return value;
	}

	std::wstring WideFromUtf8Bytes(const std::vector<char>& bytes)
	{
		if (bytes.empty())
		{
			return {};
		}
		int count = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
		std::wstring result(static_cast<size_t>(count), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), &result[0], count);
		return result;
	}

	std::wstring GetHeaderValueW(const std::wstring& header, const std::wstring& key)
	{
		std::wstring needle = key + L"=";
		size_t pos = header.find(needle);
		if (pos == std::wstring::npos)
		{
			return {};
		}
		size_t start = pos + needle.size();
		size_t end = header.find(L'\n', start);
		std::wstring value = (end == std::wstring::npos) ? header.substr(start) : header.substr(start, end - start);
		return TrimCopyW(value);
	}

	CString EnumerateServerProcesses()
	{
		HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snapshot == INVALID_HANDLE_VALUE)
		{
			return _T("snapshot_failed");
		}

		PROCESSENTRY32 entry = {};
		entry.dwSize = sizeof(entry);
		CString lines;
		if (Process32First(snapshot, &entry))
		{
			do
			{
				CString line;
				line.Format(_T("%lu %s\r\n"), entry.th32ProcessID, entry.szExeFile);
				lines += line;
			} while (Process32Next(snapshot, &entry));
		}
		CloseHandle(snapshot);
		lines.TrimRight();
		return lines.IsEmpty() ? _T("no_processes") : lines;
	}

	bool ExecCommandCapture(const CString& command, CString& outStdout, CString& outError)
	{
		outStdout.Empty();
		outError.Empty();

		SECURITY_ATTRIBUTES sa = {};
		sa.nLength = sizeof(sa);
		sa.bInheritHandle = TRUE;

		HANDLE readPipe = nullptr;
		HANDLE writePipe = nullptr;
		if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
		{
			outError = _T("CreatePipe failed.");
			return false;
		}
		SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

		CString cmdLine = _T("cmd.exe /c ") + command;
		STARTUPINFO si = {};
		si.cb = sizeof(si);
		si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
		si.hStdOutput = writePipe;
		si.hStdError = writePipe;
		si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
		si.wShowWindow = SW_HIDE;
		PROCESS_INFORMATION pi = {};

		std::wstring cmdLineW(cmdLine.GetString());
		BOOL ok = CreateProcessW(
			nullptr,
			&cmdLineW[0],
			nullptr,
			nullptr,
			TRUE,
			CREATE_NO_WINDOW,
			nullptr,
			nullptr,
			&si,
			&pi);

		CloseHandle(writePipe);
		writePipe = nullptr;

		if (!ok)
		{
			CloseHandle(readPipe);
			outError.Format(_T("CreateProcess failed (error %lu)."), GetLastError());
			return false;
		}

		std::string buffer;
		char tmp[4096];
		DWORD read = 0;
		while (ReadFile(readPipe, tmp, sizeof(tmp), &read, nullptr) && read > 0)
		{
			buffer.append(tmp, tmp + read);
			if (buffer.size() > 1024 * 1024)
			{
				break;
			}
		}
		CloseHandle(readPipe);
		readPipe = nullptr;

		WaitForSingleObject(pi.hProcess, 5000);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);

		outStdout = CStringFromUtf8(buffer);
		outStdout.TrimRight();
		return true;
	}

	struct BinaryResponse
	{
		std::string headerUtf8;
		std::vector<char> body;
	};

	CString HandleRequest(CServerDlg* pDlg, const CString& request, ServerEvent& serverEvent)
	{
		CString normalized = request;
		normalized.Replace(_T("\r"), _T(""));
		normalized.TrimRight();

		if (normalized.CompareNoCase(_T("PING")) == 0)
		{
			serverEvent.logLine = _T("Received PING from client.");
			serverEvent.lastResponse = _T("PING");
			return _T("OK PONG");
		}

		if (normalized.CompareNoCase(_T("STATUS")) == 0)
		{
			serverEvent.logLine = _T("Client requested server status.");
			serverEvent.lastResponse = _T("STATUS");
			CString response;
			response.Format(_T("OK %s | active clients=%ld"), pDlg->GetServerStateText().GetString(), pDlg->GetActiveClientCount());
			return response;
		}

		CString prefix = _T("PROCESS_SNAPSHOT\n");
		if (normalized.Left(prefix.GetLength()).CompareNoCase(prefix) == 0)
		{
			CString processSnapshot = normalized.Mid(prefix.GetLength());
			CString clientId;
			CString clientPrefix = _T("CLIENT_ID=");
			if (processSnapshot.Left(clientPrefix.GetLength()).CompareNoCase(clientPrefix) == 0)
			{
				int firstNewLine = processSnapshot.Find(_T('\n'));
				if (firstNewLine != -1)
				{
					clientId = TrimCopy(processSnapshot.Mid(clientPrefix.GetLength(), firstNewLine - clientPrefix.GetLength()));
					processSnapshot = processSnapshot.Mid(firstNewLine + 1);
				}
			}
			processSnapshot = TrimCopy(processSnapshot);
			if (processSnapshot.IsEmpty())
			{
				return _T("ERR PROCESS_SNAPSHOT payload is empty.");
			}

			if (!clientId.IsEmpty())
			{
				serverEvent.clientId = clientId;
			}
			serverEvent.processSnapshot = processSnapshot;
			serverEvent.logLine = _T("Received process snapshot from client.");
			serverEvent.lastResponse = _T("PROCESS_SNAPSHOT");

			int count = 0;
			int start = 0;
			while (start < processSnapshot.GetLength())
			{
				int end = processSnapshot.Find(_T('\n'), start);
				++count;
				if (end == -1)
				{
					break;
				}
				start = end + 1;
			}

			CString response;
			response.Format(_T("OK Received %d process rows"), count);
			return response;
		}

		prefix = _T("FETCH_MESSAGE\n");
		if (normalized.Left(prefix.GetLength()).CompareNoCase(prefix) == 0)
		{
			CString payload = normalized.Mid(prefix.GetLength());
			CString clientId;
			CString clientPrefix = _T("CLIENT_ID=");
			if (payload.Left(clientPrefix.GetLength()).CompareNoCase(clientPrefix) == 0)
			{
				int firstNewLine = payload.Find(_T('\n'));
				if (firstNewLine != -1)
				{
					clientId = TrimCopy(payload.Mid(clientPrefix.GetLength(), firstNewLine - clientPrefix.GetLength()));
					payload = payload.Mid(firstNewLine + 1);
				}
			}

			if (!clientId.IsEmpty())
			{
				serverEvent.clientId = clientId;
			}

			clientId = TrimCopy(clientId);
			if (clientId.IsEmpty())
			{
				return _T("ERR FETCH_MESSAGE requires CLIENT_ID.");
			}

			serverEvent.logLine = _T("Client fetched pending message.");
			serverEvent.lastResponse = _T("FETCH_MESSAGE");

			CString pendingMessage = pDlg->ConsumePendingMessage(clientId);
			if (pendingMessage.IsEmpty())
			{
				return _T("NO_MESSAGE");
			}
			return _T("MESSAGE\n") + pendingMessage;
		}

		return _T("ERR Unsupported request. Allowed: PING, STATUS, PROCESS_SNAPSHOT, FETCH_MESSAGE");
	}

	BinaryResponse HandleRequestBinary(CServerDlg* pDlg, const std::vector<char>& requestBytes, ServerEvent& serverEvent)
	{
		BinaryResponse response = {};
		if (requestBytes.empty())
		{
			serverEvent.logLine = _T("Client connected but sent no data.");
			response.headerUtf8 = Utf8FromCString(_T("ERR Empty request.\r\n"));
			return response;
		}

		auto findSeparator = [&](const std::string& sep) -> size_t
			{
				if (requestBytes.size() < sep.size())
				{
					return std::string::npos;
				}
				for (size_t i = 0; i + sep.size() <= requestBytes.size(); ++i)
				{
					if (memcmp(requestBytes.data() + i, sep.data(), sep.size()) == 0)
					{
						return i;
					}
				}
				return std::string::npos;
			};

		size_t headerEnd = findSeparator("\r\n\r\n");
		size_t sepLen = 4;
		if (headerEnd == std::string::npos)
		{
			headerEnd = findSeparator("\n\n");
			sepLen = 2;
		}

		std::vector<char> headerBytes;
		std::vector<char> bodyBytes;
		if (headerEnd == std::string::npos)
		{
			headerBytes = requestBytes;
		}
		else
		{
			headerBytes.assign(requestBytes.begin(), requestBytes.begin() + static_cast<long long>(headerEnd));
			bodyBytes.assign(requestBytes.begin() + static_cast<long long>(headerEnd + sepLen), requestBytes.end());
		}

		std::wstring headerW = WideFromUtf8Bytes(headerBytes);
		CString header = headerW.c_str();
		header.Replace(_T("\r"), _T(""));
		header.TrimRight();

		int firstNewLine = header.Find(_T('\n'));
		CString commandLine = (firstNewLine == -1) ? header : header.Left(firstNewLine);
		commandLine.Trim();

		CString upper = commandLine;
		upper.MakeUpper();

		if (upper == _T("PING") || upper == _T("STATUS") || upper == _T("PROCESS_SNAPSHOT") || upper == _T("FETCH_MESSAGE"))
		{
			CString textResponse = HandleRequest(pDlg, header, serverEvent);
			response.headerUtf8 = Utf8FromCString(textResponse + _T("\r\n"));
			return response;
		}

		if (upper == _T("LIST_PROC"))
		{
			serverEvent.logLine = _T("Client requested server process list.");
			serverEvent.lastResponse = _T("LIST_PROC");
			CString lines = EnumerateServerProcesses();
			response.headerUtf8 = Utf8FromCString(_T("OK\n") + lines + _T("\r\n"));
			return response;
		}

		if (upper == _T("EXEC"))
		{
			std::wstring cmdW = GetHeaderValueW(headerW, L"CMD");
			CString cmd = cmdW.c_str();
			cmd.Trim();
			if (cmd.IsEmpty())
			{
				response.headerUtf8 = Utf8FromCString(_T("ERR EXEC requires CMD=<command>\r\n"));
				return response;
			}
			serverEvent.logLine.Format(_T("EXEC requested: %s"), cmd.GetString());
			serverEvent.lastResponse = _T("EXEC");

			CString stdoutText;
			CString errText;
			if (!ExecCommandCapture(cmd, stdoutText, errText))
			{
				response.headerUtf8 = Utf8FromCString(_T("ERR EXEC failed: ") + errText + _T("\r\n"));
				return response;
			}

			if (stdoutText.GetLength() > 20000)
			{
				stdoutText = stdoutText.Left(20000) + _T("\r\n...truncated...");
			}
			response.headerUtf8 = Utf8FromCString(_T("OK\n") + stdoutText + _T("\r\n"));
			return response;
		}

		if (upper == _T("GET_FILE"))
		{
			std::wstring pathW = GetHeaderValueW(headerW, L"PATH");
			if (pathW.empty())
			{
				response.headerUtf8 = Utf8FromCString(_T("ERR GET_FILE requires PATH=<file path>\r\n"));
				return response;
			}

			HANDLE fileHandle = CreateFileW(pathW.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (fileHandle == INVALID_HANDLE_VALUE)
			{
				CString msg;
				msg.Format(_T("ERR GET_FILE cannot open (error %lu)\r\n"), GetLastError());
				response.headerUtf8 = Utf8FromCString(msg);
				return response;
			}

			LARGE_INTEGER fileSize = {};
			if (!GetFileSizeEx(fileHandle, &fileSize) || fileSize.QuadPart < 0 || fileSize.QuadPart >(50LL * 1024 * 1024))
			{
				CloseHandle(fileHandle);
				response.headerUtf8 = Utf8FromCString(_T("ERR GET_FILE unsupported size (max 50MB)\r\n"));
				return response;
			}

			response.body.resize(static_cast<size_t>(fileSize.QuadPart));
			DWORD bytesRead = 0;
			size_t offset = 0;
			while (offset < response.body.size())
			{
				DWORD chunk = static_cast<DWORD>(std::min<size_t>(64 * 1024, response.body.size() - offset));
				if (!ReadFile(fileHandle, response.body.data() + offset, chunk, &bytesRead, nullptr) || bytesRead == 0)
				{
					CloseHandle(fileHandle);
					response.body.clear();
					response.headerUtf8 = Utf8FromCString(_T("ERR GET_FILE read failed\r\n"));
					return response;
				}
				offset += bytesRead;
			}
			CloseHandle(fileHandle);

			serverEvent.logLine.Format(_T("GET_FILE served: %s (%llu bytes)"), CString(pathW.c_str()).GetString(), static_cast<unsigned long long>(response.body.size()));
			serverEvent.lastResponse = _T("GET_FILE");

			CString headerLine;
			headerLine.Format(_T("OK\r\nSIZE=%llu\r\n\r\n"), static_cast<unsigned long long>(response.body.size()));
			response.headerUtf8 = Utf8FromCString(headerLine);
			return response;
		}

		if (upper == _T("PUT_FILE"))
		{
			if (requestBytes.size() < sizeof(unsigned int))
			{
				response.headerUtf8 = Utf8FromCString(_T("ERR PUT_FILE invalid payload\r\n"));
				return response;
			}

			size_t pos = 0;
			unsigned int pathLen = 0;
			memcpy(&pathLen, requestBytes.data() + pos, sizeof(pathLen));
			pos += sizeof(pathLen);

			if (requestBytes.size() < pos + pathLen + sizeof(unsigned long long))
			{
				response.headerUtf8 = Utf8FromCString(_T("ERR PUT_FILE invalid payload\r\n"));
				return response;
			}

			std::string pathUtf8(requestBytes.data() + pos, requestBytes.data() + pos + pathLen);
			pos += pathLen;

			unsigned long long fileSize = 0;
			memcpy(&fileSize, requestBytes.data() + pos, sizeof(fileSize));
			pos += sizeof(fileSize);

			if (fileSize > (50ULL * 1024 * 1024))
			{
				response.headerUtf8 = Utf8FromCString(_T("ERR PUT_FILE max 50MB\r\n"));
				return response;
			}

			if (requestBytes.size() < pos + static_cast<size_t>(fileSize))
			{
				response.headerUtf8 = Utf8FromCString(_T("ERR PUT_FILE size mismatch\r\n"));
				return response;
			}

			std::vector<char> pathBytes(pathUtf8.begin(), pathUtf8.end());
			std::wstring pathW = WideFromUtf8Bytes(pathBytes);

			HANDLE fileHandle = CreateFileW(pathW.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (fileHandle == INVALID_HANDLE_VALUE)
			{
				CString msg;
				msg.Format(_T("ERR PUT_FILE cannot create (error %lu)\r\n"), GetLastError());
				response.headerUtf8 = Utf8FromCString(msg);
				return response;
			}

			DWORD written = 0;
			size_t offset = 0;
			const char* data = requestBytes.data() + pos;
			while (offset < static_cast<size_t>(fileSize))
			{
				DWORD chunk = static_cast<DWORD>(std::min<size_t>(64 * 1024, static_cast<size_t>(fileSize) - offset));
				if (!WriteFile(fileHandle, data + offset, chunk, &written, nullptr) || written == 0)
				{
					CloseHandle(fileHandle);
					response.headerUtf8 = Utf8FromCString(_T("ERR PUT_FILE write failed\r\n"));
					return response;
				}
				offset += written;
			}
			CloseHandle(fileHandle);

			serverEvent.logLine.Format(_T("PUT_FILE saved: %s (%llu bytes)"), CString(pathW.c_str()).GetString(), fileSize);
			serverEvent.lastResponse = _T("PUT_FILE");
			response.headerUtf8 = Utf8FromCString(_T("OK PUT_FILE saved\r\n"));
			return response;
		}

		response.headerUtf8 = Utf8FromCString(_T("ERR Unsupported request. Allowed: PING, STATUS, PROCESS_SNAPSHOT, FETCH_MESSAGE, LIST_PROC, EXEC, GET_FILE, PUT_FILE\r\n"));
		return response;
	}
}

class CAboutDlg : public CDialogEx
{
public:
	CAboutDlg();

#ifdef AFX_DESIGN_TIME
	enum { IDD = IDD_ABOUTBOX };
#endif

protected:
	virtual void DoDataExchange(CDataExchange* pDX);
	DECLARE_MESSAGE_MAP()
};

CAboutDlg::CAboutDlg() : CDialogEx(IDD_ABOUTBOX)
{
}

void CAboutDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
}

BEGIN_MESSAGE_MAP(CAboutDlg, CDialogEx)
END_MESSAGE_MAP()

CServerDlg::CServerDlg(CWnd* pParent /*=nullptr*/)
	: CDialogEx(IDD_SERVER_DIALOG, pParent)
	, m_serverPort(kServerPort)
	, m_stopEvent(nullptr)
	, m_listenSocket(reinterpret_cast<void*>(INVALID_SOCKET))
	, m_activeClientCount(0)
	, m_selectedClientIndex(-1)
	, m_nextRequestId(1)
{
	m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
}

CServerDlg::~CServerDlg()
{
	StopServer();
	WSACleanup();
}

void CServerDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_LIST_CLIENTS, m_clientList);
	DDX_Control(pDX, IDC_LIST_PROCESSES, m_processList);
	DDX_Text(pDX, IDC_EDIT_MESSAGE, m_messageText);
	DDX_Text(pDX, IDC_EDIT_OUTPUT, m_output);
	DDX_Text(pDX, IDC_EDIT_EXEC_OUTPUT, m_execOutput);
	DDX_Text(pDX, IDC_STATIC_SERVER_STATE, m_serverState);
	DDX_Text(pDX, IDC_STATIC_LAST_RESPONSE, m_lastClientResponse);
}

BEGIN_MESSAGE_MAP(CServerDlg, CDialogEx)
	ON_WM_SYSCOMMAND()
	ON_WM_PAINT()
	ON_WM_QUERYDRAGICON()
	ON_BN_CLICKED(IDC_BUTTON_START_SERVER, &CServerDlg::OnBnClickedStartServer)
	ON_BN_CLICKED(IDC_BUTTON_STOP_SERVER, &CServerDlg::OnBnClickedStopServer)
	ON_BN_CLICKED(IDC_BUTTON_EXEC, &CServerDlg::OnBnClickedExec)
	ON_BN_CLICKED(IDC_BUTTON_LIST_PROC, &CServerDlg::OnBnClickedListProc)
	ON_BN_CLICKED(IDC_BUTTON_GET_FILE, &CServerDlg::OnBnClickedGetFile)
	ON_BN_CLICKED(IDC_BUTTON_PUT_FILE, &CServerDlg::OnBnClickedPutFile)
	ON_LBN_SELCHANGE(IDC_LIST_CLIENTS, &CServerDlg::OnLbnSelchangeClientList)
	ON_MESSAGE(WM_APP_WORKER_LOG, &CServerDlg::OnWorkerLog)
END_MESSAGE_MAP()

BOOL CServerDlg::OnInitDialog()
{
	CDialogEx::OnInitDialog();

	ASSERT((IDM_ABOUTBOX & 0xFFF0) == IDM_ABOUTBOX);
	ASSERT(IDM_ABOUTBOX < 0xF000);

	CMenu* pSysMenu = GetSystemMenu(FALSE);
	if (pSysMenu != nullptr)
	{
		BOOL bNameValid;
		CString strAboutMenu;
		bNameValid = strAboutMenu.LoadString(IDS_ABOUTBOX);
		ASSERT(bNameValid);
		if (!strAboutMenu.IsEmpty())
		{
			pSysMenu->AppendMenu(MF_SEPARATOR);
			pSysMenu->AppendMenu(MF_STRING, IDM_ABOUTBOX, strAboutMenu);
		}
	}

	SetIcon(m_hIcon, TRUE);
	SetIcon(m_hIcon, FALSE);

	WSADATA wsaData = {};
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
	{
		AppendOutputLine(_T("Winsock initialization failed."));
	}

	m_serverState.Format(_T("Stopped (port %u)"), m_serverPort);
	m_lastClientResponse = _T("No client snapshot yet.");
	m_messageText = _T("Enter message for the selected client.");
	UpdateData(FALSE);
	AppendOutputLine(_T("Ready. Server listens on 192.168.111.1, shows client snapshots, and can queue a text message for the selected client."));
	StartServerAsync();
	return TRUE;
}

void CServerDlg::OnSysCommand(UINT nID, LPARAM lParam)
{
	if ((nID & 0xFFF0) == IDM_ABOUTBOX)
	{
		CAboutDlg dlgAbout;
		dlgAbout.DoModal();
	}
	else
	{
		CDialogEx::OnSysCommand(nID, lParam);
	}
}

void CServerDlg::OnPaint()
{
	if (IsIconic())
	{
		CPaintDC dc(this);
		SendMessage(WM_ICONERASEBKGND, reinterpret_cast<WPARAM>(dc.GetSafeHdc()), 0);
		int cxIcon = GetSystemMetrics(SM_CXICON);
		int cyIcon = GetSystemMetrics(SM_CYICON);
		CRect rect;
		GetClientRect(&rect);
		int x = (rect.Width() - cxIcon + 1) / 2;
		int y = (rect.Height() - cyIcon + 1) / 2;
		dc.DrawIcon(x, y, m_hIcon);
	}
	else
	{
		CDialogEx::OnPaint();
	}
}

HCURSOR CServerDlg::OnQueryDragIcon()
{
	return static_cast<HCURSOR>(m_hIcon);
}

CString CServerDlg::GetServerStateText() const
{
	return m_serverState;
}

LONG CServerDlg::GetActiveClientCount() const
{
	return m_activeClientCount;
}

LONG CServerDlg::IncrementActiveClientCount()
{
	return InterlockedIncrement(&m_activeClientCount);
}

LONG CServerDlg::DecrementActiveClientCount()
{
	return InterlockedDecrement(&m_activeClientCount);
}

CString CServerDlg::ConsumePendingMessage(const CString& clientId)
{
	for (auto& client : m_clients)
	{
		if (client.clientId == clientId)
		{
			CString pending = client.pendingMessage;
			client.pendingMessage.Empty();
			return pending;
		}
	}
	return CString();
}

void CServerDlg::AppendOutputLine(const CString& line)
{
	UpdateData(TRUE);
	if (!m_output.IsEmpty())
	{
		m_output += _T("\r\n");
	}
	m_output += line;
	UpdateData(FALSE);
}

void CServerDlg::RefreshServerLabels()
{
	UpdateData(FALSE);
	if (GetDlgItem(IDC_BUTTON_START_SERVER) != nullptr)
	{
		GetDlgItem(IDC_BUTTON_START_SERVER)->EnableWindow(m_listenSocket == reinterpret_cast<void*>(INVALID_SOCKET));
	}
	if (GetDlgItem(IDC_BUTTON_STOP_SERVER) != nullptr)
	{
		GetDlgItem(IDC_BUTTON_STOP_SERVER)->EnableWindow(m_listenSocket != reinterpret_cast<void*>(INVALID_SOCKET));
	}
}

void CServerDlg::ReplaceProcessList(const CString& processSnapshot)
{
	m_processList.ResetContent();
	int start = 0;
	while (start < processSnapshot.GetLength())
	{
		int end = processSnapshot.Find(_T('\n'), start);
		CString line = (end == -1) ? processSnapshot.Mid(start) : processSnapshot.Mid(start, end - start);
		line.Trim();
		if (!line.IsEmpty())
		{
			m_processList.AddString(line);
		}
		if (end == -1)
		{
			break;
		}
		start = end + 1;
	}
}

void CServerDlg::UpsertClientSnapshot(const CString& clientId, const CString& lastResponse, const CString& processSnapshot)
{
	for (auto& client : m_clients)
	{
		if (client.clientId == clientId)
		{
			if (!lastResponse.IsEmpty())
			{
				client.lastResponse = lastResponse;
			}
			if (!processSnapshot.IsEmpty())
			{
				client.processSnapshot = processSnapshot;
			}
			RefreshClientList();
			return;
		}
	}

	ClientSnapshot snapshot = {};
	snapshot.clientId = clientId;
	snapshot.lastResponse = lastResponse;
	snapshot.processSnapshot = processSnapshot;
	m_clients.push_back(snapshot);
	RefreshClientList();
}

void CServerDlg::SetSelectedClientPendingMessage(const CString& message)
{
	if (m_selectedClientIndex < 0 || m_selectedClientIndex >= static_cast<int>(m_clients.size()))
	{
		return;
	}

	m_clients[static_cast<size_t>(m_selectedClientIndex)].pendingMessage = message;
}

void CServerDlg::RefreshClientList()
{
	int previousSelection = m_selectedClientIndex;
	m_clientList.ResetContent();
	for (size_t i = 0; i < m_clients.size(); ++i)
	{
		CString line;
		line.Format(_T("%s"), m_clients[i].clientId.GetString());
		m_clientList.AddString(line);
	}

	if (!m_clients.empty())
	{
		if (previousSelection < 0 || previousSelection >= static_cast<int>(m_clients.size()))
		{
			m_selectedClientIndex = 0;
		}
		else
		{
			m_selectedClientIndex = previousSelection;
		}
		m_clientList.SetCurSel(m_selectedClientIndex);
		ShowSelectedClientSnapshot();
	}
	else
	{
		m_selectedClientIndex = -1;
		m_processList.ResetContent();
	}
}

void CServerDlg::ShowSelectedClientSnapshot()
{
	if (m_selectedClientIndex < 0 || m_selectedClientIndex >= static_cast<int>(m_clients.size()))
	{
		m_processList.ResetContent();
		return;
	}

	const auto& client = m_clients[static_cast<size_t>(m_selectedClientIndex)];
	m_messageText = client.pendingMessage;
	m_execOutput = client.lastExecOutput;
	UpdateData(FALSE);
	if (!client.processSnapshot.IsEmpty())
	{
		ReplaceProcessList(client.processSnapshot);
	}
	else
	{
		m_processList.ResetContent();
	}
}

UINT CServerDlg::ServerThreadProc(LPVOID pParam)
{
	auto* pDlg = static_cast<CServerDlg*>(pParam);
	SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listenSocket == INVALID_SOCKET)
	{
		auto event = std::make_unique<ServerEvent>();
		event->logLine = _T("Server socket creation failed.");
		pDlg->PostMessage(WM_APP_WORKER_LOG, 0, reinterpret_cast<LPARAM>(event.release()));
		return 0;
	}

	sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_port = htons(static_cast<u_short>(pDlg->m_serverPort));
	InetPton(AF_INET, _T("192.168.111.1"), &address.sin_addr);

	BOOL reuse = TRUE;
	setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

	if (bind(listenSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
	{
		auto event = std::make_unique<ServerEvent>();
		event->logLine.Format(_T("Bind failed on 192.168.111.1:%u (error %d)."), pDlg->m_serverPort, WSAGetLastError());
		closesocket(listenSocket);
		pDlg->PostMessage(WM_APP_WORKER_LOG, 0, reinterpret_cast<LPARAM>(event.release()));
		return 0;
	}

	if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR)
	{
		auto event = std::make_unique<ServerEvent>();
		event->logLine.Format(_T("Listen failed (error %d)."), WSAGetLastError());
		closesocket(listenSocket);
		pDlg->PostMessage(WM_APP_WORKER_LOG, 0, reinterpret_cast<LPARAM>(event.release()));
		return 0;
	}

	u_long nonBlocking = 1;
	ioctlsocket(listenSocket, FIONBIO, &nonBlocking);
	pDlg->m_listenSocket = reinterpret_cast<void*>(listenSocket);
	pDlg->m_serverState.Format(_T("Listening on 192.168.111.1:%u"), pDlg->m_serverPort);
	auto started = std::make_unique<ServerEvent>();
	started->logLine = _T("Server is listening for localhost clients.");
	pDlg->PostMessage(WM_APP_WORKER_LOG, 0, reinterpret_cast<LPARAM>(started.release()));

	while (WaitForSingleObject(pDlg->m_stopEvent, 50) == WAIT_TIMEOUT)
	{
		sockaddr_in clientAddress = {};
		int clientLength = sizeof(clientAddress);
		SOCKET clientSocket = accept(listenSocket, reinterpret_cast<sockaddr*>(&clientAddress), &clientLength);
		if (clientSocket == INVALID_SOCKET)
		{
			int error = WSAGetLastError();
			if (error == WSAEWOULDBLOCK)
			{
				continue;
			}
			break;
		}

		u_long clientBlocking = 0;
		ioctlsocket(clientSocket, FIONBIO, &clientBlocking);

		AfxBeginThread(&CServerDlg::ClientSessionThreadProc, new ClientSessionPayload{ pDlg, clientSocket, MakePeerLabel(clientAddress) });
	}

	closesocket(listenSocket);
	pDlg->m_listenSocket = reinterpret_cast<void*>(INVALID_SOCKET);
	pDlg->m_serverState.Format(_T("Stopped (port %u)"), pDlg->m_serverPort);
	auto stopped = std::make_unique<ServerEvent>();
	stopped->logLine = _T("Server stopped.");
	pDlg->PostMessage(WM_APP_WORKER_LOG, 0, reinterpret_cast<LPARAM>(stopped.release()));
	return 0;
}

UINT CServerDlg::ClientSessionThreadProc(LPVOID pParam)
{
	std::unique_ptr<ClientSessionPayload> payload(static_cast<ClientSessionPayload*>(pParam));
	LONG currentClients = payload->pDlg->IncrementActiveClientCount();
	{
		auto accepted = std::make_unique<ServerEvent>();
		accepted->logLine.Format(_T("Accepted client %s. Active client threads: %ld"), payload->remoteAddress.GetString(), currentClients);
		payload->pDlg->PostMessage(WM_APP_WORKER_LOG, 0, reinterpret_cast<LPARAM>(accepted.release()));
	}

	Frame hello = {};
	if (!RecvFrame(payload->socket, hello) || hello.type != FrameType::HELLO)
	{
		auto event = std::make_unique<ServerEvent>();
		event->clientId = payload->remoteAddress;
		int wsa = WSAGetLastError();
		event->logLine.Format(_T("Client %s missing/invalid HELLO (WSA=%d). Closing."), payload->remoteAddress.GetString(), wsa);
		payload->pDlg->PostMessage(WM_APP_WORKER_LOG, 0, reinterpret_cast<LPARAM>(event.release()));
		closesocket(payload->socket);
		payload->pDlg->DecrementActiveClientCount();
		return 0;
	}

	CString clientId = CStringFromUtf8(std::string(hello.payload.begin(), hello.payload.end()));
	clientId.Trim();
	if (clientId.IsEmpty())
	{
		clientId = payload->remoteAddress;
	}

	{
		CSingleLock lock(&payload->pDlg->m_onlineClientsLock, TRUE);
		CServerDlg::OnlineClient online = {};
		online.clientId = clientId;
		online.remoteAddress = payload->remoteAddress;
		online.socketHandle = reinterpret_cast<void*>(payload->socket);
		payload->pDlg->m_onlineClients.push_back(online);
	}

	{
		auto event = std::make_unique<ServerEvent>();
		event->clientId = clientId;
		event->logLine.Format(_T("Client online: %s (%s)"), clientId.GetString(), payload->remoteAddress.GetString());
		event->lastResponse = _T("HELLO");
		payload->pDlg->PostMessage(WM_APP_WORKER_LOG, 0, reinterpret_cast<LPARAM>(event.release()));
	}

	while (WaitForSingleObject(payload->pDlg->m_stopEvent, 10) == WAIT_TIMEOUT)
	{
		Frame frame = {};
		if (!RecvFrame(payload->socket, frame))
		{
			auto ev = std::make_unique<ServerEvent>();
			ev->clientId = clientId;
			int wsa = WSAGetLastError();
			ev->logLine.Format(_T("Client %s recv failed/disconnected (WSA=%d)."), clientId.GetString(), wsa);
			payload->pDlg->PostMessage(WM_APP_WORKER_LOG, 0, reinterpret_cast<LPARAM>(ev.release()));
			break;
		}

		auto event = std::make_unique<ServerEvent>();
		event->clientId = clientId;
		CString payloadText = CStringFromUtf8(std::string(frame.payload.begin(), frame.payload.end()));

		switch (frame.type)
		{
		case FrameType::EXEC_RESP:
			event->logLine = _T("Received EXEC response (PowerShell).");
			event->lastResponse = _T("EXEC");
			event->execOutput = payloadText;
			break;
		case FrameType::LIST_PROC_RESP:
			event->logLine = _T("Received LIST_PROC response.");
			event->lastResponse = _T("LIST_PROC");
			event->processSnapshot = payloadText;
			break;
		case FrameType::PUT_FILE_RESP:
			event->logLine = _T("Received PUT_FILE response.");
			event->lastResponse = payloadText;
			break;
		case FrameType::GET_FILE_RESP:
		{
			event->logLine = _T("Received GET_FILE response.");
			event->lastResponse = _T("GET_FILE");

			if (frame.payload.size() < sizeof(unsigned int) + sizeof(unsigned long long))
			{
				event->lastResponse = _T("GET_FILE: invalid payload.");
				break;
			}
			unsigned int status = 1;
			unsigned long long size = 0;
			memcpy(&status, frame.payload.data(), sizeof(status));
			memcpy(&size, frame.payload.data() + sizeof(status), sizeof(size));

			CString savePath;
			{
				CSingleLock lock(&payload->pDlg->m_pendingLock, TRUE);
				auto it = payload->pDlg->m_pendingDownloadSavePath.find(frame.requestId);
				if (it != payload->pDlg->m_pendingDownloadSavePath.end())
				{
					savePath = it->second;
					payload->pDlg->m_pendingDownloadSavePath.erase(it);
				}
			}

			if (status != 0)
			{
				event->lastResponse = payloadText;
				break;
			}

			size_t headerSize = sizeof(unsigned int) + sizeof(unsigned long long);
			if (frame.payload.size() < headerSize + static_cast<size_t>(size))
			{
				event->lastResponse = _T("GET_FILE: size mismatch.");
				break;
			}
			if (savePath.IsEmpty())
			{
				event->lastResponse = _T("GET_FILE: no save path (request not found).");
				break;
			}

			HANDLE outFile = CreateFile(savePath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (outFile == INVALID_HANDLE_VALUE)
			{
				event->lastResponse = _T("GET_FILE: cannot create local file.");
				break;
			}
			DWORD written = 0;
			const char* data = frame.payload.data() + headerSize;
			unsigned long long remaining = size;
			while (remaining > 0)
			{
				DWORD chunk = static_cast<DWORD>(std::min<unsigned long long>(64 * 1024ULL, remaining));
				if (!WriteFile(outFile, data + (size - remaining), chunk, &written, nullptr) || written == 0)
				{
					CloseHandle(outFile);
					event->lastResponse = _T("GET_FILE: write failed.");
					break;
				}
				remaining -= written;
			}
			CloseHandle(outFile);
			CString msg;
			msg.Format(_T("GET_FILE saved to %s (%llu bytes)"), savePath.GetString(), size);
			event->lastResponse = msg;
			break;
		}
		case FrameType::ERROR_RESP:
			event->logLine = _T("Client returned error: ") + payloadText;
			event->lastResponse = payloadText;
			{
				CSingleLock lock(&payload->pDlg->m_pendingLock, TRUE);
				payload->pDlg->m_pendingDownloadSavePath.erase(frame.requestId);
			}
			break;
		default:
			event->logLine = _T("Received unsupported frame type.");
			event->lastResponse = payloadText;
			break;
		}

		payload->pDlg->PostMessage(WM_APP_WORKER_LOG, 0, reinterpret_cast<LPARAM>(event.release()));
	}

	{
		CSingleLock lock(&payload->pDlg->m_onlineClientsLock, TRUE);
		auto& vec = payload->pDlg->m_onlineClients;
		vec.erase(std::remove_if(vec.begin(), vec.end(), [&](const CServerDlg::OnlineClient& c)
			{
				return c.socketHandle == reinterpret_cast<void*>(payload->socket);
			}), vec.end());
	}

	closesocket(payload->socket);

	{
		auto event = std::make_unique<ServerEvent>();
		event->clientId = clientId;
		event->logLine.Format(_T("Client offline: %s"), clientId.GetString());
		payload->pDlg->PostMessage(WM_APP_WORKER_LOG, 0, reinterpret_cast<LPARAM>(event.release()));
	}

	LONG remainingClients = payload->pDlg->DecrementActiveClientCount();
	auto closed = std::make_unique<ServerEvent>();
	closed->logLine.Format(_T("Closed client %s. Active client threads: %ld"), payload->remoteAddress.GetString(), remainingClients);
	payload->pDlg->PostMessage(WM_APP_WORKER_LOG, 0, reinterpret_cast<LPARAM>(closed.release()));
	return 0;
}

void CServerDlg::StartServerAsync()
{
	if (m_listenSocket != reinterpret_cast<void*>(INVALID_SOCKET))
	{
		return;
	}

	if (m_stopEvent != nullptr)
	{
		CloseHandle(m_stopEvent);
	}

	m_stopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
	m_serverState = _T("Starting...");
	RefreshServerLabels();
	AfxBeginThread(&CServerDlg::ServerThreadProc, this);
}

void CServerDlg::StopServer()
{
	if (m_stopEvent != nullptr)
	{
		SetEvent(m_stopEvent);
	}

	SOCKET listenSocket = reinterpret_cast<SOCKET>(m_listenSocket);
	if (listenSocket != INVALID_SOCKET)
	{
		closesocket(listenSocket);
		m_listenSocket = reinterpret_cast<void*>(INVALID_SOCKET);
	}

	if (m_stopEvent != nullptr)
	{
		CloseHandle(m_stopEvent);
		m_stopEvent = nullptr;
	}

	m_serverState.Format(_T("Stopped (port %u)"), m_serverPort);
	RefreshServerLabels();
}

void CServerDlg::OnBnClickedStartServer()
{
	StartServerAsync();
}

void CServerDlg::OnBnClickedStopServer()
{
	StopServer();
	AppendOutputLine(_T("Server stop requested."));
}

void CServerDlg::OnBnClickedExec()
{
	UpdateData(TRUE);
	m_messageText.Trim();
	if (m_selectedClientIndex < 0 || m_selectedClientIndex >= static_cast<int>(m_clients.size()))
	{
		AppendOutputLine(_T("No client selected."));
		return;
	}
	if (m_messageText.IsEmpty())
	{
		AppendOutputLine(_T("Command is empty."));
		return;
	}

	const CString clientId = m_clients[static_cast<size_t>(m_selectedClientIndex)].clientId;
	SOCKET clientSocket = INVALID_SOCKET;
	{
		CSingleLock lock(&m_onlineClientsLock, TRUE);
		for (const auto& c : m_onlineClients)
		{
			if (c.clientId == clientId)
			{
				clientSocket = reinterpret_cast<SOCKET>(c.socketHandle);
				break;
			}
		}
	}
	if (clientSocket == INVALID_SOCKET)
	{
		AppendOutputLine(_T("Selected client is offline."));
		return;
	}

	unsigned int requestId = static_cast<unsigned int>(InterlockedIncrement(&m_nextRequestId));
	std::string cmdUtf8 = Utf8FromCString(m_messageText);
	if (!SendFrame(clientSocket, FrameType::EXEC_REQ, requestId, cmdUtf8.data(), static_cast<unsigned int>(cmdUtf8.size())))
	{
		CString msg;
		msg.Format(_T("Failed to send EXEC frame (WSA=%d)."), WSAGetLastError());
		AppendOutputLine(msg);
		return;
	}

	CString logLine;
	logLine.Format(_T("Sent EXEC to %s (req=%u)"), clientId.GetString(), requestId);
	AppendOutputLine(logLine);
}

void CServerDlg::OnBnClickedListProc()
{
	if (m_selectedClientIndex < 0 || m_selectedClientIndex >= static_cast<int>(m_clients.size()))
	{
		AppendOutputLine(_T("No client selected."));
		return;
	}
	const CString clientId = m_clients[static_cast<size_t>(m_selectedClientIndex)].clientId;
	SOCKET clientSocket = INVALID_SOCKET;
	{
		CSingleLock lock(&m_onlineClientsLock, TRUE);
		for (const auto& c : m_onlineClients)
		{
			if (c.clientId == clientId)
			{
				clientSocket = reinterpret_cast<SOCKET>(c.socketHandle);
				break;
			}
		}
	}
	if (clientSocket == INVALID_SOCKET)
	{
		AppendOutputLine(_T("Selected client is offline."));
		return;
	}

	unsigned int requestId = static_cast<unsigned int>(InterlockedIncrement(&m_nextRequestId));
	if (!SendFrame(clientSocket, FrameType::LIST_PROC_REQ, requestId, nullptr, 0))
	{
		CString msg;
		msg.Format(_T("Failed to send LIST_PROC frame (WSA=%d)."), WSAGetLastError());
		AppendOutputLine(msg);
		return;
	}
	CString logLine;
	logLine.Format(_T("Sent LIST_PROC to %s (req=%u)"), clientId.GetString(), requestId);
	AppendOutputLine(logLine);
}

void CServerDlg::OnBnClickedGetFile()
{
	if (m_selectedClientIndex < 0 || m_selectedClientIndex >= static_cast<int>(m_clients.size()))
	{
		AppendOutputLine(_T("No client selected."));
		return;
	}
	const CString clientId = m_clients[static_cast<size_t>(m_selectedClientIndex)].clientId;
	SOCKET clientSocket = INVALID_SOCKET;
	{
		CSingleLock lock(&m_onlineClientsLock, TRUE);
		for (const auto& c : m_onlineClients)
		{
			if (c.clientId == clientId)
			{
				clientSocket = reinterpret_cast<SOCKET>(c.socketHandle);
				break;
			}
		}
	}
	if (clientSocket == INVALID_SOCKET)
	{
		AppendOutputLine(_T("Selected client is offline."));
		return;
	}

	UpdateData(TRUE);
	CString remotePath = m_messageText;
	remotePath.Trim();
	if (remotePath.IsEmpty())
	{
		AppendOutputLine(_T("Enter remote path into the command box first."));
		return;
	}

	CFileDialog saveDlg(FALSE);
	if (saveDlg.DoModal() != IDOK)
	{
		return;
	}
	CString savePath = saveDlg.GetPathName();

	unsigned int requestId = static_cast<unsigned int>(InterlockedIncrement(&m_nextRequestId));
	{
		CSingleLock lock(&m_pendingLock, TRUE);
		m_pendingDownloadSavePath[requestId] = savePath;
	}
	std::string pathUtf8 = Utf8FromCString(remotePath);
	if (!SendFrame(clientSocket, FrameType::GET_FILE_REQ, requestId, pathUtf8.data(), static_cast<unsigned int>(pathUtf8.size())))
	{
		CString msg;
		msg.Format(_T("Failed to send GET_FILE frame (WSA=%d)."), WSAGetLastError());
		AppendOutputLine(msg);
		return;
	}
	CString logLine;
	logLine.Format(_T("Sent GET_FILE to %s (req=%u)"), clientId.GetString(), requestId);
	AppendOutputLine(logLine);
}

void CServerDlg::OnBnClickedPutFile()
{
	if (m_selectedClientIndex < 0 || m_selectedClientIndex >= static_cast<int>(m_clients.size()))
	{
		AppendOutputLine(_T("No client selected."));
		return;
	}
	const CString clientId = m_clients[static_cast<size_t>(m_selectedClientIndex)].clientId;
	SOCKET clientSocket = INVALID_SOCKET;
	{
		CSingleLock lock(&m_onlineClientsLock, TRUE);
		for (const auto& c : m_onlineClients)
		{
			if (c.clientId == clientId)
			{
				clientSocket = reinterpret_cast<SOCKET>(c.socketHandle);
				break;
			}
		}
	}
	if (clientSocket == INVALID_SOCKET)
	{
		AppendOutputLine(_T("Selected client is offline."));
		return;
	}

	UpdateData(TRUE);
	CString remoteDestPath = m_messageText;
	remoteDestPath.Trim();
	if (remoteDestPath.IsEmpty())
	{
		AppendOutputLine(_T("Enter remote destination path into the command box first."));
		return;
	}

	CFileDialog openDlg(TRUE);
	if (openDlg.DoModal() != IDOK)
	{
		return;
	}
	CString localPath = openDlg.GetPathName();

	HANDLE fileHandle = CreateFile(localPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (fileHandle == INVALID_HANDLE_VALUE)
	{
		AppendOutputLine(_T("Cannot open local file."));
		return;
	}
	LARGE_INTEGER size = {};
	if (!GetFileSizeEx(fileHandle, &size) || size.QuadPart < 0 || size.QuadPart >(50LL * 1024 * 1024))
	{
		CloseHandle(fileHandle);
		AppendOutputLine(_T("Unsupported file size (max 50MB)."));
		return;
	}

	std::vector<char> bytes(static_cast<size_t>(size.QuadPart));
	DWORD bytesRead = 0;
	size_t offset = 0;
	while (offset < bytes.size())
	{
		DWORD chunk = static_cast<DWORD>(std::min<size_t>(64 * 1024, bytes.size() - offset));
		if (!ReadFile(fileHandle, bytes.data() + offset, chunk, &bytesRead, nullptr) || bytesRead == 0)
		{
			CloseHandle(fileHandle);
			AppendOutputLine(_T("Read local file failed."));
			return;
		}
		offset += bytesRead;
	}
	CloseHandle(fileHandle);

	CString fileName = localPath;
	int lastSlash = fileName.ReverseFind(_T('\\'));
	if (lastSlash == -1)
	{
		lastSlash = fileName.ReverseFind(_T('/'));
	}
	if (lastSlash != -1)
	{
		fileName = fileName.Mid(lastSlash + 1);
	}

	CString finalRemotePath = remoteDestPath;
	if (finalRemotePath.Right(1) != _T("\\") && finalRemotePath.Right(1) != _T("/"))
	{
		finalRemotePath += _T("\\");
	}
	finalRemotePath += fileName;

	unsigned int requestId = static_cast<unsigned int>(InterlockedIncrement(&m_nextRequestId));
	std::string pathUtf8 = Utf8FromCString(finalRemotePath);
	unsigned int pathLen = static_cast<unsigned int>(pathUtf8.size());
	unsigned long long fileSize = static_cast<unsigned long long>(bytes.size());

	std::vector<char> payload;
	payload.resize(sizeof(pathLen) + pathLen + sizeof(fileSize) + bytes.size());
	size_t pos = 0;
	memcpy(payload.data() + pos, &pathLen, sizeof(pathLen));
	pos += sizeof(pathLen);
	memcpy(payload.data() + pos, pathUtf8.data(), pathLen);
	pos += pathLen;
	memcpy(payload.data() + pos, &fileSize, sizeof(fileSize));
	pos += sizeof(fileSize);
	memcpy(payload.data() + pos, bytes.data(), bytes.size());

	if (!SendFrame(clientSocket, FrameType::PUT_FILE_REQ, requestId, payload.data(), static_cast<unsigned int>(payload.size())))
	{
		CString msg;
		msg.Format(_T("Failed to send PUT_FILE frame (WSA=%d)."), WSAGetLastError());
		AppendOutputLine(msg);
		return;
	}
	CString logLine;
	logLine.Format(_T("Sent PUT_FILE to %s (req=%u)"), clientId.GetString(), requestId);
	AppendOutputLine(logLine);
}

void CServerDlg::OnLbnSelchangeClientList()
{
	m_selectedClientIndex = m_clientList.GetCurSel();
	ShowSelectedClientSnapshot();
}

LRESULT CServerDlg::OnWorkerLog(WPARAM, LPARAM lParam)
{
	std::unique_ptr<ServerEvent> event(reinterpret_cast<ServerEvent*>(lParam));
	if (event != nullptr)
	{
		if (!event->clientId.IsEmpty())
		{
			UpsertClientSnapshot(event->clientId, event->lastResponse, event->processSnapshot);
		}
		if (!event->execOutput.IsEmpty())
		{
			for (auto& client : m_clients)
			{
				if (client.clientId == event->clientId)
				{
					client.lastExecOutput = event->execOutput;
					break;
				}
			}
			if (m_selectedClientIndex >= 0 && m_selectedClientIndex < static_cast<int>(m_clients.size()) &&
				m_clients[static_cast<size_t>(m_selectedClientIndex)].clientId == event->clientId)
			{
				m_execOutput = event->execOutput;
				UpdateData(FALSE);
			}
		}
		if (!event->lastResponse.IsEmpty())
		{
			m_lastClientResponse = event->lastResponse;
		}
		if (!event->logLine.IsEmpty())
		{
			AppendOutputLine(event->logLine);
		}
		RefreshServerLabels();
	}
	return 0;
}
