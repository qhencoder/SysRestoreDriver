#pragma once

#include <afxwin.h>
#include <SetupAPI.h>

#pragma comment(lib, "SetupAPI.lib")

// IOCTL 控制码与通讯设备符号链接名由驱动端头文件统一提供
// 用户态分支自动展开 QH_CONTROL_SYSTEM_LINK_NAME 为 "\\\\.\\QHEngineControlDevice"
#include "QHIoctl.h"

// 卷信息结构
struct QHVolumeInfo
{
    WCHAR   DriveLetter;        // 盘符，如 'C'
    CString DisplayText;        // 显示文本，如 "C: (NTFS)"
};

class CQHEngineUIDlg : public CDialogEx
{
    DECLARE_DYNAMIC(CQHEngineUIDlg)

public:
    CQHEngineUIDlg(CWnd* pParent = nullptr);
    ~CQHEngineUIDlg();

    enum { IDD = IDD_QHENGINEUI_DIALOG };

protected:
    virtual void DoDataExchange(CDataExchange* pDX);

protected:
    HICON m_hIcon;

    // 控件变量
    CCheckListBox m_listVolumes;
    CButton       m_btnEnable;
    CButton       m_btnDisable;
    CStatic       m_status;

    // 数据
    CArray<QHVolumeInfo> m_volumes;
    COLORREF m_statusColor;       // 状态文本颜色

    // 初始化
    virtual BOOL OnInitDialog();

    // 枚举本地固定磁盘卷
    void EnumerateVolumes();

    // 填充卷列表到界面
    void PopulateVolumeList();

    // 从资源提取驱动文件到临时目录，返回临时目录路径
    CString ExtractDriverFiles();

    // 安装 INF 驱动
    BOOL InstallDriver(const CString& infPath);

    // 为选中卷创建保护状态文件 _qh_protect_state.data
    BOOL CreateProtectStateFiles(const CArray<int>& selectedIndices);

    // 关闭所有卷的保护: 把每个卷根目录下的 _qh_protect_state.data 首字节改写为 0
    // 该文件已被驱动加入 ProtectBitmap, 写入会直通真实磁盘不被重定向
    BOOL DisableProtectByFile();

    // 查询驱动保护状态：成功返回 TRUE，isProtecting 写出结果；失败返回 FALSE
    BOOL QueryProtectStatus(BOOL& isProtecting);

    // 根据保护状态更新按钮和列表的启用/禁用
    void UpdateUIByProtectStatus();

    // 设置状态文本
    void SetStatus(const CString& text, BOOL isError = FALSE);

    // 消息处理
    afx_msg void OnBnClickedBtnEnable();
    afx_msg void OnBnClickedBtnDisable();
    afx_msg HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor);
    afx_msg void OnNMClickLink52pojie(NMHDR* pNMHDR, LRESULT* pResult);

    DECLARE_MESSAGE_MAP()
};
