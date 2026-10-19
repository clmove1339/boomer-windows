// boomer for Windows: zoom into a screenshot of the desktop, pixelated.
// Drag = pan, wheel = zoom, F = flashlight, Ctrl+wheel = flashlight size,
// 0 = reset, Q/Esc = quit.
// `boomer --daemon` stays in the background and pops up instantly on Win+Shift+Z.
// `--live` keeps the image updating, but hides boomer from screen sharing.
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <math.h>
#include <string.h>
#include <assert.h>

static int sw, sh, sx, sy;           // virtual screen size/origin
static HDC hdcImg, hdcDim, hdcBack;  // screenshot, darkened screenshot, back buffer
static DWORD *img, *dim;
static int daemon, live;
static int dirty0, dirty1;            // rows of img changed since dim was last updated
static double dim_shade;             // shade the dim buffer was computed for
static double scale = 1, target = 1; // current and animated-to zoom
static double vx, vy;                // image point shown at window (0,0)
static int mx, my;                   // cursor in client coords
static int dragging, flashlight, closing; // closing = animating back to 1:1 before hiding
static double shade;                 // flashlight darkness, animated 0..1 towards `flashlight`
static double radius = 200;

// One Desktop Duplication per monitor; works for fullscreen games where GDI sees black.
typedef struct {
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    IDXGIOutputDuplication *dup;
    ID3D11Texture2D *stage;
    int x, y, fresh;
} Dup;
static Dup dups[16];
static int ndups, dup_lost, retry;
// D3D device per GPU, kept across popups: creating one takes ~170ms, the rest ~10ms.
static struct { LUID luid; ID3D11Device *dev; ID3D11DeviceContext *ctx; } devs[8];
static int ndevs;

static int get_device(IDXGIAdapter1 *a, ID3D11Device **dev, ID3D11DeviceContext **ctx) {
    DXGI_ADAPTER_DESC1 ad;
    int i;
    IDXGIAdapter1_GetDesc1(a, &ad);
    for (i = 0; i < ndevs; i++)
        if (!memcmp(&devs[i].luid, &ad.AdapterLuid, sizeof(LUID))) break;
    if (i < ndevs && ID3D11Device_GetDeviceRemovedReason(devs[i].dev) != S_OK) { // driver reset
        ID3D11DeviceContext_Release(devs[i].ctx);
        ID3D11Device_Release(devs[i].dev);
        devs[i] = devs[--ndevs];
        i = ndevs;
    }
    if (i == ndevs) {
        if (ndevs == 8 || FAILED(D3D11CreateDevice((IDXGIAdapter *)a, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0,
                                                   D3D11_SDK_VERSION, &devs[i].dev, NULL, &devs[i].ctx)))
            return 0;
        devs[i].luid = ad.AdapterLuid;
        ndevs++;
    }
    *dev = devs[i].dev;
    *ctx = devs[i].ctx;
    return 1;
}

static HDC make_dib(int w, int h, DWORD **bits) {
    BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB}};
    HDC dc = CreateCompatibleDC(NULL);
    *bits = NULL;
    SelectObject(dc, CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)bits, NULL, 0));
    return dc;
}

static void free_dib(HDC dc) {
    HGDIOBJ b = GetCurrentObject(dc, OBJ_BITMAP);
    DeleteDC(dc);
    DeleteObject(b);
}

static void free_bufs(void) {
    if (hdcImg) { free_dib(hdcImg); free_dib(hdcDim); free_dib(hdcBack); }
    hdcImg = NULL;
    sw = sh = 0;
}

// Follow the current monitor layout: resize buffers and window if it changed.
// Returns 0 if the screen buffers could not be allocated.
static int fit_screen(HWND w) {
    int cw = GetSystemMetrics(SM_CXVIRTUALSCREEN), ch = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    DWORD *back;
    if (cw != sw || ch != sh) {
        free_bufs();
        sw = cw; sh = ch;
        hdcImg = make_dib(sw, sh, &img);
        hdcDim = make_dib(sw, sh, &dim);
        hdcBack = make_dib(sw, sh, &back);
        if (!img || !dim || !back) { free_bufs(); return 0; }
        SetStretchBltMode(hdcBack, COLORONCOLOR); // nearest neighbour = pixelated
    }
    sx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    sy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    SetWindowPos(w, HWND_TOPMOST, sx, sy, sw, sh, SWP_NOACTIVATE);
    return 1;
}

static void close_dups(void) {
    for (int i = 0; i < ndups; i++) {
        if (dups[i].stage) ID3D11Texture2D_Release(dups[i].stage);
        IDXGIOutputDuplication_Release(dups[i].dup);
        ID3D11DeviceContext_Release(dups[i].ctx);
        ID3D11Device_Release(dups[i].dev);
    }
    ndups = 0;
}

static void open_dups(void) {
    IDXGIFactory1 *f;
    IDXGIAdapter1 *a;
    close_dups();
    dup_lost = 0;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f))) return;
    for (UINT i = 0; IDXGIFactory1_EnumAdapters1(f, i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        ID3D11Device *dev;
        ID3D11DeviceContext *ctx;
        IDXGIOutput *o;
        if (get_device(a, &dev, &ctx)) {
            for (UINT j = 0; ndups < 16 && IDXGIAdapter1_EnumOutputs(a, j, &o) != DXGI_ERROR_NOT_FOUND; j++) {
                DXGI_OUTPUT_DESC od;
                IDXGIOutput1 *o1;
                IDXGIOutput_GetDesc(o, &od);
                // shortcut: rotated monitors are left to the GDI capture; rotate the frame if anyone needs it.
                if (od.AttachedToDesktop && od.Rotation <= DXGI_MODE_ROTATION_IDENTITY &&
                    SUCCEEDED(IDXGIOutput_QueryInterface(o, &IID_IDXGIOutput1, (void **)&o1))) {
                    Dup *d = &dups[ndups];
                    HRESULT hr = IDXGIOutput1_DuplicateOutput(o1, (IUnknown *)dev, &d->dup);
                    if (hr == E_ACCESSDENIED) dup_lost = 1; // secure desktop (lock screen, UAC): retry later
                    if (SUCCEEDED(hr)) {
                        d->dev = dev; ID3D11Device_AddRef(dev);
                        d->ctx = ctx; ID3D11DeviceContext_AddRef(ctx);
                        d->stage = NULL;
                        d->x = od.DesktopCoordinates.left;
                        d->y = od.DesktopCoordinates.top;
                        d->fresh = 1;
                        ndups++;
                    }
                    IDXGIOutput1_Release(o1);
                }
                IDXGIOutput_Release(o);
            }
        }
        IDXGIAdapter1_Release(a);
    }
    IDXGIFactory1_Release(f);
}

// Returns 1 if any pixel changed: our own repaints also produce frames, so skip identical ones.
static int copy_frame(Dup *d, ID3D11Texture2D *tex) {
    int changed = 0;
    D3D11_TEXTURE2D_DESC td;
    D3D11_MAPPED_SUBRESOURCE map;
    int x0 = d->x - sx, y0 = d->y - sy, w, h;
    ID3D11Texture2D_GetDesc(tex, &td);
    // Clamp: the frame can be a different size than the desktop mid mode-switch, or lie
    // outside our buffer if a monitor was added while open (layout is re-read on next popup).
    w = min((int)td.Width, sw - x0);
    h = min((int)td.Height, sh - y0);
    if (td.Format != DXGI_FORMAT_B8G8R8A8_UNORM || x0 < 0 || y0 < 0 || w <= 0 || h <= 0) return 0;
    if (!d->stage) {
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        td.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(d->dev, &td, NULL, &d->stage))) return 0;
    }
    ID3D11DeviceContext_CopyResource(d->ctx, (ID3D11Resource *)d->stage, (ID3D11Resource *)tex);
    if (FAILED(ID3D11DeviceContext_Map(d->ctx, (ID3D11Resource *)d->stage, 0, D3D11_MAP_READ, 0, &map))) return 0;
    for (int y = 0; y < h; y++) {
        DWORD *dst = img + (size_t)(y0 + y) * sw + x0;
        BYTE *src = (BYTE *)map.pData + (size_t)y * map.RowPitch;
        if (memcmp(dst, src, (size_t)w * 4)) {
            memcpy(dst, src, (size_t)w * 4);
            changed = 1;
            dirty0 = min(dirty0, y0 + y);
            dirty1 = max(dirty1, y0 + y + 1);
        }
    }
    ID3D11DeviceContext_Unmap(d->ctx, (ID3D11Resource *)d->stage, 0);
    return changed;
}

// Pull new frames into img; returns 1 if the picture changed.
static int grab(UINT timeout) {
    int changed = 0;
    for (int i = 0; i < ndups; i++) {
        Dup *d = &dups[i];
        DXGI_OUTDUPL_FRAME_INFO fi;
        IDXGIResource *res;
        ID3D11Texture2D *tex;
        HRESULT hr = IDXGIOutputDuplication_AcquireNextFrame(d->dup, timeout, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (FAILED(hr)) { dup_lost = 1; continue; } // mode switch, UAC, lock screen...
        // 0 = no new image (cursor-only update, or the black first frame of a new duplication)
        if (fi.LastPresentTime.QuadPart &&
            SUCCEEDED(IDXGIResource_QueryInterface(res, &IID_ID3D11Texture2D, (void **)&tex))) {
            GdiFlush();
            changed |= copy_frame(d, tex);
            ID3D11Texture2D_Release(tex);
            d->fresh = 0;
        }
        IDXGIResource_Release(res);
        IDXGIOutputDuplication_ReleaseFrame(d->dup);
    }
    return changed;
}

static int waiting(void) {
    for (int i = 0; i < ndups; i++)
        if (dups[i].fresh) return 1;
    return 0;
}

// Map the visible part of the image to integer source/dest rects so pixels stay crisp.
static int view_rect(int imgw, int winw, double v, double s, int *src0, int *srcw, int *dst0, int *dstw) {
    int a = (int)floor(v), b = (int)ceil(v + winw / s);
    if (a < 0) a = 0;
    if (b > imgw) b = imgw;
    if (b <= a) return 0;
    *src0 = a; *srcw = b - a;
    *dst0 = (int)lround((a - v) * s);
    *dstw = (int)lround((b - v) * s) - *dst0;
    return 1;
}

static void blit(HDC dst, HDC src) {
    int x, w, dx, dw, y, h, dy, dh;
    if (view_rect(sw, sw, vx, scale, &x, &w, &dx, &dw) &&
        view_rect(sh, sh, vy, scale, &y, &h, &dy, &dh))
        StretchBlt(dst, dx, dy, dw, dh, src, x, y, w, h, SRCCOPY);
}

// Zoom to s keeping the image point under the cursor fixed.
static void zoom_to(double s) {
    double ix = vx + mx / scale, iy = vy + my / scale;
    scale = s;
    vx = ix - mx / scale;
    vy = iy - my / scale;
}

static void popup(HWND w) {
    HDC screen;
    if (!fit_screen(w)) {
        MessageBoxW(NULL, L"Not enough memory for the screen buffers", L"boomer", MB_ICONERROR);
        if (!daemon) DestroyWindow(w);
        return;
    }
    // GDI first as a fallback for monitors DXGI can't duplicate, then DXGI on top.
    screen = GetDC(NULL);
    BitBlt(hdcImg, 0, 0, sw, sh, screen, sx, sy, SRCCOPY | CAPTUREBLT);
    ReleaseDC(NULL, screen);
    open_dups();
    for (int t = 0; t < 10 && waiting(); t++) grab(25); // first real frame from every monitor
    if (!live) close_dups();
    flashlight = 0;
    shade = 0;
    dirty0 = 0;
    dirty1 = sh;
    scale = target = 1;
    vx = vy = 0;
    dragging = closing = 0;
    InvalidateRect(w, NULL, FALSE);
    SetTimer(w, 1, 15, NULL);
    ShowWindow(w, SW_SHOW);
    SetForegroundWindow(w);
}

static void hide(HWND w) {
    close_dups();
    if (!daemon) { DestroyWindow(w); return; }
    KillTimer(w, 1);
    ShowWindow(w, SW_HIDE);
    // Don't hold three screen-sized buffers while idle; fit_screen reallocates on popup.
    free_bufs();
}

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_HOTKEY:
        if (!IsWindowVisible(w)) popup(w);
        return 0;
    case WM_MOUSEWHEEL: {
        if (closing) return 0;
        POINT p = {(short)LOWORD(lp), (short)HIWORD(lp)};
        double f = pow(1.1, (short)HIWORD(wp) / (double)WHEEL_DELTA);
        ScreenToClient(w, &p);
        mx = p.x; my = p.y;
        if (flashlight && (LOWORD(wp) & MK_CONTROL)) {
            radius *= f;
            if (radius < 10) radius = 10;
            if (radius > sw + sh) radius = sw + sh; // bigger covers the screen anyway; keeps (int)radius valid
        } else {
            target *= f;
            if (target < 0.1) target = 0.1;
            if (target > 200) target = 200;
        }
        InvalidateRect(w, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (closing) return 0;
        dragging = 1;
        SetCapture(w);
        return 0;
    case WM_LBUTTONUP:
        dragging = 0;
        ReleaseCapture();
        return 0;
    case WM_MOUSEMOVE: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        if (!(wp & MK_LBUTTON)) dragging = 0; // button released outside (Alt+Tab mid-drag)
        if (dragging) { vx -= (x - mx) / scale; vy -= (y - my) / scale; }
        mx = x; my = y;
        if (dragging || shade > 0) InvalidateRect(w, NULL, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        if (lp & 0x40000000) return 0; // auto-repeat of a held key
        if (wp == VK_ESCAPE || wp == 'Q') { // first press animates out, a second one skips it
            if (closing) hide(w);
            closing = 1;
            flashlight = dragging = 0;
            ReleaseCapture();
            return 0;
        }
        if (closing) return 0;
        if (wp == '0' || wp == VK_NUMPAD0) { scale = target = 1; vx = vy = 0; }
        if (wp == 'F') flashlight = !flashlight;
        InvalidateRect(w, NULL, FALSE);
        return 0;
    case WM_TIMER:
        if (live && dup_lost && ++retry % 30 == 0) open_dups();
        if (grab(0)) InvalidateRect(w, NULL, FALSE);
        if (fabs(flashlight - shade) > 1e-3) { // flashlight fade in/out
            shade += (flashlight - shade) * 0.25;
            if (fabs(flashlight - shade) <= 1e-3) shade = flashlight;
            InvalidateRect(w, NULL, FALSE);
        }
        if (closing) { // ease view back to 1:1, then hide
            // Scale about the one screen point that is in the same place zoomed and at 1:1,
            // so the view shrinks back into place instead of sliding: vx = c * (1 - 1/scale).
            double s = scale + (1 - scale) * 0.3;
            vx *= 0.7 * scale / s;
            vy *= 0.7 * scale / s;
            scale = s;
            if (fabs(scale - 1) < 5e-3 && fabs(vx) < 1 && fabs(vy) < 1 && shade < 0.02) hide(w);
            InvalidateRect(w, NULL, FALSE);
            return 0;
        }
        if (fabs(target - scale) > 1e-4 * target) { // smooth zoom animation
            zoom_to(fabs(target - scale) < 1e-3 * target ? target : scale + (target - scale) * 0.3);
            InvalidateRect(w, NULL, FALSE);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        PatBlt(hdcBack, 0, 0, sw, sh, BLACKNESS);
        if (shade == 0) blit(hdcBack, hdcImg);
        if (shade > 0) { // darkened copy everywhere, then the normal image inside the circle
            HRGN r = CreateEllipticRgn(mx - (int)radius, my - (int)radius, mx + (int)radius, my + (int)radius);
            // Recompute all rows while fading, otherwise only rows the live capture changed.
            int a = dim_shade != shade ? 0 : dirty0, b = dim_shade != shade ? sh : dirty1;
            DWORD k = (DWORD)(256 - shade * 192); // 256 = 100%, 64 = 25% brightness
            for (size_t i = (size_t)a * sw; i < (size_t)b * sw; i++)
                dim[i] = ((img[i] & 0xff00ff) * k >> 8 & 0xff00ff) | ((img[i] & 0x00ff00) * k >> 8 & 0x00ff00);
            dirty0 = sh;
            dirty1 = 0;
            dim_shade = shade;
            blit(hdcBack, hdcDim);
            SelectClipRgn(hdcBack, r);
            blit(hdcBack, hdcImg);
            SelectClipRgn(hdcBack, NULL);
            DeleteObject(r);
        }
        BitBlt(dc, 0, 0, sw, sh, hdcBack, 0, 0, SRCCOPY);
        EndPaint(w, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

static void self_test(void) {
    int s, sw_, d, dw;
    assert(view_rect(100, 100, 0, 1, &s, &sw_, &d, &dw) && s == 0 && sw_ == 100 && d == 0 && dw == 100);
    assert(view_rect(100, 100, 10.5, 2, &s, &sw_, &d, &dw) && s == 10 && sw_ == 51 && d == -1 && dw == 102);
    assert(view_rect(100, 100, -50, 1, &s, &sw_, &d, &dw) && s == 0 && sw_ == 50 && d == 50);
    assert(!view_rect(100, 100, 200, 1, &s, &sw_, &d, &dw));
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prevInst, LPSTR cmd, int show) {
    MSG msg;
    WNDCLASSW wc = {0};
    HWND w;

    self_test();
    daemon = strstr(cmd, "--daemon") != NULL;
    live = strstr(cmd, "--live") != NULL;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); // capture real pixels, not scaled

    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"boomer";
    RegisterClassW(&wc);
    w = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"boomer", L"boomer", WS_POPUP,
                        0, 0, 1, 1, NULL, NULL, inst, NULL);
    // Live capture must not see our own window; this also hides boomer from screen sharing.
    if (live) SetWindowDisplayAffinity(w, WDA_EXCLUDEFROMCAPTURE);
    if (daemon) {
        if (!RegisterHotKey(w, 1, MOD_WIN | MOD_SHIFT | MOD_NOREPEAT, 'Z')) {
            MessageBoxW(NULL, L"Win+Shift+Z is taken (boomer already running?)", L"boomer", MB_ICONERROR);
            return 1;
        }
        open_dups(); // warm up the GPU devices so the first popup is instant
        close_dups();
    } else {
        popup(w);
    }
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
