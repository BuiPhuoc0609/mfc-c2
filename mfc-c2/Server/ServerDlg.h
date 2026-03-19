// ServerDlg.h : header file
//

#pragma once

#include <afxwin.h>
#include <afxmt.h>
#include <vector>
#include <map>

class CListBox;

class CServerDlg : public CDialogEx
{
public:
	CServerDlg(CWnd* pParent = nullptr);
	virtual ~CServerDlg();
	CString GetServerStateText() const;
	LONG GetActiveClientCount() const;
	LONG IncrementActiveClientCount();
	LONG DecrementActiveClientCount();
	CString ConsumePendingMessage(const CString& clientId);

#ifdef AFX_DESIGN_TIME
	enum { IDD = IDD_SERVER_DIALOG };
#endif

protected:
	struct ClientSnapshot
	{
		CString clientId;
		CString lastResponse;
		CString processSnapshot;
		CString pendingMessage;
		CString lastExecOutput;
	};

	struct OnlineClient
	{
		CString clientId;
		CString remoteAddress;
		void* socketHandle;
	};

	virtual void DoDataExchange(CDataExchange* pDX);

	HICON m_hIcon;
	CListBox m_clientList;
	CListBox m_processList;
	CString m_messageText;
	CString m_output;
	CString m_execOutput;
	CString m_serverState;
	CString m_lastClientResponse;
	UINT m_serverPort;
	HANDLE m_stopEvent;
	void* m_listenSocket;
	volatile LONG m_activeClientCount;
	std::vector<ClientSnapshot> m_clients;
	int m_selectedClientIndex;

	CCriticalSection m_onlineClientsLock;
	std::vector<OnlineClient> m_onlineClients;
	volatile LONG m_nextRequestId;

	CCriticalSection m_pendingLock;
	std::map<unsigned int, CString> m_pendingDownloadSavePath; // requestId -> local save path

	void AppendOutputLine(const CString& line);
	void RefreshServerLabels();
	void StartServerAsync();
	void StopServer();
	void ReplaceProcessList(const CString& processSnapshot);
	void UpsertClientSnapshot(const CString& clientId, const CString& lastResponse, const CString& processSnapshot);
	void RefreshClientList();
	void ShowSelectedClientSnapshot();
	void SetSelectedClientPendingMessage(const CString& message);

	static UINT ServerThreadProc(LPVOID pParam);
	static UINT ClientSessionThreadProc(LPVOID pParam);

	virtual BOOL OnInitDialog();
	afx_msg void OnSysCommand(UINT nID, LPARAM lParam);
	afx_msg void OnPaint();
	afx_msg HCURSOR OnQueryDragIcon();
	afx_msg void OnBnClickedStartServer();
	afx_msg void OnBnClickedStopServer();
	afx_msg void OnBnClickedExec();
	afx_msg void OnBnClickedListProc();
	afx_msg void OnBnClickedGetFile();
	afx_msg void OnBnClickedPutFile();
	afx_msg void OnLbnSelchangeClientList();
	afx_msg LRESULT OnWorkerLog(WPARAM wParam, LPARAM lParam);
	DECLARE_MESSAGE_MAP()
};
