#pragma once
#include "framework.h"
#include "resource.h"
#include "ImgFormatLoader.h"
#include <vector>
#include <functional>
#include <cmath>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"


class CImageView : public CWindowImpl<CImageView>
{
public:
    DECLARE_WND_CLASS(L"ImageViewWTL")

    // 状态
    CComPtr<ID2D1Factory>           m_pD2DFactory;
    CComPtr<ID2D1HwndRenderTarget>  m_pRenderTarget;

    // 主图
    CComPtr<ID2D1Bitmap> m_pBitmap;
    int m_imgWidth = 0;
    int m_imgHeight = 0;

    // 通道位图
    CComPtr<ID2D1Bitmap> m_pChanBitmap[4];
    bool m_chanCreated[4] = { false, false, false, false };

    // 缓存解码后的 BGRA 像素（与 D2D 原生顺序一致，避免通道交换）
    std::vector<unsigned char> m_rgbaCache;

    // 视图
    double m_zoom = 1.0;
    D2D1_POINT_2F m_pan = { 0.0f, 0.0f };
    bool m_fitToWindow = true;
    bool m_pixelPerfect = false;

    bool m_showChan[4] = { true, true, true, true };
    bool m_separateMode = false;

    // 交互
    bool  m_dragging = false;
    POINT m_lastMouse = {};

    // 回调
    std::function<void()> OnImageLoaded;

    BEGIN_MSG_MAP(CImageView)
        MESSAGE_HANDLER(WM_CREATE, OnCreate)
        MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
        MESSAGE_HANDLER(WM_PAINT, OnPaint)
        MESSAGE_HANDLER(WM_SIZE, OnSize)
        MESSAGE_HANDLER(WM_ERASEBKGND, OnEraseBackground)
        MESSAGE_HANDLER(WM_MOUSEWHEEL, OnMouseWheel)
        MESSAGE_HANDLER(WM_LBUTTONDOWN, OnLButtonDown)
        MESSAGE_HANDLER(WM_LBUTTONUP, OnLButtonUp)
        MESSAGE_HANDLER(WM_MOUSEMOVE, OnMouseMove)
        MESSAGE_HANDLER(WM_LBUTTONDBLCLK, OnLButtonDblClk)
        MESSAGE_HANDLER(WM_DROPFILES, OnDropFiles)
    END_MSG_MAP()

    // 初始化
    LRESULT OnCreate(UINT, WPARAM, LPARAM, BOOL&)
    {
        D2D1_FACTORY_OPTIONS opts = {};
#ifdef _DEBUG
        opts.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
        ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory), &opts, (void**)&m_pD2DFactory);

        CreateRenderTarget();
        ::DragAcceptFiles(m_hWnd, TRUE);
        return 0;
    }

    void CreateRenderTarget()
    {
        if (!m_pD2DFactory) return;
        RECT rc; GetClientRect(&rc);
        D2D1_SIZE_U size = D2D1::SizeU(
            (((1) > (rc.right - rc.left)) ? (1) : (rc.right - rc.left)),
            max(1, rc.bottom - rc.top));

        m_pRenderTarget.Release();
        m_pD2DFactory->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(m_hWnd, size),
            &m_pRenderTarget);
    }

    // 图像加载
    HRESULT LoadImage(const wchar_t* filePath)
    {
        if (!m_pRenderTarget) return E_FAIL;

        // 分支：引擎专有格式图片tex
        if (img::IsTexFile(filePath))
        {
            img::TexImage tex;
            if (!img::LoadTexFile(filePath, tex)) return E_FAIL;

            if (m_rgbaCache.capacity() > tex.pixels.size() * 4)
                std::vector<unsigned char>().swap(m_rgbaCache);
            m_rgbaCache = std::move(tex.pixels);

            m_pBitmap.Release();
            HRESULT hr = CreateD2DBitmapFromBGRA(
                m_pRenderTarget, m_rgbaCache.data(),
                tex.width, tex.height, &m_pBitmap);
            if (FAILED(hr)) return hr;

            m_imgWidth = tex.width;
            m_imgHeight = tex.height;

            ReleaseChannelBitmaps();
            m_fitToWindow = true;
            m_pan = { 0.0f, 0.0f };
            m_separateMode = false;

            if (OnImageLoaded) OnImageLoaded();
            Invalidate(FALSE);
            return S_OK;
        }
        // 分支结束


        // 1. 读文件
        std::vector<unsigned char> buf;
        if (!ReadFileToBuffer(filePath, buf)) return E_FAIL;

        // 2. stb 解码到 RGBA
        int w = 0, h = 0, comp = 0;
        unsigned char* pixels = stbi_load_from_memory(
            buf.data(), (int)buf.size(), &w, &h, &comp, 4);
        if (!pixels) return E_FAIL;

        // 3. 缓存 RGBA（用于后续通道分离，避免重复解码）
        const size_t newSize = (size_t)w * h * 4;
        if (m_rgbaCache.capacity() > newSize * 4)
            std::vector<unsigned char>().swap(m_rgbaCache);
        m_rgbaCache.resize(newSize);
        for (size_t i = 0; i < (size_t)w * h; ++i) {
            m_rgbaCache[i * 4 + 0] = pixels[i * 4 + 2];   // B ← R
            m_rgbaCache[i * 4 + 1] = pixels[i * 4 + 1];   // G
            m_rgbaCache[i * 4 + 2] = pixels[i * 4 + 0];   // R ← B
            m_rgbaCache[i * 4 + 3] = pixels[i * 4 + 3];   // A
        }
        stbi_image_free(pixels);

        // 4. 创建 D2D 主位图
        m_pBitmap.Release();
        HRESULT hr = CreateD2DBitmapFromBGRA(
            m_pRenderTarget, m_rgbaCache.data(), w, h, &m_pBitmap);
        if (FAILED(hr)) return hr;

        m_imgWidth = w;
        m_imgHeight = h;

        // 5. 释放旧通道位图
        ReleaseChannelBitmaps();

        // 6. 重置视图
        m_fitToWindow = true;
        m_pan = { 0.0f, 0.0f };
        m_separateMode = false;

        if (OnImageLoaded) OnImageLoaded();
        Invalidate(FALSE);
        return S_OK;
    }

    // 从 BGRA 原始像素创建 D2D 位图
    static HRESULT CreateD2DBitmapFromBGRA(
        ID2D1RenderTarget* pRT,
        const unsigned char* bgra, int w, int h,
        ID2D1Bitmap** out)
    {
        if (!pRT || !bgra || w <= 0 || h <= 0 || !out) return E_INVALIDARG;

        const UINT stride = (UINT)w * 4;
        std::vector<BYTE> premul((size_t)stride * h);

        for (int y = 0; y < h; ++y)
        {
            const unsigned char* src = bgra + (size_t)y * w * 4;
            BYTE* dst = premul.data() + (size_t)y * stride;

            for (int x = 0; x < w; ++x, src += 4, dst += 4)
            {
                const BYTE b = src[0];
                const BYTE g = src[1];
                const BYTE r = src[2];
                const BYTE a = src[3];

                if (a == 255) {
                    dst[0] = b; dst[1] = g; dst[2] = r; dst[3] = 255;
                    continue;
                }
                dst[0] = (BYTE)((b * a + 127) / 255);
                dst[1] = (BYTE)((g * a + 127) / 255);
                dst[2] = (BYTE)((r * a + 127) / 255);
                dst[3] = a;
            }
        }

        D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_PREMULTIPLIED));

        return pRT->CreateBitmap(
            D2D1::SizeU((UINT32)w, (UINT32)h),
            premul.data(), stride, &props, out);
    }
    
    // 生成通道位图
    HRESULT GenerateChannelBitmaps()
    {
        if (m_rgbaCache.empty() || m_imgWidth <= 0 || m_imgHeight <= 0)
            return E_FAIL;
        if (!m_pRenderTarget) return E_FAIL;

        const int  w = m_imgWidth, h = m_imgHeight;
        const UINT dstStride = (UINT)w * 4;

        // 1. 需要生成通道
        bool needGen[4] = { false, false, false, false };
        int  needCount = 0;
        for (int ch = 0; ch < 4; ++ch)
        {
            needGen[ch] = m_showChan[ch]
                && !(m_chanCreated[ch] && m_pChanBitmap[ch]);
            if (needGen[ch]) ++needCount;
        }
        if (needCount == 0)
        {
            Invalidate(FALSE);
            return S_OK;
        }

        // 分配灰度缓冲
        std::vector<BYTE> buffers[4];
        for (int ch = 0; ch < 4; ++ch)
            if (needGen[ch])
                buffers[ch].resize((size_t)dstStride * h);

        // 单次遍历 RGBA 缓存
        BYTE* dstPtr[4] = { nullptr, nullptr, nullptr, nullptr };
        for (int ch = 0; ch < 4; ++ch)
            if (needGen[ch]) dstPtr[ch] = buffers[ch].data();

        const size_t pixelCount = (size_t)w * h;
        const unsigned char* src = m_rgbaCache.data();

        for (size_t i = 0; i < pixelCount; ++i, src += 4)
        {
            if (needGen[0]) {  // R
                BYTE* d = dstPtr[0] + i * 4;
                BYTE v = src[2]; d[0] = d[1] = d[2] = v; d[3] = 255;
            }
            if (needGen[1]) {  // G
                BYTE* d = dstPtr[1] + i * 4;
                BYTE v = src[1]; d[0] = d[1] = d[2] = v; d[3] = 255;
            }
            if (needGen[2]) {  // B
                BYTE* d = dstPtr[2] + i * 4;
                BYTE v = src[0]; d[0] = d[1] = d[2] = v; d[3] = 255;
            }
            if (needGen[3]) {  // A
                BYTE* d = dstPtr[3] + i * 4;
                BYTE v = src[3]; d[0] = d[1] = d[2] = v; d[3] = 255;
            }
        }

        // 为每个已填好的缓冲创建 D2D 位图
        D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_PREMULTIPLIED));

        for (int ch = 0; ch < 4; ++ch)
        {
            if (!needGen[ch]) continue;

            m_pChanBitmap[ch].Release();
            HRESULT hr = m_pRenderTarget->CreateBitmap(
                D2D1::SizeU((UINT32)w, (UINT32)h),
                buffers[ch].data(),
                dstStride,
                &props,
                &m_pChanBitmap[ch]);

            m_chanCreated[ch] = SUCCEEDED(hr);
        }

        Invalidate(FALSE);
        return S_OK;
    }

    void ReleaseChannelBitmaps()
    {
        for (int i = 0; i < 4; ++i)
        {
            m_pChanBitmap[i].Release();
            m_chanCreated[i] = false;
        }
    }

    // 读文件到内存
    static bool ReadFileToBuffer(const wchar_t* path,
        std::vector<unsigned char>& out)
    {
        HANDLE hFile = ::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return false;

        LARGE_INTEGER size = {};
        if (!::GetFileSizeEx(hFile, &size) || size.QuadPart <= 0 ||
            size.QuadPart > 512ll * 1024 * 1024)
        {
            ::CloseHandle(hFile);
            return false;
        }

        out.resize((size_t)size.QuadPart);
        DWORD read = 0;
        BOOL ok = ::ReadFile(hFile, out.data(),
            (DWORD)out.size(), &read, nullptr);
        ::CloseHandle(hFile);
        return ok && read == (DWORD)out.size();
    }

    // 渲染
    LRESULT OnPaint(UINT, WPARAM, LPARAM, BOOL&)
    {
        PAINTSTRUCT ps;
        BeginPaint(&ps);
        if (m_pRenderTarget) Render();
        EndPaint(&ps);
        return 0;
    }

    void Render()
    {
        m_pRenderTarget->BeginDraw();
        m_pRenderTarget->Clear(D2D1::ColorF(0.10f, 0.10f, 0.11f));

        D2D1_SIZE_F rt = m_pRenderTarget->GetSize();

        if (!m_separateMode)
        {
            DrawBitmapCentered(m_pBitmap, rt);
        }
        else
        {
            std::vector<int> actives;
            for (int i = 0; i < 4; ++i)
                if (m_showChan[i] && m_chanCreated[i]) actives.push_back(i);

            if (!actives.empty())
            {
                const float gap = 8.0f;
                float cellW = (rt.width - gap * (actives.size() - 1))
                    / (float)actives.size();

                for (size_t i = 0; i < actives.size(); ++i)
                {
                    D2D1_RECT_F cell = D2D1::RectF(
                        i * (cellW + gap), 0,
                        i * (cellW + gap) + cellW, rt.height);
                    DrawBitmapInRect(m_pChanBitmap[actives[i]], cell);
                }
            }
        }

        m_pRenderTarget->EndDraw();
    }

    void DrawBitmapCentered(ID2D1Bitmap* pBmp, D2D1_SIZE_F rt)
    {
        if (!pBmp) return;
        D2D1_SIZE_F bmpSz = pBmp->GetSize();

        if (m_fitToWindow)
        {
            double sx = rt.width / bmpSz.width;
            double sy = rt.height / bmpSz.height;
            m_zoom = (sx < sy) ? sx : sy;
            if (m_pixelPerfect) m_zoom = SnapZoom(m_zoom);
        }

        float drawW = (float)(bmpSz.width * m_zoom);
        float drawH = (float)(bmpSz.height * m_zoom);
        float left = (rt.width - drawW) * 0.5f + m_pan.x;
        float top = (rt.height - drawH) * 0.5f + m_pan.y;

        D2D1_RECT_F dest = D2D1::RectF(left, top, left + drawW, top + drawH);
        m_pRenderTarget->DrawBitmap(
            pBmp, dest, 1.0f,
            m_pixelPerfect
            ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR
            : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    void DrawBitmapInRect(ID2D1Bitmap* pBmp, D2D1_RECT_F cell)
    {
        if (!pBmp) return;
        D2D1_SIZE_F sz = pBmp->GetSize();

        double scale = (cell.right - cell.left) / sz.width;
        double sy = (cell.bottom - cell.top) / sz.height;
        if (sy < scale) scale = sy;
        if (m_pixelPerfect) scale = SnapZoom(scale);

        float w = (float)(sz.width * scale);
        float h = (float)(sz.height * scale);
        float cx = (cell.left + cell.right) * 0.5f;
        float cy = (cell.top + cell.bottom) * 0.5f;

        D2D1_RECT_F dest = D2D1::RectF(
            cx - w * 0.5f, cy - h * 0.5f,
            cx + w * 0.5f, cy + h * 0.5f);

        m_pRenderTarget->DrawBitmap(
            pBmp, dest, 1.0f,
            m_pixelPerfect
            ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR
            : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    // 视图辅助
    double ComputeFitZoomFor(ID2D1Bitmap* pBmp) const
    {
        if (!pBmp || !m_pRenderTarget) return 1.0;
        D2D1_SIZE_F rt = m_pRenderTarget->GetSize();
        D2D1_SIZE_F bs = pBmp->GetSize();
        if (bs.width <= 0.0f || bs.height <= 0.0f) return 1.0;
        double sx = rt.width / bs.width;
        double sy = rt.height / bs.height;
        double z = (sx < sy) ? sx : sy;
        if (m_pixelPerfect) z = SnapZoom(z);
        return z;
    }

    double ComputeFitZoom() const { return ComputeFitZoomFor(m_pBitmap); }

    static double NextZoomUp(double z)
    {
        if (z >= 1.0)
            return floor(z + 1e-9) + 1.0; 

        // z 在 (0,1) 区间，形式为 1/n
        double n = floor(1.0 / z + 1e-9);
        if (n <= 2.0) return 1.0; 
        return 1.0 / (n - 1.0); 
    }

    static double NextZoomDown(double z)
    {
        if (z > 1.0)
            return ceil(z - 1e-9) - 1.0;     

        // z = 1/n，向下就是 1/(n+1)
        double n = ceil(1.0 / z - 1e-9);
        return 1.0 / (n + 1.0);                     
    }

    static double SnapZoom(double z)
    {
        if (z >= 1.0) return floor(z);
        return 1.0 / ceil(1.0 / z);
    }

    // 交互
    LRESULT OnMouseWheel(UINT, WPARAM wParam, LPARAM lParam, BOOL&)
    {
        if (!m_pBitmap || !m_pRenderTarget) return 0;

        short delta = GET_WHEEL_DELTA_WPARAM(wParam);
        if (delta == 0) return 0;

        if (m_fitToWindow)
        {
            D2D1_SIZE_F rt = m_pRenderTarget->GetSize();
            D2D1_SIZE_F bs = m_pBitmap->GetSize();
            double sx = rt.width / bs.width;
            double sy = rt.height / bs.height;
            double fitZoom = (sx < sy) ? sx : sy;
            if (m_pixelPerfect) fitZoom = SnapZoom(fitZoom);

            m_zoom = fitZoom;
            m_pan = { 0.0f, 0.0f };
            m_fitToWindow = false;
        }

        // 光标位置
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(&pt);

        const double oldZoom = m_zoom;
        double factor = pow(1.15, delta / (double)WHEEL_DELTA);
        double newZoom = oldZoom * factor;

        if (newZoom < 0.02)  newZoom = 0.02;
        if (newZoom > 128.0) newZoom = 128.0;

        if (m_pixelPerfect)
        {
            double snapped = SnapZoom(newZoom);
            // 吸附后如果没变化，强制朝滚轮方向走一档
            if (delta > 0 && snapped <= oldZoom + 1e-9)
                snapped = NextZoomUp(oldZoom);
            else if (delta < 0 && snapped >= oldZoom - 1e-9)
                snapped = NextZoomDown(oldZoom);

            newZoom = snapped;
            if (newZoom < 0.02)  newZoom = 0.02;
            if (newZoom > 128.0) newZoom = 128.0;
        }

        if (oldZoom > 0.0)
        {
            D2D1_SIZE_F rt = m_pRenderTarget->GetSize();
            const float cx = rt.width * 0.5f;
            const float cy = rt.height * 0.5f;
            const double k = newZoom / oldZoom;

            m_pan.x = (float)((pt.x - cx) * (1.0 - k) + m_pan.x * k);
            m_pan.y = (float)((pt.y - cy) * (1.0 - k) + m_pan.y * k);
        }

        m_zoom = newZoom;
        Invalidate(FALSE);
        return 0;
    }

    LRESULT OnLButtonDown(UINT, WPARAM, LPARAM lParam, BOOL&)
    {
        m_dragging = true;
        m_lastMouse.x = GET_X_LPARAM(lParam);
        m_lastMouse.y = GET_Y_LPARAM(lParam);
        SetCapture();
        return 0;
    }

    LRESULT OnMouseMove(UINT, WPARAM, LPARAM lParam, BOOL&)
    {
        if (!m_dragging) return 0;
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);
        m_pan.x += (float)(x - m_lastMouse.x);
        m_pan.y += (float)(y - m_lastMouse.y);
        m_lastMouse.x = x;
        m_lastMouse.y = y;
        m_fitToWindow = false;
        Invalidate(FALSE);
        return 0;
    }

    LRESULT OnLButtonUp(UINT, WPARAM, LPARAM, BOOL&)
    {
        if (m_dragging) { m_dragging = false; ReleaseCapture(); }
        return 0;
    }

    LRESULT OnLButtonDblClk(UINT, WPARAM, LPARAM, BOOL&)
    {
        m_fitToWindow = true;
        m_pan = { 0.0f, 0.0f };
        Invalidate(FALSE);
        return 0;
    }

    LRESULT OnSize(UINT, WPARAM, LPARAM lParam, BOOL&)
    {
        if (m_pRenderTarget)
            m_pRenderTarget->Resize(D2D1::SizeU(
                LOWORD(lParam), HIWORD(lParam)));
        Invalidate(FALSE);
        return 0;
    }

    LRESULT OnEraseBackground(UINT, WPARAM, LPARAM, BOOL&) { return 1; }

    LRESULT OnDropFiles(UINT, WPARAM wParam, LPARAM, BOOL&)
    {
        HDROP hDrop = (HDROP)wParam;
        wchar_t path[MAX_PATH] = {};
        if (::DragQueryFileW(hDrop, 0, path, MAX_PATH) > 0)
            LoadImage(path);
        ::DragFinish(hDrop);
        return 0;
    }

    LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL&)
    {
        ReleaseChannelBitmaps();
        m_pBitmap.Release();
        m_pRenderTarget.Release();
        m_pD2DFactory.Release();
        m_rgbaCache.clear();
        m_rgbaCache.shrink_to_fit();
        return 0;
    }

    // ══════════ 外部接口 ══════════
    void SetFitToWindow(bool v) { m_fitToWindow = v; Invalidate(FALSE); }
    void SetPixelPerfect(bool v) { m_pixelPerfect = v; Invalidate(FALSE); }
    void SetSeparateMode(bool v) { m_separateMode = v; Invalidate(FALSE); }
    void SetChannelVisible(int i, bool v) { m_showChan[i] = v; Invalidate(FALSE); }
};