#include "stdafx.h"
#include "QHEngineUI.h"
#include "QHEngineUIDlg.h"
#include <commctrl.h>

#pragma comment(lib, "Comctl32.lib")

CQHEngineUIApp theApp;

CQHEngineUIApp::CQHEngineUIApp()
{
}

BOOL CQHEngineUIApp::InitInstance()
{
    CWinApp::InitInstance();

    // Register SysLink (ICC_LINK_CLASS) - required for dialogs containing
    // SysLink control. Without this, DoModal silently fails to create the
    // unregistered window class and the program exits with no UI.
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icc.dwICC = ICC_LINK_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    // 创建主对话框
    CQHEngineUIDlg dlg;
    m_pMainWnd = &dlg;
    dlg.DoModal();

    return FALSE;
}
