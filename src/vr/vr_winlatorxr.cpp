// winsock2.h must precede any header that pulls in Windows.h.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "vr/vr_winlatorxr.h"

#include "qcommon/qcommon.h"
#include "win32/win_crash_diagnostics.h"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#pragma comment(lib, "ws2_32.lib")

namespace kisak::vr::winlatorxr
{
namespace
{

constexpr const char* kExchangeDirectory = "Z:\\tmp\\xr";
constexpr const char* kVersionPath = "Z:\\tmp\\xr\\version";
constexpr const char* kVrMarkerPath = "Z:\\tmp\\xr\\vr";
constexpr const char* kSystemPath = "Z:\\tmp\\xr\\system";
constexpr const char* kDefaultApiVersion = "0.5";

constexpr std::size_t kRenderFrameSyncHistoryCount = 16u;

struct RenderFrameSync
{
    bool valid = false;
    std::uint32_t renderFrameId = 0u;
    int sync = 0;
};

std::atomic<bool> g_running{false};
std::atomic<bool> g_stopRequested{false};
std::atomic<bool> g_wsaStarted{false};
std::thread g_receiveThread;
SOCKET g_receiveSocket = INVALID_SOCKET;
SOCKET g_sendSocket = INVALID_SOCKET;
std::uint16_t g_boundPort = 0u;

std::mutex g_packetMutex;
std::condition_variable g_packetCondition;
Packet g_latestPacket;
bool g_latestPacketValid = false;
std::uint64_t g_packetCount = 0u;
std::uint64_t g_rejectedPacketCount = 0u;
std::string g_lastRejectedPacket;

std::mutex g_sendMutex;
SystemInfo g_systemInfo;

std::mutex g_syncMutex;
std::array<RenderFrameSync, kRenderFrameSyncHistoryCount>
    g_renderFrameSyncs = {};
std::size_t g_renderFrameSyncWriteIndex = 0u;

bool g_loggedStampFailure = false;
bool g_loggedFirstStamp = false;
bool g_loggedScreenFailure = false;
bool g_loggedFirstScreen = false;

std::mutex g_screenMutex;
VirtualScreen g_virtualScreen;
ScopeLens g_scopeLens;

bool DirectoryExists(const char* path)
{
    const DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0u;
}

bool WriteTextFile(const char* path, const char* text)
{
    FILE* file = nullptr;
    if (fopen_s(&file, path, "wb") != 0 || file == nullptr)
    {
        return false;
    }

    const std::size_t length = std::strlen(text);
    const bool written =
        std::fwrite(text, 1u, length, file) == length;
    std::fclose(file);
    return written;
}

std::string ReadTextFile(const char* path)
{
    std::string text;
    FILE* file = nullptr;
    if (fopen_s(&file, path, "rb") != 0 || file == nullptr)
    {
        return text;
    }

    std::array<char, 512> buffer = {};
    std::size_t read = 0u;
    while ((read = std::fread(buffer.data(), 1u, buffer.size(), file)) > 0u &&
           text.size() < 4096u)
    {
        text.append(buffer.data(), read);
    }

    std::fclose(file);
    return text;
}

const char* RequestedApiVersion()
{
    const char* requested = std::getenv("KISAK_VR_WINLATORXR_API_VERSION");
    return requested != nullptr && requested[0] != '\0'
        ? requested
        : kDefaultApiVersion;
}

bool BindReceiveSocket(const std::uint16_t port)
{
    SOCKET candidate = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (candidate == INVALID_SOCKET)
    {
        return false;
    }

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

    if (bind(
            candidate,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) == SOCKET_ERROR)
    {
        closesocket(candidate);
        return false;
    }

    // A short timeout lets Stop() join the receiver promptly.
    const DWORD timeoutMilliseconds = 250u;
    setsockopt(
        candidate,
        SOL_SOCKET,
        SO_RCVTIMEO,
        reinterpret_cast<const char*>(&timeoutMilliseconds),
        sizeof(timeoutMilliseconds));

    g_receiveSocket = candidate;
    g_boundPort = port;
    return true;
}

void ReceiveLoop()
{
    // Runs outside the game's threads; it must not call Com_Printf.
    std::array<char, 2048> buffer = {};

    while (!g_stopRequested.load(std::memory_order_acquire))
    {
        const int received = recvfrom(
            g_receiveSocket,
            buffer.data(),
            static_cast<int>(buffer.size() - 1u),
            0,
            nullptr,
            nullptr);

        if (received <= 0)
        {
            continue;
        }

        const std::string_view text(
            buffer.data(),
            static_cast<std::size_t>(received));

        Packet packet;
        if (!ParsePacket(text, &packet))
        {
            std::lock_guard<std::mutex> lock(g_packetMutex);
            ++g_rejectedPacketCount;
            g_lastRejectedPacket.assign(
                text.substr(0u, (std::min)(text.size(), std::size_t{200u})));
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(g_packetMutex);
            g_latestPacket = std::move(packet);
            g_latestPacketValid = true;
            ++g_packetCount;
        }

        g_packetCondition.notify_all();
    }
}

struct ScreenVertex
{
    float x;
    float y;
    float z;
    float rhw;
    float u;
    float v;
};

// Copies the screen's source rectangle into a texture, clears the window,
// and draws that texture as a perspective-correct quad in each eye. The
// temporary texture is released immediately so it never blocks a legacy
// IDirect3DDevice9::Reset(); render targets and all device state are
// restored so the renderer's own state cache stays valid.
namespace
{

// The scope camera's image, copied out of the window before the eye views
// repaint that area. Kept across frames so a dropped capture shows the
// previous image rather than nothing.
struct ScopePanelCapture
{
    IDirect3DTexture9* texture = nullptr;
    int size = 0;
    bool valid = false;
};

ScopePanelCapture g_scopePanel;

// An optic is round; the captured panel is square. The alpha comes from a
// mask built once: opaque inside the lens with a short falloff at the rim so
// the edge is not a staircase.
IDirect3DTexture9* g_scopeLensMask = nullptr;

IDirect3DTexture9* EnsureScopeLensMask(IDirect3DDevice9* const device)
{
    constexpr UINT kMaskSize = 256u;

    if (g_scopeLensMask != nullptr)
    {
        return g_scopeLensMask;
    }

    if (FAILED(device->CreateTexture(
            kMaskSize, kMaskSize, 1u, 0u,
            D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
            &g_scopeLensMask, nullptr)))
    {
        g_scopeLensMask = nullptr;
        return nullptr;
    }

    D3DLOCKED_RECT locked = {};
    if (FAILED(g_scopeLensMask->LockRect(0u, &locked, nullptr, 0u)))
    {
        g_scopeLensMask->Release();
        g_scopeLensMask = nullptr;
        return nullptr;
    }

    constexpr float kEdge = 0.02f;
    const float center = 0.5f * static_cast<float>(kMaskSize - 1u);

    for (UINT y = 0u; y < kMaskSize; ++y)
    {
        auto* const row = reinterpret_cast<std::uint32_t*>(
            static_cast<std::uint8_t*>(locked.pBits) +
            static_cast<std::size_t>(y) * locked.Pitch);

        for (UINT x = 0u; x < kMaskSize; ++x)
        {
            const float dx = (static_cast<float>(x) - center) / center;
            const float dy = (static_cast<float>(y) - center) / center;
            const float radius = std::sqrt(dx * dx + dy * dy);

            float coverage = (1.0f - radius) / kEdge;
            coverage = coverage < 0.0f ? 0.0f : (coverage > 1.0f ? 1.0f : coverage);

            row[x] =
                (static_cast<std::uint32_t>(coverage * 255.0f + 0.5f) << 24) |
                0x00FFFFFFu;
        }
    }

    g_scopeLensMask->UnlockRect(0u);
    return g_scopeLensMask;
}

} // namespace


// Winlator's public builds create the game window with a title bar and place
// it at an offset -- this session logged vid_xpos -6, vid_ypos 32. The
// HMD_SYNC pixel is stamped at back-buffer (0,0), but WinlatorXR samples
// X-screen (0,0), so with an offset window it never sees the stamp and keeps
// presenting the frame flat: the side-by-side pair then shows as two copies.
//
// Strip the frame and pin the window to the screen, every frame. It is
// idempotent and only acts on drift, because the window can be re-styled at
// any device reset.
void EnsureBorderlessWindow(IDirect3DDevice9* const device)
{
    static HWND cachedWindow = nullptr;
    static bool loggedFix = false;

    HWND window = cachedWindow;
    if (window == nullptr)
    {
        D3DDEVICE_CREATION_PARAMETERS parameters = {};
        if (FAILED(device->GetCreationParameters(&parameters)))
        {
            return;
        }
        window = parameters.hFocusWindow;
        if (window == nullptr)
        {
            return;
        }
        cachedWindow = window;
    }

    const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    if (screenWidth <= 0 || screenHeight <= 0)
    {
        return;
    }

    const LONG_PTR style = GetWindowLongPtrA(window, GWL_STYLE);
    const LONG_PTR wantedStyle =
        (style & ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME |
                   WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) |
        WS_POPUP;

    const LONG_PTR exStyle = GetWindowLongPtrA(window, GWL_EXSTYLE);
    const LONG_PTR wantedExStyle =
        exStyle & ~(WS_EX_CLIENTEDGE | WS_EX_WINDOWEDGE |
                    WS_EX_DLGMODALFRAME | WS_EX_STATICEDGE);

    RECT bounds = {};
    const bool haveBounds = GetWindowRect(window, &bounds) != FALSE;
    const bool placed =
        haveBounds &&
        bounds.left == 0 &&
        bounds.top == 0 &&
        (bounds.right - bounds.left) == screenWidth &&
        (bounds.bottom - bounds.top) == screenHeight;

    if (placed && wantedStyle == style && wantedExStyle == exStyle)
    {
        return;
    }

    if (wantedStyle != style)
    {
        SetWindowLongPtrA(window, GWL_STYLE, wantedStyle);
    }
    if (wantedExStyle != exStyle)
    {
        SetWindowLongPtrA(window, GWL_EXSTYLE, wantedExStyle);
    }

    SetWindowPos(
        window,
        HWND_TOP,
        0,
        0,
        screenWidth,
        screenHeight,
        SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);

    if (!loggedFix)
    {
        loggedFix = true;
        Com_Printf(
            0,
            "[VR][WINLATORXR] Pinned the game window borderless at 0,0 "
            "%dx%d so the HMD_SYNC pixel lands on screen (0,0).\n",
            screenWidth,
            screenHeight);
    }
}

HRESULT PresentVirtualScreen(
    IDirect3DDevice9* const device,
    IDirect3DSurface9* const backBuffer,
    const VirtualScreen& screen)
{
    D3DSURFACE_DESC description = {};
    HRESULT hr = backBuffer->GetDesc(&description);
    if (FAILED(hr))
    {
        return hr;
    }

    const float windowWidth = static_cast<float>(description.Width);
    const float windowHeight = static_cast<float>(description.Height);
    const float eyeWidth = windowWidth * 0.5f;

    const RECT sourceRect = {
        static_cast<LONG>(screen.sourceLeft * windowWidth),
        static_cast<LONG>(screen.sourceTop * windowHeight),
        static_cast<LONG>(screen.sourceRight * windowWidth),
        static_cast<LONG>(screen.sourceBottom * windowHeight),
    };
    const LONG sourceWidth = sourceRect.right - sourceRect.left;
    const LONG sourceHeight = sourceRect.bottom - sourceRect.top;
    if (sourceWidth < 16 || sourceHeight < 16)
    {
        return S_OK;
    }

    IDirect3DTexture9* screenTexture = nullptr;
    hr = device->CreateTexture(
        static_cast<UINT>(sourceWidth),
        static_cast<UINT>(sourceHeight),
        1u,
        D3DUSAGE_RENDERTARGET,
        description.Format,
        D3DPOOL_DEFAULT,
        &screenTexture,
        nullptr);
    if (FAILED(hr))
    {
        return hr;
    }

    IDirect3DSurface9* screenSurface = nullptr;
    hr = screenTexture->GetSurfaceLevel(0u, &screenSurface);
    if (SUCCEEDED(hr))
    {
        hr = device->StretchRect(
            backBuffer,
            &sourceRect,
            screenSurface,
            nullptr,
            D3DTEXF_NONE);
    }

    if (SUCCEEDED(hr))
    {
        hr = device->ColorFill(
            backBuffer,
            nullptr,
            D3DCOLOR_XRGB(0, 0, 0));
    }

    IDirect3DStateBlock9* savedState = nullptr;
    IDirect3DSurface9* savedTarget = nullptr;
    IDirect3DSurface9* savedDepth = nullptr;

    if (SUCCEEDED(hr))
    {
        hr = device->CreateStateBlock(D3DSBT_ALL, &savedState);
    }

    if (SUCCEEDED(hr))
    {
        device->GetRenderTarget(0u, &savedTarget);
        device->GetDepthStencilSurface(&savedDepth);

        device->SetRenderTarget(0u, backBuffer);
        device->SetDepthStencilSurface(nullptr);

        const D3DVIEWPORT9 viewport = {
            0u,
            0u,
            description.Width,
            description.Height,
            0.0f,
            1.0f,
        };
        device->SetViewport(&viewport);

        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        device->SetTexture(0u, screenTexture);
        device->SetTextureStageState(0u, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0u, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0u, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        device->SetTextureStageState(0u, D3DTSS_TEXCOORDINDEX, 0u);
        device->SetTextureStageState(0u, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        device->SetTextureStageState(1u, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetSamplerState(0u, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0u, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0u, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(0u, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0u, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0u, D3DSAMP_SRGBTEXTURE, FALSE);
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xFu);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);

        // Present runs after the frame's EndScene; open a scene only when
        // the renderer has none open.
        const bool ownScene = SUCCEEDED(device->BeginScene());

        constexpr std::size_t kCells = VirtualScreen::kGridCells;
        constexpr std::size_t kRow = kCells + 1u;
        std::array<ScreenVertex, kCells * kCells * 6u> vertices = {};

        for (std::size_t eye = 0u; eye < 2u && SUCCEEDED(hr); ++eye)
        {
            const float eyeLeft = static_cast<float>(eye) * eyeWidth;
            const auto& grid = screen.grid[eye];

            std::size_t vertexCount = 0u;
            for (std::size_t row = 0u; row < kCells; ++row)
            {
                for (std::size_t column = 0u; column < kCells; ++column)
                {
                    const std::size_t cell[4] = {
                        row * kRow + column,
                        row * kRow + column + 1u,
                        (row + 1u) * kRow + column + 1u,
                        (row + 1u) * kRow + column,
                    };

                    bool behind = false;
                    bool allLeft = true;
                    bool allRight = true;
                    bool allAbove = true;
                    bool allBelow = true;
                    for (const std::size_t point : cell)
                    {
                        behind = behind || !(grid[point][2] > 0.0f);
                        allLeft = allLeft && grid[point][0] < 0.0f;
                        allRight = allRight && grid[point][0] > 1.0f;
                        allAbove = allAbove && grid[point][1] < 0.0f;
                        allBelow = allBelow && grid[point][1] > 1.0f;
                    }

                    if (behind || allLeft || allRight || allAbove || allBelow)
                    {
                        continue;
                    }

                    static const std::size_t kTriangles[6] = {0u, 1u, 2u, 0u, 2u, 3u};
                    for (const std::size_t corner : kTriangles)
                    {
                        const std::size_t point = cell[corner];
                        ScreenVertex& vertex = vertices[vertexCount++];
                        vertex.x = eyeLeft + grid[point][0] * eyeWidth - 0.5f;
                        vertex.y = grid[point][1] * windowHeight - 0.5f;
                        vertex.z = 0.5f;
                        vertex.rhw = grid[point][2];
                        vertex.u = static_cast<float>(point % kRow) / kCells;
                        vertex.v = static_cast<float>(point / kRow) / kCells;
                    }
                }
            }

            if (vertexCount == 0u)
            {
                continue;
            }

            const RECT scissor = {
                static_cast<LONG>(eyeLeft),
                0,
                static_cast<LONG>(eyeLeft + eyeWidth),
                static_cast<LONG>(windowHeight),
            };
            device->SetScissorRect(&scissor);

            hr = device->DrawPrimitiveUP(
                D3DPT_TRIANGLELIST,
                static_cast<UINT>(vertexCount / 3u),
                vertices.data(),
                sizeof(ScreenVertex));
        }

        if (ownScene)
        {
            device->EndScene();
        }

        device->SetTexture(0u, nullptr);
        device->SetRenderTarget(0u, savedTarget);
        device->SetDepthStencilSurface(savedDepth);
        savedState->Apply();
    }

    if (savedDepth != nullptr)
    {
        savedDepth->Release();
    }
    if (savedTarget != nullptr)
    {
        savedTarget->Release();
    }
    if (savedState != nullptr)
    {
        savedState->Release();
    }
    if (screenSurface != nullptr)
    {
        screenSurface->Release();
    }
    screenTexture->Release();
    return hr;
}


// Draws the captured scope panel on the optic. This is PresentVirtualScreen's
// sibling: same pre-transformed grid, but it composites over the world instead
// of replacing it, and it samples the panel captured earlier in the frame.
//
// The two should be folded together once this has been confirmed on hardware.
HRESULT PresentScopeLens(
    IDirect3DDevice9* const device,
    IDirect3DSurface9* const backBuffer,
    const ScopeLens& lens)
{
    if (g_scopePanel.texture == nullptr || !g_scopePanel.valid)
    {
        return S_OK;
    }

    D3DSURFACE_DESC description = {};
    HRESULT hr = backBuffer->GetDesc(&description);
    if (FAILED(hr))
    {
        return hr;
    }

    const float windowHeight = static_cast<float>(description.Height);
    const float eyeWidth = static_cast<float>(description.Width) * 0.5f;

    IDirect3DStateBlock9* savedState = nullptr;
    IDirect3DSurface9* savedTarget = nullptr;
    IDirect3DSurface9* savedDepth = nullptr;

    hr = device->CreateStateBlock(D3DSBT_ALL, &savedState);

    if (SUCCEEDED(hr))
    {
        device->GetRenderTarget(0u, &savedTarget);
        device->GetDepthStencilSurface(&savedDepth);

        device->SetRenderTarget(0u, backBuffer);
        device->SetDepthStencilSurface(nullptr);

        const D3DVIEWPORT9 viewport = {
            0u,
            0u,
            description.Width,
            description.Height,
            0.0f,
            1.0f,
        };
        device->SetViewport(&viewport);

        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        IDirect3DTexture9* const mask = EnsureScopeLensMask(device);

        device->SetTexture(0u, g_scopePanel.texture);
        device->SetTextureStageState(0u, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0u, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0u, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        device->SetTextureStageState(0u, D3DTSS_TEXCOORDINDEX, 0u);
        device->SetTextureStageState(
            0u, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        if (mask != nullptr)
        {
            // Colour stays the panel's; alpha comes from the mask, sampled
            // with the same coordinates.
            device->SetTexture(1u, mask);
            device->SetTextureStageState(1u, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(1u, D3DTSS_COLORARG1, D3DTA_CURRENT);
            device->SetTextureStageState(1u, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(1u, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            device->SetTextureStageState(1u, D3DTSS_TEXCOORDINDEX, 0u);
            device->SetTextureStageState(
                1u, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
            device->SetTextureStageState(2u, D3DTSS_COLOROP, D3DTOP_DISABLE);
            device->SetSamplerState(1u, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            device->SetSamplerState(1u, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            device->SetSamplerState(1u, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            device->SetSamplerState(1u, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            device->SetSamplerState(1u, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        }
        else
        {
            device->SetTextureStageState(1u, D3DTSS_COLOROP, D3DTOP_DISABLE);
        }
        device->SetSamplerState(0u, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0u, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0u, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(0u, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0u, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0u, D3DSAMP_SRGBTEXTURE, FALSE);
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(
            D3DRS_ALPHABLENDENABLE, mask != nullptr ? TRUE : FALSE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xFu);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);

        const bool ownScene = SUCCEEDED(device->BeginScene());

        constexpr std::size_t kCells = VirtualScreen::kGridCells;
        constexpr std::size_t kRow = kCells + 1u;
        std::array<ScreenVertex, kCells * kCells * 6u> vertices = {};

        for (std::size_t eye = 0u; eye < 2u && SUCCEEDED(hr); ++eye)
        {
            const float eyeLeft = static_cast<float>(eye) * eyeWidth;
            const auto& grid = lens.grid[eye];

            std::size_t vertexCount = 0u;
            for (std::size_t row = 0u; row < kCells; ++row)
            {
                for (std::size_t column = 0u; column < kCells; ++column)
                {
                    const std::size_t cell[4] = {
                        row * kRow + column,
                        row * kRow + column + 1u,
                        (row + 1u) * kRow + column + 1u,
                        (row + 1u) * kRow + column,
                    };

                    bool behind = false;
                    bool allLeft = true;
                    bool allRight = true;
                    bool allAbove = true;
                    bool allBelow = true;
                    for (const std::size_t point : cell)
                    {
                        behind = behind || !(grid[point][2] > 0.0f);
                        allLeft = allLeft && grid[point][0] < 0.0f;
                        allRight = allRight && grid[point][0] > 1.0f;
                        allAbove = allAbove && grid[point][1] < 0.0f;
                        allBelow = allBelow && grid[point][1] > 1.0f;
                    }

                    if (behind || allLeft || allRight || allAbove || allBelow)
                    {
                        continue;
                    }

                    static const std::size_t kTriangles[6] =
                        {0u, 1u, 2u, 0u, 2u, 3u};
                    for (const std::size_t corner : kTriangles)
                    {
                        const std::size_t point = cell[corner];
                        ScreenVertex& vertex = vertices[vertexCount++];
                        vertex.x = eyeLeft + grid[point][0] * eyeWidth - 0.5f;
                        vertex.y = grid[point][1] * windowHeight - 0.5f;
                        vertex.z = 0.5f;
                        vertex.rhw = grid[point][2];
                        vertex.u = static_cast<float>(point % kRow) / kCells;
                        vertex.v = static_cast<float>(point / kRow) / kCells;
                    }
                }
            }

            if (vertexCount == 0u)
            {
                continue;
            }

            const RECT scissor = {
                static_cast<LONG>(eyeLeft),
                0,
                static_cast<LONG>(eyeLeft + eyeWidth),
                static_cast<LONG>(windowHeight),
            };
            device->SetScissorRect(&scissor);

            hr = device->DrawPrimitiveUP(
                D3DPT_TRIANGLELIST,
                static_cast<UINT>(vertexCount / 3u),
                vertices.data(),
                sizeof(ScreenVertex));
        }

        if (ownScene)
        {
            device->EndScene();
        }

        device->SetTexture(0u, nullptr);
        device->SetTexture(1u, nullptr);
        device->SetRenderTarget(0u, savedTarget);
        device->SetDepthStencilSurface(savedDepth);
        savedState->Apply();
    }

    if (savedDepth != nullptr)
    {
        savedDepth->Release();
    }
    if (savedTarget != nullptr)
    {
        savedTarget->Release();
    }
    if (savedState != nullptr)
    {
        savedState->Release();
    }
    return hr;
}

} // namespace

bool IsContainerDetected()
{
    // WinlatorXR writes the system file itself; a bare folder is not enough.
    const DWORD attributes = GetFileAttributesA(kSystemPath);
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0u;
}

// KISAK_SP_VR_PRESENT_WATCHDOG
// Some launches reach the menu and then never present a frame, with every
// thread asleep and nothing submitted to the GPU -- a deadlock rather than a
// spin, and in none of the handshake waits that are already instrumented.
// Count presents and, when they stop, say so along with the last stage the
// engine published, so a frozen run names where it got to.
std::atomic<unsigned int> g_presentCount{0u};
std::atomic<bool> g_watchdogStarted{false};

void PresentWatchdogMain()
{
    unsigned int lastCount = 0u;
    int quietSeconds = 0;
    bool reported = false;

    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::seconds(2));

        const unsigned int now =
            g_presentCount.load(std::memory_order_relaxed);

        if (now != lastCount)
        {
            lastCount = now;
            quietSeconds = 0;
            reported = false;
            continue;
        }

        quietSeconds += 2;

        if (quietSeconds >= 6 && !reported)
        {
            Com_Printf(
                0,
                "[WATCHDOG] No frame presented for %i s; %u presented so far. "
                "Last stage reached: '%s'.\n",
                quietSeconds,
                now,
                KisakCrash_GetStage());

            reported = true;
        }
    }
}

void StartPresentWatchdog()
{
    bool expected = false;
    if (g_watchdogStarted.compare_exchange_strong(expected, true))
    {
        std::thread(PresentWatchdogMain).detach();
    }
}

bool Start(std::string* const error)
{
    if (g_running.load(std::memory_order_acquire))
    {
        return true;
    }

    StartPresentWatchdog();

    const auto fail = [error](const char* message)
    {
        if (error != nullptr)
        {
            *error = message;
        }
        return false;
    };

    if (!DirectoryExists(kExchangeDirectory) &&
        !CreateDirectoryA(kExchangeDirectory, nullptr))
    {
        return fail(
            "Z:\\tmp\\xr is unavailable. Start the game from a "
            "WinlatorXR container with the XR API enabled.");
    }

    g_systemInfo = ParseSystemInfo(ReadTextFile(kSystemPath));

    // Winsock 2.2. q_shared.h undefines MAKEWORD.
    WSADATA wsaData = {};
    if (WSAStartup(0x0202u, &wsaData) != 0)
    {
        return fail("WSAStartup failed.");
    }
    g_wsaStarted.store(true, std::memory_order_release);

    if (!BindReceiveSocket(kIncomingPort) &&
        !BindReceiveSocket(kIncomingFallbackPort))
    {
        Stop();
        return fail(
            "Could not bind UDP 127.0.0.1:7872 or :7873 for XrAPI "
            "tracking data.");
    }

    g_sendSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sendSocket == INVALID_SOCKET)
    {
        Stop();
        return fail("Could not create the XrAPI state socket.");
    }

    const char* apiVersion = RequestedApiVersion();
    const bool wroteVersion = WriteTextFile(kVersionPath, apiVersion);
    const bool wroteMarker = WriteTextFile(kVrMarkerPath, "VR");

    if (!wroteVersion)
    {
        Stop();
        return fail("Could not write Z:\\tmp\\xr\\version.");
    }

    {
        std::lock_guard<std::mutex> lock(g_packetMutex);
        g_latestPacket = {};
        g_latestPacketValid = false;
        g_packetCount = 0u;
        g_rejectedPacketCount = 0u;
        g_lastRejectedPacket.clear();
    }

    {
        std::lock_guard<std::mutex> lock(g_syncMutex);
        g_renderFrameSyncs = {};
        g_renderFrameSyncWriteIndex = 0u;
    }

    g_loggedStampFailure = false;
    g_loggedFirstStamp = false;
    g_loggedScreenFailure = false;
    g_loggedFirstScreen = false;
    {
        std::lock_guard<std::mutex> lock(g_screenMutex);
        g_virtualScreen = {};
        g_scopeLens = {};
    }
    g_stopRequested.store(false, std::memory_order_release);
    g_receiveThread = std::thread(ReceiveLoop);
    g_running.store(true, std::memory_order_release);

    Com_Printf(
        0,
        "[VR][WINLATORXR] XrAPI %s advertised%s; listening on "
        "127.0.0.1:%u. Headset '%s' '%s', virtual screen %dx%d.\n",
        apiVersion,
        wroteMarker ? "" : " (vr marker not written)",
        static_cast<unsigned int>(g_boundPort),
        g_systemInfo.manufacturer.c_str(),
        g_systemInfo.product.c_str(),
        g_systemInfo.screenWidth,
        g_systemInfo.screenHeight);

    return true;
}

void Stop()
{
    const bool wasRunning =
        g_running.exchange(false, std::memory_order_acq_rel);

    g_stopRequested.store(true, std::memory_order_release);

    if (g_receiveThread.joinable())
    {
        g_receiveThread.join();
    }

    if (wasRunning)
    {
        // Return WinlatorXR to its normal flat presentation.
        StatePacket state;
        state.vrMode = VrMode::Disabled;
        state.stereoMode = StereoMode::Flat;
        SendState(state);
        DeleteFileA(kVrMarkerPath);
    }

    if (g_receiveSocket != INVALID_SOCKET)
    {
        closesocket(g_receiveSocket);
        g_receiveSocket = INVALID_SOCKET;
    }

    {
        std::lock_guard<std::mutex> lock(g_sendMutex);
        if (g_sendSocket != INVALID_SOCKET)
        {
            closesocket(g_sendSocket);
            g_sendSocket = INVALID_SOCKET;
        }
    }

    if (g_wsaStarted.exchange(false, std::memory_order_acq_rel))
    {
        WSACleanup();
    }

    g_boundPort = 0u;
}

bool IsRunning()
{
    return g_running.load(std::memory_order_acquire);
}

const SystemInfo& GetSystemInfo()
{
    return g_systemInfo;
}

bool WaitForPacket(
    const int lastSync,
    const unsigned int timeoutMilliseconds,
    Packet* const packet,
    bool* const fresh)
{
    if (packet == nullptr)
    {
        return false;
    }

    std::unique_lock<std::mutex> lock(g_packetMutex);

    const bool arrived = g_packetCondition.wait_for(
        lock,
        std::chrono::milliseconds(timeoutMilliseconds),
        [lastSync]()
        {
            return g_latestPacketValid &&
                g_latestPacket.sync != lastSync;
        });

    if (fresh != nullptr)
    {
        *fresh = arrived;
    }

    if (!g_latestPacketValid)
    {
        return false;
    }

    *packet = g_latestPacket;
    return true;
}

std::uint64_t ReceivedPacketCount()
{
    std::lock_guard<std::mutex> lock(g_packetMutex);
    return g_packetCount;
}

std::uint64_t RejectedPacketCount(std::string* lastRejected)
{
    std::lock_guard<std::mutex> lock(g_packetMutex);
    if (lastRejected != nullptr)
    {
        *lastRejected = g_lastRejectedPacket;
    }
    return g_rejectedPacketCount;
}

void SendState(const StatePacket& state)
{
    const std::string text = FormatStatePacket(state);

    std::lock_guard<std::mutex> lock(g_sendMutex);
    if (g_sendSocket == INVALID_SOCKET)
    {
        return;
    }

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(kOutgoingStatePort);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

    sendto(
        g_sendSocket,
        text.data(),
        static_cast<int>(text.size()),
        0,
        reinterpret_cast<const sockaddr*>(&address),
        sizeof(address));
}

void SetScopeLens(const ScopeLens& lens)
{
    std::lock_guard<std::mutex> lock(g_screenMutex);
    g_scopeLens = lens;
}

void SetVirtualScreen(const VirtualScreen& screen)
{
    std::lock_guard<std::mutex> lock(g_screenMutex);
    g_virtualScreen = screen;
}

void RecordRenderFrameSync(
    const std::uint32_t renderFrameId,
    const int sync)
{
    std::lock_guard<std::mutex> lock(g_syncMutex);

    RenderFrameSync& entry =
        g_renderFrameSyncs[g_renderFrameSyncWriteIndex];
    entry.valid = true;
    entry.renderFrameId = renderFrameId;
    entry.sync = sync;

    g_renderFrameSyncWriteIndex =
        (g_renderFrameSyncWriteIndex + 1u) %
        g_renderFrameSyncs.size();
}

} // namespace kisak::vr::winlatorxr

void VR_WinlatorXrBeforePresent(
    IDirect3DDevice9* const device,
    const std::uint64_t renderFrameId)
{
    kisak::vr::winlatorxr::g_presentCount.fetch_add(
        1u,
        std::memory_order_relaxed);

    namespace wxr = kisak::vr::winlatorxr;

    if (device == nullptr ||
        !wxr::IsRunning() ||
        FAILED(device->TestCooperativeLevel()))
    {
        return;
    }

    // Before anything else: WinlatorXR only enters VR when it finds the sync
    // pixel at screen (0,0).
    wxr::EnsureBorderlessWindow(device);

    int sync = 0;
    bool matched = false;

    {
        std::lock_guard<std::mutex> lock(wxr::g_syncMutex);
        for (const wxr::RenderFrameSync& entry : wxr::g_renderFrameSyncs)
        {
            if (entry.valid &&
                entry.renderFrameId ==
                    static_cast<std::uint32_t>(renderFrameId))
            {
                sync = entry.sync;
                matched = true;
                break;
            }
        }
    }

    // Menus and loading screens do not record a render pose; stamp the most
    // recent sync so WinlatorXR keeps presenting.
    if (!matched)
    {
        std::lock_guard<std::mutex> lock(wxr::g_packetMutex);
        if (!wxr::g_latestPacketValid)
        {
            return;
        }
        sync = wxr::g_latestPacket.sync;
    }

    IDirect3DSurface9* backBuffer = nullptr;
    HRESULT hr = device->GetBackBuffer(
        0,
        0,
        D3DBACKBUFFER_TYPE_MONO,
        &backBuffer);

    wxr::VirtualScreen screen;
    {
        std::lock_guard<std::mutex> lock(wxr::g_screenMutex);
        screen = wxr::g_virtualScreen;
    }

    wxr::ScopeLens lens;
    {
        std::lock_guard<std::mutex> lock(wxr::g_screenMutex);
        lens = wxr::g_scopeLens;
    }

    if (SUCCEEDED(hr) && backBuffer != nullptr && lens.active)
    {
        wxr::PresentScopeLens(device, backBuffer, lens);
    }

    if (SUCCEEDED(hr) && backBuffer != nullptr && screen.active)
    {
        const HRESULT screenResult =
            wxr::PresentVirtualScreen(device, backBuffer, screen);

        if (FAILED(screenResult) && !wxr::g_loggedScreenFailure)
        {
            Com_PrintWarning(
                0,
                "[VR][WINLATORXR] Could not draw the virtual screen "
                "(HRESULT 0x%08X); menus and videos may look split.\n",
                static_cast<unsigned int>(screenResult));
            wxr::g_loggedScreenFailure = true;
        }
        else if (SUCCEEDED(screenResult) && !wxr::g_loggedFirstScreen)
        {
            Com_Printf(
                0,
                "[VR][WINLATORXR] Drew the first room-fixed virtual screen "
                "(source %.2f-%.2f x %.2f-%.2f of the window).\n",
                screen.sourceLeft,
                screen.sourceRight,
                screen.sourceTop,
                screen.sourceBottom);
            wxr::g_loggedFirstScreen = true;
        }
    }

    if (SUCCEEDED(hr) && backBuffer != nullptr)
    {
        // Stamp last so the menu copy cannot overwrite it. HWXR stamps a
        // 10x10 block so window scaling cannot lose the pixel.
        const RECT syncRect = {0, 0, 10, 10};
        hr = device->ColorFill(
            backBuffer,
            &syncRect,
            D3DCOLOR_XRGB(wxr::SyncPixelRed(sync), 0, 0));
        backBuffer->Release();
    }

    if (FAILED(hr))
    {
        if (!wxr::g_loggedStampFailure)
        {
            Com_PrintWarning(
                0,
                "[VR][WINLATORXR] Could not stamp the HMD_SYNC pixel "
                "(HRESULT 0x%08X). WinlatorXR may present frames with a "
                "mismatched pose.\n",
                static_cast<unsigned int>(hr));
            wxr::g_loggedStampFailure = true;
        }
        return;
    }

    if (!wxr::g_loggedFirstStamp)
    {
        Com_Printf(
            0,
            "[VR][WINLATORXR] Stamped the first HMD_SYNC pixel (%d, %s).\n",
            sync,
            matched ? "matched render pose" : "latest packet");
        wxr::g_loggedFirstStamp = true;
    }
}

void VR_WinlatorXrCaptureScopePanel(
    IDirect3DDevice9* const device,
    const int panelX,
    const int panelY,
    const int panelSize)
{
    using kisak::vr::winlatorxr::g_scopePanel;

    if (device == nullptr || panelSize <= 0)
    {
        return;
    }

    IDirect3DSurface9* source = nullptr;
    if (FAILED(device->GetRenderTarget(0u, &source)) ||
        source == nullptr)
    {
        return;
    }

    D3DSURFACE_DESC sourceDescription = {};
    HRESULT hr = source->GetDesc(&sourceDescription);

    if (SUCCEEDED(hr) &&
        (g_scopePanel.texture == nullptr ||
         g_scopePanel.size != panelSize))
    {
        if (g_scopePanel.texture != nullptr)
        {
            g_scopePanel.texture->Release();
            g_scopePanel.texture = nullptr;
            g_scopePanel.valid = false;
        }

        hr = device->CreateTexture(
            static_cast<UINT>(panelSize),
            static_cast<UINT>(panelSize),
            1u,
            D3DUSAGE_RENDERTARGET,
            sourceDescription.Format,
            D3DPOOL_DEFAULT,
            &g_scopePanel.texture,
            nullptr);

        g_scopePanel.size = SUCCEEDED(hr) ? panelSize : 0;
    }

    if (SUCCEEDED(hr) && g_scopePanel.texture != nullptr)
    {
        IDirect3DSurface9* destination = nullptr;
        if (SUCCEEDED(g_scopePanel.texture->GetSurfaceLevel(
                0u, &destination)))
        {
            const RECT sourceRect = {
                static_cast<LONG>(panelX),
                static_cast<LONG>(panelY),
                static_cast<LONG>(panelX + panelSize),
                static_cast<LONG>(panelY + panelSize),
            };

            g_scopePanel.valid = SUCCEEDED(device->StretchRect(
                source,
                &sourceRect,
                destination,
                nullptr,
                D3DTEXF_NONE));

            destination->Release();
        }
    }

    source->Release();
}
