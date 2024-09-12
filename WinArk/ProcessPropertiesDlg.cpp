#include "stdafx.h"
#include "ProcessPropertiesDlg.h"
#include "FormatHelper.h"
#include "EnvironmentDlg.h"
#include "DriverHelper.h"
#include "TokenPropertiesDlg.h"
#include "JobPropertiesDlg.h"
#include "ObjectManager.h"
#include "ClipboardHelper.h"

using namespace WinSys;

void CProcessPropertiesDlg::OnFinalMessage(HWND) {
	if (!m_Modal)
		delete this;
}

LRESULT CProcessPropertiesDlg::OnCloseCmd(WORD, WORD wID, HWND, BOOL&) {
	if (m_Modal)
		EndDialog(wID);
	else
		DestroyWindow();
	return 0;
}

LRESULT CProcessPropertiesDlg::OnGetMinMaxInfo(UINT, WPARAM, LPARAM lParam, BOOL& handled) {
	auto pMMI = (PMINMAXINFO)lParam;
	pMMI->ptMaxTrackSize.y = m_ptMinTrackSize.y;
	pMMI->ptMaxTrackSize.x = m_ptMinTrackSize.x * 2;
	handled = FALSE;

	return 0;
}

LRESULT CProcessPropertiesDlg::OnInitDialog(UINT, WPARAM, LPARAM, BOOL&) {
	InitDynamicLayout();

	InitProcess();

	return 0;
}

void CProcessPropertiesDlg::InitProcess() {
	auto pi = m_px.GetProcessInfo();
	CString text;
	text.Format(L"%s (%u) Properties", pi->GetImageName().c_str(), pi->Id);
	SetWindowText(text);

	HICON hIcon = nullptr, hIconBig;
	::ExtractIconEx(m_px.GetExecutablePath().c_str(), 0, &hIconBig, &hIcon,1);
	if (hIcon == nullptr) {
		hIcon = AtlLoadSysIcon(IDI_APPLICATION);
		hIconBig = hIcon;
	}
	((CStatic)GetDlgItem(IDC_APPICON)).SetIcon(hIconBig);

	SetDlgItemText(IDC_NAME, (L" " + pi->GetImageName()).c_str());
	SetDlgItemInt(IDC_PID, pi->Id);
	auto& path = m_px.GetExecutablePath();
	bool imagePath = false;
	if (!path.empty() && path[1] == L':') {
		imagePath = true;
		SetDlgItemText(IDC_PATH, path.c_str());
	}
	SetDlgItemText(IDC_COMMANDLINE, m_px.GetCmdLine().c_str());
	text.Format(L"%d bit", m_px.GetBitness());
	SetDlgItemText(IDC_PLATFORM, text);
	SetDlgItemText(IDC_USERNAME, m_px.UserName().c_str());
	SetDlgItemText(IDC_CREATED, FormatHelper::TimeToString(pi->CreateTime));

	if (pi->ParentId > 0) {
		auto parent = m_pm.GetProcessById(pi->ParentId);
		if (parent && (parent->CreateTime < pi->CreateTime || parent->Id == 4)) {
			text.Format(L" %s (%u)", parent->GetImageName().c_str(), parent->Id);
		}
		else {
			text.Format(L" <non-existent> (%u)", pi->ParentId);
		}
	}
	else {
		text.Empty();
	}
	SetDlgItemText(IDC_PARENT, text);
	SetDlgItemText(IDC_DESC, m_px.GetDescription().c_str());

	GetDlgItem(IDC_EXPLORE).EnableWindow(imagePath);
	auto dir = m_px.GetCurDirectory();
	if (dir.empty())
		GetDlgItem(IDC_EXPLORE_DIR).EnableWindow(FALSE);
	else
		SetDlgItemText(IDC_CURDIR, dir.c_str());

	// enable / disable job button
	GetDlgItem(IDC_JOB).EnableWindow((m_px.GetAttributes(m_pm) & ProcessAttributes::InJob) == ProcessAttributes::InJob);
}

LRESULT CProcessPropertiesDlg::OnShowEnvironment(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	HANDLE  hProcess = DriverHelper::OpenProcess(m_px.GetProcessInfo()->Id, PROCESS_QUERY_INFORMATION | PROCESS_VM_READ);
	if (!hProcess) {
		AtlMessageBox(*this, L"Failed to open process", IDS_TITLE, MB_ICONERROR);
		return 0;
	}

	CEnvironmentDlg dlg(hProcess);
	dlg.DoModal();
	::CloseHandle(hProcess);
	return 0;
}

LRESULT CProcessPropertiesDlg::OnShowToken(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	HANDLE hToken = nullptr;
	auto hProcess = DriverHelper::OpenProcess(m_px.GetProcessInfo()->Id, PROCESS_QUERY_INFORMATION);
	if (hProcess) {
		::OpenProcessToken(hProcess, TOKEN_QUERY, &hToken);
		::CloseHandle(hProcess);
	}
	if (!hToken) {
		AtlMessageBox(*this, L"Failed to open process token", IDS_TITLE, MB_ICONERROR);
		return 0;
	}
	CTokenPropertiesDlg dlg(hToken);
	dlg.DoModal();

	return 0;
}

LRESULT CProcessPropertiesDlg::OnShowJob(WORD, WORD wID, HWND, BOOL&) {
	ObjectManager mgr;
	mgr.EnumHandles(L"Job");
	HANDLE hJob{ nullptr };
	USHORT type = 0;
	std::vector<HANDLE> handles;
	for (auto& hi : mgr.GetHandles()) {
		hJob = DriverHelper::DupHandle(UlongToHandle(hi->HandleValue), hi->ProcessId, JOB_OBJECT_QUERY | SYNCHRONIZE, 0);
		if (!hJob)
			continue;
		if (m_px.GetProcess()->IsInJob(hJob)) {
			handles.push_back(hJob);
			if (type == 0)
				type = hi->ObjectTypeIndex;
		}
		hJob = nullptr;
	}
	if (handles.empty()) {
		AtlMessageBox(*this, L"Failed to open job object", IDS_TITLE, MB_ICONERROR);
		return 0;
	}
	hJob = handles.back();
	handles.pop_back();
	std::for_each(handles.begin(), handles.end(), [](auto h) { ::CloseHandle(h); });

	auto name = mgr.GetObjectName(hJob, type);
	CJobPropertiesDlg dlg(m_pm, hJob, name.c_str());
	dlg.DoModal();
	::CloseHandle(hJob);
	return 0;
}

LRESULT CProcessPropertiesDlg::OnExplore(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	if ((INT_PTR)::ShellExecute(nullptr, L"open", L"explorer",
		(L"/select,\"" + m_px.GetExecutablePath() + L"\"").c_str(),
		nullptr, SW_SHOWDEFAULT) < 32)
		AtlMessageBox(*this, L"Failed to locate executable", IDS_TITLE, MB_ICONERROR);

	return 0;
}

LRESULT CProcessPropertiesDlg::OnExploreDirectory(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	if ((INT_PTR)::ShellExecute(nullptr, L"explore", m_px.GetCurDirectory().c_str(),
		nullptr, nullptr, SW_SHOWDEFAULT) < 32)
		AtlMessageBox(*this, L"Failed to locate directory", IDS_TITLE, MB_ICONERROR);

	return 0;
}

LRESULT CProcessPropertiesDlg::OnCopy(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	auto& cmd = m_px.GetCmdLine();
	if (!cmd.empty()) {
		ClipboardHelper::CopyText(*this, cmd.c_str());
	}
	return 0;
}