#include "stdafx.h"
#include "QHEngineUI.h"
#include "QHEngineUIDlg.h"
#include "resource.h"
#include <Shlwapi.h>

#pragma comment(lib, "Shlwapi.lib")

IMPLEMENT_DYNAMIC(CQHEngineUIDlg, CDialogEx)

CQHEngineUIDlg::CQHEngineUIDlg(CWnd* pParent /*=nullptr*/)
    : CDialogEx(IDD_QHENGINEUI_DIALOG, pParent)
{
    m_hIcon = AfxGetApp()->LoadStandardIcon(IDI_APPLICATION);
}

CQHEngineUIDlg::~CQHEngineUIDlg()
{
}

void CQHEngineUIDlg::DoDataExchange(CDataExchange* pDX)
{
    CDialogEx::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_VOLUME_LIST, m_listVolumes);
    DDX_Control(pDX, IDC_BTN_ENABLE, m_btnEnable);
    DDX_Control(pDX, IDC_BTN_DISABLE, m_btnDisable);
    DDX_Control(pDX, IDC_STATUS, m_status);
}

BEGIN_MESSAGE_MAP(CQHEngineUIDlg, CDialogEx)
    ON_BN_CLICKED(IDC_BTN_ENABLE, &CQHEngineUIDlg::OnBnClickedBtnEnable)
    ON_BN_CLICKED(IDC_BTN_DISABLE, &CQHEngineUIDlg::OnBnClickedBtnDisable)
    ON_NOTIFY(NM_CLICK, IDC_LINK_52POJIE, &CQHEngineUIDlg::OnNMClickLink52pojie)
    ON_NOTIFY(NM_RETURN, IDC_LINK_52POJIE, &CQHEngineUIDlg::OnNMClickLink52pojie)
    ON_WM_CTLCOLOR()
END_MESSAGE_MAP()

BOOL CQHEngineUIDlg::OnInitDialog()
{
    CDialogEx::OnInitDialog();

    SetIcon(m_hIcon, TRUE);
    SetIcon(m_hIcon, FALSE);

    m_statusColor = RGB(51, 51, 51);

    // 枚举卷并填充列表
    EnumerateVolumes();
    PopulateVolumeList();

    // 根据当前驱动状态启用/禁用按钮和列表
    UpdateUIByProtectStatus();

    return TRUE;
}

void CQHEngineUIDlg::EnumerateVolumes()
{
    m_volumes.RemoveAll();

    WCHAR driveStrings[256] = { 0 };
    DWORD len = GetLogicalDriveStringsW(256, driveStrings);
    if (len == 0)
        return;

    WCHAR fsName[MAX_PATH] = { 0 };
    WCHAR volumeName[MAX_PATH] = { 0 };
    const WCHAR* p = driveStrings;

    while (*p)
    {
        CString driveStr(p);
        WCHAR rootPath[4] = { driveStr[0], L':', L'\\', 0 };

        // 只处理固定磁盘
        if (GetDriveTypeW(rootPath) != DRIVE_FIXED)
        {
            p += driveStr.GetLength() + 1;
            continue;
        }

        QHVolumeInfo info;
        info.DriveLetter = driveStr[0];

        // 获取文件系统类型和卷标
        fsName[0] = L'\0';
        volumeName[0] = L'\0';
        GetVolumeInformationW(rootPath, volumeName, MAX_PATH, NULL, NULL, NULL, fsName, MAX_PATH);

        CString fsStr(fsName);
        CString volLabel(volumeName);

        if (!volLabel.IsEmpty())
            info.DisplayText.Format(L"%c: (%s) - %s", info.DriveLetter, volLabel, fsStr);
        else
            info.DisplayText.Format(L"%c: (%s)", info.DriveLetter, fsStr);

        m_volumes.Add(info);
        p += driveStr.GetLength() + 1;
    }
}

void CQHEngineUIDlg::PopulateVolumeList()
{
    m_listVolumes.ResetContent();

    for (INT_PTR i = 0; i < m_volumes.GetCount(); i++)
    {
        int idx = m_listVolumes.AddString(m_volumes[i].DisplayText);
        m_listVolumes.SetCheck(idx, 1); // 默认全选
    }
}

// 前向声明（实现在 DisableProtect 之后）
static BOOL RunBcdedit(const wchar_t* args);

void CQHEngineUIDlg::OnBnClickedBtnEnable()
{
    m_btnEnable.EnableWindow(FALSE);

    // 1. 检查是否勾选了卷
    CArray<int> selectedIndices;
    for (INT_PTR i = 0; i < m_volumes.GetCount(); i++)
    {
        if (m_listVolumes.GetCheck((int)i) == 1)
            selectedIndices.Add((int)i);
    }

    if (selectedIndices.GetCount() == 0)
    {
        SetStatus(L"请至少选择一个要保护的卷", TRUE);
        m_btnEnable.EnableWindow(TRUE);
        return;
    }

    // 2. 提取驱动文件到临时目录
    SetStatus(L"正在准备驱动文件...");
    CString tempDir = ExtractDriverFiles();
    if (tempDir.IsEmpty())
    {
        SetStatus(L"提取驱动文件失败", TRUE);
        m_btnEnable.EnableWindow(TRUE);
        return;
    }

    // 3. 安装驱动
    SetStatus(L"正在安装驱动...");
    CString infPath = tempDir + L"\\SysRestoreDriver.inf";
    if (!InstallDriver(infPath))
    {
        SetStatus(L"驱动安装失败，请确认以管理员身份运行", TRUE);
        m_btnEnable.EnableWindow(TRUE);
        return;
    }

    // 4. 创建保护状态文件
    //    每个选中卷根目录下创建 _qh_protect_state.data, 首字节写 1
    //    驱动重启加载时读取此文件首字节决定是否激活保护
    SetStatus(L"正在创建保护状态文件...");
    if (!CreateProtectStateFiles(selectedIndices))
    {
        SetStatus(L"创建保护状态文件失败", TRUE);
        m_btnEnable.EnableWindow(TRUE);
        return;
    }

    // 5. 配置 bootmgr 启动失败策略
    // 重启还原场景下系统本来就是"坏了重启即修复"，不需要 WinRE 自动启动修复
    // 同时避免 bootmgr 累计启动失败计数后开机直接进 WinRE
    // 失败不阻断保护启用，只给出非致命提示
    SetStatus(L"正在配置启动策略...");
    // BOOL bcdOk1 = RunBcdedit(L"/set {default} bootstatuspolicy ignoreallfailures");
    // BOOL bcdOk2 = RunBcdedit(L"/set {default} recoveryenabled No");
    // if (!bcdOk1 || !bcdOk2)
    // {
    //     AfxMessageBox(
    //         L"启动策略配置失败，保护仍可使用，但开机可能进入 WinRE。\n\n"
    //         L"建议手动以管理员身份执行：\n"
    //         L"  bcdedit /set {default} bootstatuspolicy ignoreallfailures\n"
    //         L"  bcdedit /set {default} recoveryenabled No",
    //         MB_ICONWARNING);
    // }

    // 6. 成功
    SetStatus(L"保护已开启！请重启计算机使保护生效。");
    UpdateUIByProtectStatus();
    AfxMessageBox(L"磁盘保护已成功开启！\n请重启计算机使保护生效。", MB_ICONINFORMATION);
}

void CQHEngineUIDlg::OnBnClickedBtnDisable()
{
    m_btnDisable.EnableWindow(FALSE);

    SetStatus(L"正在关闭保护...");

    if (!DisableProtectByFile())
    {
        SetStatus(L"关闭保护失败，未找到任何卷的状态文件或写入失败", TRUE);
        m_btnDisable.EnableWindow(TRUE);
        return;
    }

    // 恢复 bootmgr 默认启动失败策略
    // 与 OnBnClickedBtnEnable 中的 ignoreallfailures / recoveryenabled No 配对
    // 保护关闭后，系统不再具备"坏了重启即修复"的能力，需要恢复 WinRE 自动修复以避免真出问题时无救
    // 失败不阻断关闭流程，只给非致命提示
    SetStatus(L"正在恢复启动策略...");
    BOOL bcdOk1 = RunBcdedit(L"/set {default} bootstatuspolicy DisplayAllFailures");
    BOOL bcdOk2 = RunBcdedit(L"/set {default} recoveryenabled Yes");
    if (!bcdOk1 || !bcdOk2)
    {
        AfxMessageBox(
            L"启动策略恢复失败，保护已关闭但 BCD 仍处于忽略失败状态。\n\n"
            L"建议手动以管理员身份执行：\n"
            L"  bcdedit /set {default} bootstatuspolicy DisplayAllFailures\n"
            L"  bcdedit /set {default} recoveryenabled Yes",
            MB_ICONWARNING);
    }

    SetStatus(L"保护已关闭！请重启计算机使更改生效。");
    UpdateUIByProtectStatus();
    AfxMessageBox(L"磁盘保护已关闭！\n请重启计算机使更改生效。", MB_ICONINFORMATION);
}

// 隐藏窗口执行 bcdedit，返回退出码是否为 0
// 用 ShellExecuteExW + SW_HIDE 静默执行，避免 CMD 黑框闪烁
static BOOL RunBcdedit(const wchar_t* args)
{
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"open";
    sei.lpFile = L"bcdedit.exe";
    sei.lpParameters = args;
    sei.nShow = SW_HIDE;

    if (!ShellExecuteExW(&sei))
        return FALSE;

    if (!sei.hProcess)
        return TRUE;

    WaitForSingleObject(sei.hProcess, 5000);
    DWORD exitCode = 1;
    GetExitCodeProcess(sei.hProcess, &exitCode);
    CloseHandle(sei.hProcess);
    return exitCode == 0;
}

CString CQHEngineUIDlg::ExtractDriverFiles()
{
    // 创建临时目录
    CString tempDir;
    GetTempPath(MAX_PATH, tempDir.GetBuffer(MAX_PATH));
    tempDir.ReleaseBuffer();
    tempDir += L"SysRestoreDriver";

    CreateDirectoryW(tempDir, NULL);

    // 从 exe 资源中提取驱动文件
    struct { UINT resId; LPCWSTR fileName; } files[] = {
        { IDR_DRIVER_SYS, L"SysRestoreDriver.sys" },
        { IDR_DRIVER_INF, L"SysRestoreDriver.inf" }
    };

    for (int i = 0; i < _countof(files); i++)
    {
        HRSRC hRes = FindResourceW(AfxGetResourceHandle(), MAKEINTRESOURCEW(files[i].resId), RT_RCDATA);
        if (!hRes)
            return CString();

        HGLOBAL hData = LoadResource(AfxGetResourceHandle(), hRes);
        if (!hData)
            return CString();

        DWORD dataSize = SizeofResource(AfxGetResourceHandle(), hRes);
        LPVOID pData = LockResource(hData);
        if (!pData || dataSize == 0)
            return CString();

        CString dstPath;
        dstPath.Format(L"%s\\%s", tempDir, files[i].fileName);

        HANDLE hFile = CreateFileW(dstPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE)
            return CString();

        DWORD written;
        WriteFile(hFile, pData, dataSize, &written, NULL);
        CloseHandle(hFile);

        if (written != dataSize)
            return CString();
    }

    return tempDir;
}

BOOL CQHEngineUIDlg::InstallDriver(const CString& infPath)
{
    // 调用 InstallHinfSectionW 安装驱动
    CString cmdLine;
    cmdLine.Format(L"DefaultInstall 132 %s", infPath);
    InstallHinfSectionW(NULL, NULL, cmdLine, 0);

    // InstallHinfSection 没有返回值，检查服务是否安装成功
    SC_HANDLE hSCM = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!hSCM)
        return FALSE;

    SC_HANDLE hService = OpenServiceW(hSCM, L"SysRestoreDriver", SERVICE_QUERY_STATUS);
    CloseServiceHandle(hSCM);

    if (!hService)
        return FALSE;

    CloseServiceHandle(hService);

    // DefaultInstall 不会处理 AddFilter 指令，需手动注册为卷过滤驱动
    // 写入 Volume 类的 UpperFilters：HKLM\...\Control\Class\{71a27cdd-...}\UpperFilters
    const wchar_t* volumeClassKey = L"SYSTEM\\CurrentControlSet\\Control\\Class\\{71a27cdd-812a-11d0-bec7-08002be2092f}";
    HKEY hKey;
    LONG result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, volumeClassKey, 0, KEY_READ | KEY_SET_VALUE, &hKey);
    if (result != ERROR_SUCCESS)
        return FALSE;

    // 读取现有的 UpperFilters
    wchar_t existing[4096] = { 0 };
    DWORD existingSize = sizeof(existing) - sizeof(wchar_t);
    DWORD type = REG_MULTI_SZ;
    bool alreadyExists = false;
    result = RegQueryValueExW(hKey, L"UpperFilters", NULL, &type, (LPBYTE)existing, &existingSize);

    if (result == ERROR_SUCCESS && type == REG_MULTI_SZ)
    {
        // 检查是否已存在
        const wchar_t* p = existing;
        while (*p)
        {
            if (_wcsicmp(p, L"SysRestoreDriver") == 0)
            {
                alreadyExists = true;
                break;
            }
            p += wcslen(p) + 1;
        }
    }
    else
    {
        // 不存在，初始化为空
        existing[0] = L'\0';
        existing[1] = L'\0';
        existingSize = sizeof(wchar_t) * 2;
    }

    if (!alreadyExists)
    {
        // 追加 SysRestoreDriver
        // 找到末尾（连续两个 \0 之前）
        wchar_t* pEnd = existing;
        while (*pEnd)
            pEnd += wcslen(pEnd) + 1;

        // pEnd 指向最后的空字符串位置，写入 "SysRestoreDriver\0\0"
        wcscpy_s(pEnd, (sizeof(existing) / sizeof(wchar_t)) - (pEnd - existing), L"SysRestoreDriver");
        pEnd += wcslen(L"SysRestoreDriver") + 1;
        *pEnd = L'\0';

        DWORD newSize = (DWORD)((pEnd - existing + 1) * sizeof(wchar_t));
        result = RegSetValueExW(hKey, L"UpperFilters", 0, REG_MULTI_SZ, (const BYTE*)existing, newSize);
        if (result != ERROR_SUCCESS)
        {
            RegCloseKey(hKey);
            return FALSE;
        }
    }

    RegCloseKey(hKey);
    return TRUE;
}

BOOL CQHEngineUIDlg::CreateProtectStateFiles(const CArray<int>& selectedIndices)
{
    // 为每个选中卷在卷根目录创建 _qh_protect_state.data
    //
    // 文件要求:
    //   1. 大小必须远大于 NTFS 驻留属性阈值 (~700B), 否则数据放进 MFT 而不是普通簇,
    //      ProtectBitmap 标记的"文件扇区"集合就会为空, 后续 UI 关闭保护写入会被重定向
    //   2. 写入第一个字节 = 1 表示开启保护
    //   3. 设置 HIDDEN + SYSTEM 属性避免普通用户误删/误改
    //   4. FILE_FLAG_WRITE_THROUGH 立即落盘, 避免缓存导致驱动重启加载时读到旧数据
    //
    // 这里写入 QH_PROTECT_STATE_FILE_SIZE (1MB) 全 0 后, 把首字节改为 1
    for (INT_PTR i = 0; i < selectedIndices.GetCount(); i++)
    {
        int idx = selectedIndices[i];
        const QHVolumeInfo& vol = m_volumes[idx];

        CString filePath;
        filePath.Format(L"%c:%s", vol.DriveLetter, QH_PROTECT_STATE_FILE_NAME);

        // 如果文件已存在并带隐藏/系统属性, 先去掉属性避免 CreateFile 拒绝
        DWORD existingAttr = GetFileAttributesW(filePath);
        if (existingAttr != INVALID_FILE_ATTRIBUTES)
        {
            SetFileAttributesW(filePath, FILE_ATTRIBUTE_NORMAL);
        }

        HANDLE hFile = CreateFileW(filePath,
            GENERIC_WRITE,
            FILE_SHARE_READ,
            NULL,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
            NULL);

        if (hFile == INVALID_HANDLE_VALUE)
        {
            return FALSE;
        }

        // 一次性写 1MB 的 0 用于占位 (强制非驻留)
        const DWORD bufSize = 64 * 1024;
        BYTE* zeroBuf = new BYTE[bufSize];
        ZeroMemory(zeroBuf, bufSize);

        DWORD totalWritten = 0;
        BOOL writeOk = TRUE;
        while (totalWritten < QH_PROTECT_STATE_FILE_SIZE)
        {
            DWORD toWrite = min(bufSize, QH_PROTECT_STATE_FILE_SIZE - totalWritten);
            DWORD written = 0;
            if (!WriteFile(hFile, zeroBuf, toWrite, &written, NULL) || written != toWrite)
            {
                writeOk = FALSE;
                break;
            }
            totalWritten += written;
        }
        delete[] zeroBuf;

        if (!writeOk)
        {
            CloseHandle(hFile);
            DeleteFileW(filePath);
            return FALSE;
        }

        // 回到首字节写 1
        SetFilePointer(hFile, 0, NULL, FILE_BEGIN);
        BYTE state = 1;
        DWORD written = 0;
        if (!WriteFile(hFile, &state, 1, &written, NULL) || written != 1)
        {
            CloseHandle(hFile);
            DeleteFileW(filePath);
            return FALSE;
        }

        FlushFileBuffers(hFile);
        CloseHandle(hFile);

        // 设置隐藏 + 系统属性
        SetFileAttributesW(filePath, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
    }

    return TRUE;
}

BOOL CQHEngineUIDlg::DisableProtectByFile()
{
    // 遍历所有本地固定卷, 对存在保护状态文件的卷把首字节写 0
    //
    // 该文件在驱动初始化时已被加入 ProtectBitmap, 写入会直通真实磁盘
    // 不会被本驱动重定向, 因此重启后驱动能读到首字节 = 0, 不激活保护
    //
    // 这里直接走用户态 WriteFile, 不需要驱动通讯 IOCTL
    BOOL anySuccess = FALSE;

    for (INT_PTR i = 0; i < m_volumes.GetCount(); i++)
    {
        const QHVolumeInfo& vol = m_volumes[i];

        CString filePath;
        filePath.Format(L"%c:%s", vol.DriveLetter, QH_PROTECT_STATE_FILE_NAME);

        // 文件不存在则跳过 (此卷未开启过保护)
        DWORD attr = GetFileAttributesW(filePath);
        if (attr == INVALID_FILE_ATTRIBUTES)
            continue;

        // 临时去掉只读/隐藏/系统属性以便写入
        SetFileAttributesW(filePath, FILE_ATTRIBUTE_NORMAL);

        HANDLE hFile = CreateFileW(filePath,
            GENERIC_WRITE,
            FILE_SHARE_READ,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_WRITE_THROUGH,
            NULL);

        if (hFile == INVALID_HANDLE_VALUE)
        {
            // 恢复属性后继续下一个
            SetFileAttributesW(filePath, attr);
            continue;
        }

        SetFilePointer(hFile, 0, NULL, FILE_BEGIN);
        BYTE state = 0;
        DWORD written = 0;
        BOOL ok = WriteFile(hFile, &state, 1, &written, NULL) && written == 1;
        if (ok)
        {
            FlushFileBuffers(hFile);
            anySuccess = TRUE;
        }
        CloseHandle(hFile);

        // 恢复原属性
        SetFileAttributesW(filePath, attr);
    }

    return anySuccess;
}

// 查询驱动当前是否处于保护状态（任一卷 Initialized == TRUE 即为保护中）
// 驱动未加载（设备打不开）视为未保护，函数仍返回 TRUE 表示查询本身可用
BOOL CQHEngineUIDlg::QueryProtectStatus(BOOL& isProtecting)
{
    isProtecting = FALSE;

    HANDLE hDevice = CreateFileW(
        QH_CONTROL_SYSTEM_LINK_NAME,
        GENERIC_READ,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (hDevice == INVALID_HANDLE_VALUE)
        return TRUE;  // 驱动未加载，等同未保护

    BOOLEAN result = FALSE;
    DWORD bytesReturned = 0;
    BOOL success = DeviceIoControl(
        hDevice,
        IOCTL_QH_QUERY_PROTECT_STATUS,
        NULL,
        0,
        &result,
        sizeof(result),
        &bytesReturned,
        NULL
    );

    CloseHandle(hDevice);

    if (!success)
        return FALSE;

    isProtecting = (result != FALSE);
    return TRUE;
}

// 根据当前保护状态启用/禁用列表和按钮
// 保护中：列表禁用、开启禁用、关闭可用
// 未保护：列表可用、开启可用、关闭禁用
void CQHEngineUIDlg::UpdateUIByProtectStatus()
{
    BOOL isProtecting = FALSE;
    QueryProtectStatus(isProtecting);  // 失败时按未保护处理

    m_listVolumes.EnableWindow(!isProtecting);
    m_btnEnable.EnableWindow(!isProtecting);
    m_btnDisable.EnableWindow(isProtecting);
}

void CQHEngineUIDlg::SetStatus(const CString& text, BOOL isError)
{
    m_statusColor = isError ? RGB(255, 0, 0) : RGB(51, 51, 51);
    m_status.SetWindowTextW(text);
    m_status.Invalidate();
}

HBRUSH CQHEngineUIDlg::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor)
{
    HBRUSH hbr = CDialogEx::OnCtlColor(pDC, pWnd, nCtlColor);

    if (pWnd->GetDlgCtrlID() == IDC_STATUS)
    {
        pDC->SetTextColor(m_statusColor);
        pDC->SetBkMode(TRANSPARENT);
    }

    return hbr;
}

// 点击/回车触发 SysLink 跳转, NM_CLICK 与 NM_RETURN 共用此处理
// 用 ShellExecuteW 调起系统默认浏览器打开吾爱破解论坛
void CQHEngineUIDlg::OnNMClickLink52pojie(NMHDR* pNMHDR, LRESULT* pResult)
{
    PNMLINK pNMLink = (PNMLINK)pNMHDR;
    ShellExecuteW(NULL, L"open", pNMLink->item.szUrl, NULL, NULL, SW_SHOWNORMAL);
    *pResult = 0;
}
