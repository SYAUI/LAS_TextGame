#pragma once
#include "framework.h"
#include "resource.h"
#include "ImageView.h"

class CMainFrame : public CFrameWindowImpl<CMainFrame>,
    public CUpdateUI<CMainFrame>,
    public CIdleHandler
{
public:
    DECLARE_FRAME_WND_CLASS(L"TextGame-ImageView", IDR_MAINFRAME)

    CImageView  m_view;
    CStatusBarCtrl m_statusBar;
    CString     m_curPath;

    // 消息映射
    BEGIN_MSG_MAP(CMainFrame)
        MESSAGE_HANDLER(WM_CREATE, OnCreate)
        MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
        COMMAND_ID_HANDLER(ID_FILE_OPEN, OnFileOpen)
        COMMAND_ID_HANDLER(ID_FILE_EXIT, OnFileExit)
        COMMAND_ID_HANDLER(ID_VIEW_FIT, OnViewFit)
        COMMAND_ID_HANDLER(ID_VIEW_PIXEL_PERFECT, OnViewPixelPerfect)
        COMMAND_ID_HANDLER(ID_VIEW_SEPARATE, OnViewSeparate)
        COMMAND_ID_HANDLER(ID_VIEW_CHAN_R, OnViewChanR)
        COMMAND_ID_HANDLER(ID_VIEW_CHAN_G, OnViewChanG)
        COMMAND_ID_HANDLER(ID_VIEW_CHAN_B, OnViewChanB)
        COMMAND_ID_HANDLER(ID_VIEW_CHAN_A, OnViewChanA)
        CHAIN_MSG_MAP(CUpdateUI<CMainFrame>)
        CHAIN_MSG_MAP(CFrameWindowImpl<CMainFrame>)
    END_MSG_MAP()

    BEGIN_UPDATE_UI_MAP(CMainFrame)
        UPDATE_ELEMENT(ID_VIEW_FIT, UPDUI_MENUPOPUP)
        UPDATE_ELEMENT(ID_VIEW_PIXEL_PERFECT, UPDUI_MENUPOPUP)
        UPDATE_ELEMENT(ID_VIEW_SEPARATE, UPDUI_MENUPOPUP)
        UPDATE_ELEMENT(ID_VIEW_CHAN_R, UPDUI_MENUPOPUP)
        UPDATE_ELEMENT(ID_VIEW_CHAN_G, UPDUI_MENUPOPUP)
        UPDATE_ELEMENT(ID_VIEW_CHAN_B, UPDUI_MENUPOPUP)
        UPDATE_ELEMENT(ID_VIEW_CHAN_A, UPDUI_MENUPOPUP)
    END_UPDATE_UI_MAP()

    // 刷新 UI 状态
    BOOL OnIdle()
    {
        UIUpdateStatusBar();
        return FALSE;
    }

    // 创建
    void SyncMenuChecks()
    {
        UISetCheck(ID_VIEW_FIT, m_view.m_fitToWindow);
        UISetCheck(ID_VIEW_PIXEL_PERFECT, m_view.m_pixelPerfect);
        UISetCheck(ID_VIEW_SEPARATE, m_view.m_separateMode);
        UISetCheck(ID_VIEW_CHAN_R, m_view.m_showChan[0]);
        UISetCheck(ID_VIEW_CHAN_G, m_view.m_showChan[1]);
        UISetCheck(ID_VIEW_CHAN_B, m_view.m_showChan[2]);
        UISetCheck(ID_VIEW_CHAN_A, m_view.m_showChan[3]);
    }
    LRESULT OnCreate(UINT, WPARAM, LPARAM, BOOL&)
    {
        CreateSimpleStatusBar();
        m_statusBar.Attach(m_hWndStatusBar);
        UIAddStatusBar(m_hWndStatusBar);

        int parts[2] = { 200, -1 };
        m_statusBar.SetParts(2, parts);
        m_statusBar.SetText(0, L"就绪");

        // 创建视图窗口
        m_hWndClient = m_view.Create(m_hWnd, rcDefault, nullptr,
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, WS_EX_CLIENTEDGE);

        // 加载完成回调
        m_view.OnImageLoaded = [this]()
            {
                UpdateStatusBar();
                SyncMenuChecks();
            };

        UISetCheck(ID_VIEW_FIT, TRUE);
        UISetCheck(ID_VIEW_PIXEL_PERFECT, FALSE);
        UISetCheck(ID_VIEW_SEPARATE, FALSE);
        UISetCheck(ID_VIEW_CHAN_R, TRUE);
        UISetCheck(ID_VIEW_CHAN_G, TRUE);
        UISetCheck(ID_VIEW_CHAN_B, TRUE);
        UISetCheck(ID_VIEW_CHAN_A, TRUE);

        UpdateLayout();
        return 0;
    }

    void UpdateStatusBar()
    {
        if (m_view.m_imgWidth > 0)
        {
            CString s;
            s.Format(L"%d x %d", m_view.m_imgWidth, m_view.m_imgHeight);
            m_statusBar.SetText(0, s);
        }
        if (!m_curPath.IsEmpty())
            m_statusBar.SetText(1, m_curPath);
    }

    // 命令处理
    LRESULT OnFileOpen(WORD, WORD, HWND, BOOL&)
    {
        // 1. 定义文件类型过滤器
        COMDLG_FILTERSPEC filterSpec[] = {
            { L"图片文件", L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff;*.webp" },
            { L"所有文件", L"*.*" }
        };

        // 2. 创建并初始化对话框
        CShellFileOpenDialog dlg(
            nullptr,                                    // 默认文件名
            FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST, // 选项
            nullptr,                                    // 默认扩展名
            filterSpec,                                 // 过滤器数组
            _countof(filterSpec)                        // 过滤器数量
        );

        // 3. 显示对话框
        if (dlg.DoModal(m_hWnd) == IDOK)
        {
            // 4. 获取用户选择的文件路径
            CString filePath;
            if (SUCCEEDED(dlg.GetFilePath(filePath)))
            {
                OpenImage(filePath);
            }
        }
        return 0;
    }

    void OpenImage(const wchar_t* path)
    {
        HRESULT hr = m_view.LoadImage(path);
        if (SUCCEEDED(hr))
        {
            m_curPath = path;
            UpdateStatusBar();
            SyncMenuChecks();
        }
        else
        {
            ::MessageBoxW(m_hWnd, L"无法加载该图片文件。",
                L"错误", MB_ICONWARNING | MB_OK);
        }
    }

    LRESULT OnFileExit(WORD, WORD, HWND, BOOL&)
    {
        PostMessage(WM_CLOSE);
        return 0;
    }

    LRESULT OnViewFit(WORD, WORD, HWND, BOOL&)
    {
        bool v = !m_view.m_fitToWindow;
        m_view.SetFitToWindow(v);
        UISetCheck(ID_VIEW_FIT, v);
        return 0;
    }

    LRESULT OnViewPixelPerfect(WORD, WORD, HWND, BOOL&)
    {
        bool v = !m_view.m_pixelPerfect;
        m_view.SetPixelPerfect(v);
        UISetCheck(ID_VIEW_PIXEL_PERFECT, v);
        return 0;
    }

    LRESULT OnViewSeparate(WORD, WORD, HWND, BOOL&)
    {
        bool v = !m_view.m_separateMode;
        m_view.SetSeparateMode(v);
        UISetCheck(ID_VIEW_SEPARATE, v);

        // 第一次进入分离模式时按需生成通道 bitmap
        if (v && !m_curPath.IsEmpty())
        {
            bool anyCreated = false;
            for (int i = 0; i < 4; ++i)
                if (m_view.m_chanCreated[i]) { anyCreated = true; break; }

            if (!anyCreated)
                m_view.GenerateChannelBitmaps();
        }
        else if (!v)
        {
            m_view.ReleaseChannelBitmaps();
        }
        return 0;
    }

    LRESULT OnViewChanR(WORD, WORD, HWND, BOOL&)
    {
        bool v = !m_view.m_showChan[0];
        m_view.SetChannelVisible(0, v);
        UISetCheck(ID_VIEW_CHAN_R, v);
        if (v) m_view.GenerateChannelBitmaps();
        return 0;
    }
    LRESULT OnViewChanG(WORD, WORD, HWND, BOOL&)
    {
        bool v = !m_view.m_showChan[1];
        m_view.SetChannelVisible(1, v);
        UISetCheck(ID_VIEW_CHAN_G, v);
        if (v) m_view.GenerateChannelBitmaps();
        return 0;
    }
    LRESULT OnViewChanB(WORD, WORD, HWND, BOOL&)
    {
        bool v = !m_view.m_showChan[2];
        m_view.SetChannelVisible(2, v);
        UISetCheck(ID_VIEW_CHAN_B, v);
        if (v) m_view.GenerateChannelBitmaps();
        return 0;
    }
    LRESULT OnViewChanA(WORD, WORD, HWND, BOOL&)
    {
        bool v = !m_view.m_showChan[3];
        m_view.SetChannelVisible(3, v);
        UISetCheck(ID_VIEW_CHAN_A, v);
        if (v) m_view.GenerateChannelBitmaps();
        return 0;
    }

    LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL&)
    {
        ::PostQuitMessage(0);
        return 0;
    }
};