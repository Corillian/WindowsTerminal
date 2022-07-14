// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"

#include "DxRenderer.hpp"
#include "CustomTextLayout.h"

#include "../../interactivity/win32/CustomWindowMessages.h"
#include "../../types/inc/Viewport.hpp"
#include "../../inc/unicode.hpp"
#include "../../inc/DefaultSettings.h"
#include <VersionHelpers.h>

#include "ScreenPixelShader.h"
#include "ScreenVertexShader.h"
#include <DirectXMath.h>
#include <d3dcompiler.h>
#include <DirectXColors.h>

using namespace DirectX;

std::atomic<size_t> Microsoft::Console::Render::DxEngine::_tracelogCount{ 0 };
#pragma warning(suppress : 26477) // We don't control tracelogging macros
TRACELOGGING_DEFINE_PROVIDER(g_hDxRenderProvider,
    "Microsoft.Windows.Terminal.Renderer.DirectX",
    // {c93e739e-ae50-5a14-78e7-f171e947535d}
    (0xc93e739e, 0xae50, 0x5a14, 0x78, 0xe7, 0xf1, 0x71, 0xe9, 0x47, 0x53, 0x5d), );


// This is fun
extern "C" NTSYSAPI NTSTATUS NTAPI RtlGetVersion(_Out_ PRTL_OSVERSIONINFOW lpVersionInformation);

static bool IsWindows11OrGreater()
{
    OSVERSIONINFOEXW info = { 0 };

    info.dwOSVersionInfoSize = sizeof(info);

    if(RtlGetVersion(reinterpret_cast<PRTL_OSVERSIONINFOW>(&info)) == 0)
    {
        // Many WinUI3 things work on Windows 11 but not Windows 10
        // Disappointingly, this still seems to be true for the latest 21H2 v10.0.19044
        // so check to see if we're running Win11 (>= 10.0.22000)
        return info.dwMajorVersion > 10
            || (info.dwMajorVersion == 10 && (info.dwMinorVersion > 0 || info.dwBuildNumber >= 22000));
    }

    return false;
}

// Quad where we draw the terminal.
// pos is world space coordinates where origin is at the center of screen.
// tex is texel coordinates where origin is top left.
// Layout the quad as a triangle strip where the _screenQuadVertices are place like so.
// 2 0
// 3 1
struct ShaderInput
{
    XMFLOAT3 pos;
    XMFLOAT2 tex;
} const _screenQuadVertices[] = {
    { XMFLOAT3(1.f, 1.f, 0.f), XMFLOAT2(1.f, 0.f) },
    { XMFLOAT3(1.f, -1.f, 0.f), XMFLOAT2(1.f, 1.f) },
    { XMFLOAT3(-1.f, 1.f, 0.f), XMFLOAT2(0.f, 0.f) },
    { XMFLOAT3(-1.f, -1.f, 0.f), XMFLOAT2(0.f, 1.f) },
};

D3D11_INPUT_ELEMENT_DESC _shaderInputLayout[] = {
    { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 }
};

#pragma hdrstop

using namespace Microsoft::Console::Render;
using namespace Microsoft::Console::Types;

// Routine Description:
// - Constructs a DirectX-based renderer for console text
//   which primarily uses DirectWrite on a Direct2D surface
#pragma warning(suppress : 26455)
// TODO GH 2683: The default constructor should not throw.
DxEngine::DxEngine() :
    RenderEngineBase(),
    _pool{ til::pmr::get_default_resource() },
    _invalidMap{ &_pool },
    _invalidScroll{},
    _allInvalid{ false },
    _firstFrame{ true },
    _presentParams{ 0 },
    _presentReady{ false },
    _presentScroll{ 0 },
    _presentDirty{ 0 },
    _presentOffset{ 0 },
    _isEnabled{ false },
    _isPainting{ false },
    _displaySizePixels{},
    _foregroundColor{ 0 },
    _backgroundColor{ 0 },
    _selectionBackground{},
    _haveDeviceResources{ false },
    _swapChainHandle{ INVALID_HANDLE_VALUE },
    _swapChainDesc{ 0 },
    _swapChainFrameLatencyWaitableObject{ INVALID_HANDLE_VALUE },
    _recreateDeviceRequested{ false },
    _terminalEffectsEnabled{ false },
    _retroTerminalEffect{ false },
    _pixelShaderPath{},
    _forceFullRepaintRendering{ false },
    _softwareRendering{ false },
    _antialiasingMode{ D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE },
    _defaultBackgroundIsTransparent{ true },
    _hwndTarget{ static_cast<HWND>(INVALID_HANDLE_VALUE) },
    _sizeTarget{},
    _dpi{ USER_DEFAULT_SCREEN_DPI },
    _scale{ 1.0f },
    _prevScale{ 1.0f },
    _chainMode{ SwapChainMode::ForComposition },
    _customLayout{},
    _customRenderer{ ::Microsoft::WRL::Make<CustomTextRenderer>() },
    _drawingContext{}
{
    const auto was = _tracelogCount.fetch_add(1);
    if(0 == was)
    {
        TraceLoggingRegister(g_hDxRenderProvider);
    }

    THROW_IF_FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&_d2dFactory)));

    THROW_IF_FAILED(DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(_dwriteFactory),
        reinterpret_cast<IUnknown**>(_dwriteFactory.GetAddressOf())));

    // Initialize our default selection color to DEFAULT_FOREGROUND, but make
    // sure to set to to a D2D1::ColorF
    SetSelectionBackground(DEFAULT_FOREGROUND);

    _fontRenderData = std::make_unique<DxFontRenderData>(_dwriteFactory);
}

// Routine Description:
// - Destroys an instance of the DirectX rendering engine
DxEngine::~DxEngine()
{
    _ReleaseDeviceResources();

    const auto was = _tracelogCount.fetch_sub(1);
    if(1 == was)
    {
        TraceLoggingUnregister(g_hDxRenderProvider);
    }
}

// Routine Description:
// - Sets this engine to enabled allowing painting and presentation to occur
// Arguments:
// - <none>
// Return Value:
// - Generally S_OK, but might return a DirectX or memory error if
//   resources need to be created or adjusted when enabling to prepare for draw
//   Can give invalid state if you enable an enabled class.
[[nodiscard]] HRESULT DxEngine::Enable() noexcept
{
    return _EnableDisplayAccess(true);
}

// Routine Description:
// - Sets this engine to disabled to prevent painting and presentation from occurring
// Arguments:
// - <none>
// Return Value:
// - Should be OK. We might close/free resources, but that shouldn't error.
//   Can give invalid state if you disable a disabled class.
[[nodiscard]] HRESULT DxEngine::Disable() noexcept
{
    return _EnableDisplayAccess(false);
}

// Routine Description:
// - Helper to enable/disable painting/display access/presentation in a unified
//   manner between enable/disable functions.
// Arguments:
// - outputEnabled - true to enable, false to disable
// Return Value:
// - Generally OK. Can return invalid state if you set to the state that is already
//   active (enabling enabled, disabling disabled).
[[nodiscard]] HRESULT DxEngine::_EnableDisplayAccess(const bool outputEnabled) noexcept
{
    // Invalid state if we're setting it to the same as what we already have.
    RETURN_HR_IF(E_NOT_VALID_STATE, outputEnabled == _isEnabled);

    _isEnabled = outputEnabled;
    if(!_isEnabled)
    {
        _ReleaseDeviceResources();
    }

    return S_OK;
}

// Routine Description:
// - Compiles a shader source into binary blob.
// Arguments:
// - source - Shader source
// - target - What kind of shader this is
// - entry - Entry function of shader
// Return Value:
// - Compiled binary. Errors are thrown and logged.
static Microsoft::WRL::ComPtr<ID3DBlob> _CompileShader(const std::string_view& source, const char* target)
{
#if !TIL_FEATURE_DXENGINESHADERSUPPORT_ENABLED
    THROW_HR(E_UNEXPECTED);
    return 0;
#else
    Microsoft::WRL::ComPtr<ID3DBlob> code{};
    Microsoft::WRL::ComPtr<ID3DBlob> error{};

    const auto hr = D3DCompile(
        source.data(),
        source.size(),
        nullptr,
        nullptr,
        nullptr,
        "main",
        target,
        D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR | D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &code,
        &error);

    if(FAILED(hr))
    {
        LOG_HR_MSG(hr, "D3DCompile failed with %08x", hr);
        if(error)
        {
            LOG_HR_MSG(hr, "D3DCompile error\n%S", static_cast<const char*>(error->GetBufferPointer()));
        }

        THROW_HR(hr);
    }

    return code;
#endif
}

// Routine Description:
// - Checks if terminal effects are enabled.
// Arguments:
// Return Value:
// - True if terminal effects are enabled
bool DxEngine::_HasTerminalEffects() const noexcept
{
    return _terminalEffectsEnabled && (_retroTerminalEffect || !_pixelShaderPath.empty());
}

// Routine Description:
// - Toggles terminal effects off and on. If no terminal effect is configured has no effect
// Arguments:
// Return Value:
// - Void
void DxEngine::ToggleShaderEffects() noexcept
{
    _terminalEffectsEnabled = !_terminalEffectsEnabled;
    _recreateDeviceRequested = true;
#pragma warning(suppress : 26447) // The function is declared 'noexcept' but calls function 'Log_IfFailed()' which may throw exceptions (f.6).
    LOG_IF_FAILED(InvalidateAll());
}

// Routine Description:
// - Loads pixel shader source depending on _retroTerminalEffect and _pixelShaderPath
// Arguments:
// Return Value:
// - Pixel shader source code
std::string DxEngine::_LoadPixelShaderFile() const
{
    // If the user specified the new pixel shader, it has precedence
    if(!_pixelShaderPath.empty())
    {
        try
        {
            wil::unique_hfile hFile{ CreateFileW(_pixelShaderPath.c_str(),
                                                 GENERIC_READ,
                                                 FILE_SHARE_READ,
                                                 nullptr,
                                                 OPEN_EXISTING,
                                                 FILE_ATTRIBUTE_NORMAL,
                                                 nullptr) };

            THROW_LAST_ERROR_IF(!hFile); // This will be caught below.

            // fileSize is in bytes
            const auto fileSize = GetFileSize(hFile.get(), nullptr);
            THROW_LAST_ERROR_IF(fileSize == INVALID_FILE_SIZE);

            std::vector<char> utf8buffer;
            utf8buffer.reserve(fileSize);

            DWORD bytesRead = 0;
            THROW_LAST_ERROR_IF(!ReadFile(hFile.get(), utf8buffer.data(), fileSize, &bytesRead, nullptr));

            // convert buffer to UTF-8 string
            std::string utf8string(utf8buffer.data(), fileSize);

            return utf8string;
        }
        catch(...)
        {
            // If we ran into any problems during loading pixel shader, call to
            // the warning callback to surface the file not found error
            const auto exceptionHr = LOG_CAUGHT_EXCEPTION();
            if(_pfnWarningCallback)
            {
                _pfnWarningCallback(exceptionHr);
            }

            return std::string{};
        }
    }
    else if(_retroTerminalEffect)
    {
        return std::string{ retroPixelShaderString };
    }

    return std::string{};
}

// Routine Description:
// - Setup D3D objects for doing shader things for terminal effects.
// Arguments:
// Return Value:
// - HRESULT status.
HRESULT DxEngine::_SetupTerminalEffects()
{
    _pixelShaderLoaded = false;

    const auto pixelShaderSource = _LoadPixelShaderFile();
    if(pixelShaderSource.empty())
    {
        // There's no shader to compile. This might be due to failing to load,
        // or because there's just no shader enabled at all.
        // Turn the effects off for now.
        _terminalEffectsEnabled = false;

        return S_FALSE;
    }

    ::Microsoft::WRL::ComPtr<ID3D11Texture2D> swapBuffer;
    RETURN_IF_FAILED(_dxgiSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&swapBuffer));

    // Setup render target.
    RETURN_IF_FAILED(_d3dDevice->CreateRenderTargetView(swapBuffer.Get(), nullptr, &_renderTargetView));

    // Setup _framebufferCapture, to where we'll copy current frame when rendering effects.
    D3D11_TEXTURE2D_DESC framebufferCaptureDesc{};
    swapBuffer->GetDesc(&framebufferCaptureDesc);
    WI_SetFlag(framebufferCaptureDesc.BindFlags, D3D11_BIND_SHADER_RESOURCE);
    RETURN_IF_FAILED(_d3dDevice->CreateTexture2D(&framebufferCaptureDesc, nullptr, &_framebufferCapture));

    // Setup the viewport.
    D3D11_VIEWPORT vp;
    vp.Width = static_cast<float>(_displaySizePixels.width);
    vp.Height = static_cast<float>(_displaySizePixels.height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    _d3dDeviceContext->RSSetViewports(1, &vp);

    const char* shaderTargetVS = nullptr;
    const char* shaderTargetPS = nullptr;
    switch (_d3dDevice->GetFeatureLevel())
    {
    case D3D_FEATURE_LEVEL_10_0:
        shaderTargetVS = "vs_4_0";
        shaderTargetPS = "ps_4_0";
        break;
    case D3D_FEATURE_LEVEL_10_1:
        shaderTargetVS = "vs_4_1";
        shaderTargetPS = "ps_4_1";
        break;
    default:
        shaderTargetVS = "vs_5_0";
        shaderTargetPS = "ps_5_0";
        break;
    }

    // Prepare shaders.
    auto vertexBlob = _CompileShader(&screenVertexShaderString[0], shaderTargetVS);
    Microsoft::WRL::ComPtr<ID3DBlob> pixelBlob;
    // As the pixel shader source is user provided it's possible there's a problem with it
    //  so load it inside a try catch, on any error log and fallback on the error pixel shader
    //  If even the error pixel shader fails to load rely on standard exception handling
    try
    {
        pixelBlob = _CompileShader(pixelShaderSource, shaderTargetPS);
    }
    catch(...)
    {
        // Call to the warning callback to surface the shader compile error
        const auto exceptionHr = LOG_CAUGHT_EXCEPTION();
        if(_pfnWarningCallback)
        {
            // If this fails, it'll return E_FAIL, which is terribly
            // uninformative. Instead, raise something more useful.
            _pfnWarningCallback(D2DERR_SHADER_COMPILE_FAILED);
        }
        return exceptionHr;
    }

    RETURN_IF_FAILED(_d3dDevice->CreateVertexShader(
        vertexBlob->GetBufferPointer(),
        vertexBlob->GetBufferSize(),
        nullptr,
        &_vertexShader));

    RETURN_IF_FAILED(_d3dDevice->CreatePixelShader(
        pixelBlob->GetBufferPointer(),
        pixelBlob->GetBufferSize(),
        nullptr,
        &_pixelShader));

    RETURN_IF_FAILED(_d3dDevice->CreateInputLayout(
        static_cast<const D3D11_INPUT_ELEMENT_DESC*>(_shaderInputLayout),
        ARRAYSIZE(_shaderInputLayout),
        vertexBlob->GetBufferPointer(),
        vertexBlob->GetBufferSize(),
        &_vertexLayout));

    // Create vertex buffer for screen quad.
    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.ByteWidth = sizeof(ShaderInput) * ARRAYSIZE(_screenQuadVertices);
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = 0;

    D3D11_SUBRESOURCE_DATA InitData{};
    InitData.pSysMem = static_cast<const void*>(_screenQuadVertices);

    RETURN_IF_FAILED(_d3dDevice->CreateBuffer(&bd, &InitData, &_screenQuadVertexBuffer));

    D3D11_BUFFER_DESC pixelShaderSettingsBufferDesc{};
    pixelShaderSettingsBufferDesc.Usage = D3D11_USAGE_DEFAULT;
    pixelShaderSettingsBufferDesc.ByteWidth = sizeof(_pixelShaderSettings);
    pixelShaderSettingsBufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    _shaderStartTime = std::chrono::steady_clock::now();

    _ComputePixelShaderSettings();

    D3D11_SUBRESOURCE_DATA pixelShaderSettingsInitData{};
    pixelShaderSettingsInitData.pSysMem = &_pixelShaderSettings;

    RETURN_IF_FAILED(_d3dDevice->CreateBuffer(&pixelShaderSettingsBufferDesc, &pixelShaderSettingsInitData, &_pixelShaderSettingsBuffer));

    // Sampler state is needed to use texture as input to shader.
    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MipLODBias = 0.0f;
    samplerDesc.MaxAnisotropy = 1;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    samplerDesc.BorderColor[0] = 0;
    samplerDesc.BorderColor[1] = 0;
    samplerDesc.BorderColor[2] = 0;
    samplerDesc.BorderColor[3] = 0;
    samplerDesc.MinLOD = 0;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

    // Create the texture sampler state.
    RETURN_IF_FAILED(_d3dDevice->CreateSamplerState(&samplerDesc, &_samplerState));

    _pixelShaderLoaded = true;
    return S_OK;
}

// Routine Description:
// - Puts the correct values in _pixelShaderSettings, so the struct can be
//   passed the GPU and updates the GPU resource.
// Arguments:
// - <none>
// Return Value:
// - <none>
void DxEngine::_ComputePixelShaderSettings() noexcept
{
    if(_HasTerminalEffects() && _d3dDeviceContext && _pixelShaderSettingsBuffer)
    {
        try
        {
            // Set the time (seconds since the shader was loaded)
            _pixelShaderSettings.Time = std::chrono::duration_cast<std::chrono::duration<float>>(std::chrono::steady_clock::now() - _shaderStartTime).count();

            // Set the UI Scale
            _pixelShaderSettings.Scale = _scale;

            // Set the display resolution
            const auto w = static_cast<float>(_displaySizePixels.width);
            const auto h = static_cast<float>(_displaySizePixels.height);
            _pixelShaderSettings.Resolution = XMFLOAT2{ w, h };

            // Set the background
            DirectX::XMFLOAT4 background{};
            background.x = _backgroundColor.r;
            background.y = _backgroundColor.g;
            background.z = _backgroundColor.b;
            background.w = _backgroundColor.a;
            _pixelShaderSettings.Background = background;

            _d3dDeviceContext->UpdateSubresource(_pixelShaderSettingsBuffer.Get(), 0, nullptr, &_pixelShaderSettings, 0, 0);
        }
        CATCH_LOG();
    }
}

// Method Description:
// - Use DCompositionCreateSurfaceHandle to create a swapchain handle. This API
//   is only present in Windows 8.1+, so we need to delay-load it to make sure
//   we can still load on Windows 7.
// - We can't actually hit this on Windows 7, because only the WPF control uses
//   us on Windows 7, and they're using the ForHwnd path, which doesn't hit this
//   at all.
// Arguments:
// - <none>
// Return Value:
// - An HRESULT for failing to load dcomp.dll, or failing to find the API, or an
//   actual failure from the API itself.
[[nodiscard]] HRESULT DxEngine::_CreateSurfaceHandle() noexcept
{
#pragma warning(suppress : 26447)
    wil::unique_hmodule hDComp{ LoadLibraryEx(L"Dcomp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) };
    RETURN_LAST_ERROR_IF(hDComp.get() == nullptr);

    auto fn = GetProcAddressByFunctionDeclaration(hDComp.get(), DCompositionCreateSurfaceHandle);
    RETURN_LAST_ERROR_IF(fn == nullptr);

    return fn(GENERIC_ALL, nullptr, &_swapChainHandle);
}

// Routine Description;
// - Creates device-specific resources required for drawing
//   which generally means those that are represented on the GPU and can
//   vary based on the monitor, display adapter, etc.
// - These may need to be recreated during the course of painting a frame
//   should something about that hardware pipeline change.
// - Will free device resources that already existed as first operation.
// Arguments:
// - createSwapChain - If true, we create the entire rendering pipeline
//                   - If false, we just set up the adapter.
// Return Value:
// - Could be any DirectX/D3D/D2D/DXGI/DWrite error or memory issue.
[[nodiscard]] HRESULT DxEngine::_CreateDeviceResources(const bool createSwapChain) noexcept
try
{
    if(_haveDeviceResources)
    {
        _ReleaseDeviceResources();
    }

    auto freeOnFail = wil::scope_exit([&]() noexcept { _ReleaseDeviceResources(); });

    RETURN_IF_FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&_dxgiFactory2)));

    const DWORD DeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT |
        // clang-format off
// This causes problems for folks who do not have the whole DirectX SDK installed
// when they try to run the rest of the project in debug mode.
// As such, I'm leaving this flag here for people doing DX-specific work to toggle it
// only when they need it and shutting it off otherwise.
// Find out more about the debug layer here:
// https://docs.microsoft.com/en-us/windows/desktop/direct3d11/overviews-direct3d-11-devices-layers
// You can find out how to install it here:
// https://docs.microsoft.com/en-us/windows/uwp/gaming/use-the-directx-runtime-and-visual-studio-graphics-diagnostic-features
                              // clang-format on
                              // D3D11_CREATE_DEVICE_DEBUG |
        D3D11_CREATE_DEVICE_SINGLETHREADED;

    static constexpr std::array FeatureLevels{
        D3D_FEATURE_LEVEL_11_1,
                                                          D3D_FEATURE_LEVEL_11_0,
                                                          D3D_FEATURE_LEVEL_10_1,
                                                          D3D_FEATURE_LEVEL_10_0,
    };

    // Trying hardware first for maximum performance, then trying WARP (software) renderer second
    // in case we're running inside a downlevel VM where hardware passthrough isn't enabled like
    // for Windows 7 in a VM.
    auto hardwareResult = E_NOT_SET;

    // If we're not forcing software rendering, try hardware first.
    // Otherwise, let the error state fall down and create with the software renderer directly.
    if(!_softwareRendering)
    {
        hardwareResult = D3D11CreateDevice(nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            DeviceFlags,
            FeatureLevels.data(),
            gsl::narrow_cast<UINT>(FeatureLevels.size()),
            D3D11_SDK_VERSION,
            &_d3dDevice,
            nullptr,
            &_d3dDeviceContext);
    }

    if(FAILED(hardwareResult))
    {
        RETURN_IF_FAILED(D3D11CreateDevice(nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            DeviceFlags,
            FeatureLevels.data(),
            gsl::narrow_cast<UINT>(FeatureLevels.size()),
            D3D11_SDK_VERSION,
            &_d3dDevice,
            nullptr,
            &_d3dDeviceContext));
    }

    _displaySizePixels = _GetClientSize();

    // Get the other device types so we have deeper access to more functionality
    // in our pipeline than by just walking straight from the D3D device.

    RETURN_IF_FAILED(_d3dDevice.As(&_dxgiDevice));
    RETURN_IF_FAILED(_d2dFactory->CreateDevice(_dxgiDevice.Get(), _d2dDevice.ReleaseAndGetAddressOf()));

    // Create a device context out of it (supersedes render targets)
    RETURN_IF_FAILED(_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &_d2dDeviceContext));

    if(createSwapChain)
    {
        _swapChainDesc = { 0 };
        _swapChainDesc.Flags = 0;

        // requires DXGI 1.3 which was introduced in Windows 8.1
        WI_SetFlagIf(_swapChainDesc.Flags, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT, IsWindows8Point1OrGreater());

        _swapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        _swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        _swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        _swapChainDesc.BufferCount = 2;
        _swapChainDesc.SampleDesc.Count = 1;
        _swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        _swapChainDesc.Scaling = DXGI_SCALING_NONE;

        switch(_chainMode)
        {
        case SwapChainMode::ForHwnd:
        {
            // use the HWND's dimensions for the swap chain dimensions.
            RECT rect = { 0 };
            RETURN_IF_WIN32_BOOL_FALSE(GetClientRect(_hwndTarget, &rect));

            _swapChainDesc.Width = rect.right - rect.left;
            _swapChainDesc.Height = rect.bottom - rect.top;

            // We can't do alpha for HWNDs. Set to ignore. It will fail otherwise.
            _swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            const auto createSwapChainResult = _dxgiFactory2->CreateSwapChainForHwnd(_d3dDevice.Get(),
                _hwndTarget,
                &_swapChainDesc,
                nullptr,
                nullptr,
                &_dxgiSwapChain);
            if(FAILED(createSwapChainResult))
            {
                _swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
                RETURN_IF_FAILED(_dxgiFactory2->CreateSwapChainForHwnd(_d3dDevice.Get(),
                    _hwndTarget,
                    &_swapChainDesc,
                    nullptr,
                    nullptr,
                    &_dxgiSwapChain));
            }

            break;
        }
        case SwapChainMode::ForComposition:
        {
            if(!_swapChainHandle)
            {
                RETURN_IF_FAILED(_CreateSurfaceHandle());
            }

            RETURN_IF_FAILED(_dxgiFactory2.As(&_dxgiFactoryMedia));

            // Use the given target size for compositions.
            _swapChainDesc.Width = _displaySizePixels.narrow_width<UINT>();
            _swapChainDesc.Height = _displaySizePixels.narrow_height<UINT>();

            // We're doing advanced composition pretty much for the purpose of pretty alpha, so turn it on.
            _swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
            // It's 100% required to use scaling mode stretch for composition. There is no other choice.
            _swapChainDesc.Scaling = DXGI_SCALING_STRETCH;

            RETURN_IF_FAILED(_dxgiFactoryMedia->CreateSwapChainForCompositionSurfaceHandle(_d3dDevice.Get(),
                _swapChainHandle.get(),
                &_swapChainDesc,
                nullptr,
                &_dxgiSwapChain));
            break;
        }
        default:
            THROW_HR(E_NOTIMPL);
        }

        if(IsWindows8Point1OrGreater())
        {
            ::Microsoft::WRL::ComPtr<IDXGISwapChain2> swapChain2;
            const auto asResult = _dxgiSwapChain.As(&swapChain2);
            if(SUCCEEDED(asResult))
            {
                _swapChainFrameLatencyWaitableObject = wil::unique_handle{ swapChain2->GetFrameLatencyWaitableObject() };
            }
            else
            {
                LOG_HR_MSG(asResult, "Failed to obtain IDXGISwapChain2 from swap chain");
            }
        }

        if(_HasTerminalEffects())
        {
            const auto hr = _SetupTerminalEffects();
            if(FAILED(hr))
            {
                LOG_HR_MSG(hr, "Failed to setup terminal effects. Disabling.");
                _terminalEffectsEnabled = false;
            }
        }

        // With a new swap chain, mark the entire thing as invalid.
        RETURN_IF_FAILED(InvalidateAll());

        // This is our first frame on this new target.
        _firstFrame = true;

        RETURN_IF_FAILED(_PrepareRenderTarget());
    }

    _haveDeviceResources = true;
    if(_isPainting)
    {
        // TODO: MSFT: 21169176 - remove this or restore the "try a few times to render" code... I think
        _d2dDeviceContext->BeginDraw();
    }

    freeOnFail.release(); // don't need to release if we made it to the bottom and everything was good.

    // Notify that swap chain changed.

    if(_pfn)
    {
        try
        {
            _pfn();
        }
        CATCH_LOG(); // A failure in the notification function isn't a failure to prepare, so just log it and go on.
    }

    _recreateDeviceRequested = false;

    return S_OK;
}
CATCH_RETURN();

static constexpr D2D1_ALPHA_MODE _dxgiAlphaToD2d1Alpha(DXGI_ALPHA_MODE mode) noexcept
{
    switch(mode)
    {
    case DXGI_ALPHA_MODE_PREMULTIPLIED:
        return D2D1_ALPHA_MODE_PREMULTIPLIED;
    case DXGI_ALPHA_MODE_STRAIGHT:
        return D2D1_ALPHA_MODE_STRAIGHT;
    case DXGI_ALPHA_MODE_IGNORE:
        return D2D1_ALPHA_MODE_IGNORE;
    case DXGI_ALPHA_MODE_FORCE_DWORD:
        return D2D1_ALPHA_MODE_FORCE_DWORD;
    default:
    case DXGI_ALPHA_MODE_UNSPECIFIED:
        return D2D1_ALPHA_MODE_UNKNOWN;
    }
}

[[nodiscard]] HRESULT DxEngine::_PrepareRenderTarget() noexcept
{
    try
    {
        // Pull surface out of swap chain.
        RETURN_IF_FAILED(_dxgiSwapChain->GetBuffer(0, IID_PPV_ARGS(&_dxgiSurface)));

        // Make a bitmap and bind it to the swap chain surface
        const auto bitmapProperties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(_swapChainDesc.Format, _dxgiAlphaToD2d1Alpha(_swapChainDesc.AlphaMode)));

        RETURN_IF_FAILED(_d2dDeviceContext->CreateBitmapFromDxgiSurface(_dxgiSurface.Get(), bitmapProperties, &_d2dBitmap));

        // Assign that bitmap as the target of the D2D device context. Draw commands hit the context
        // and are backed by the bitmap which is bound to the swap chain which goes on to be presented.
        // (The foot bone connected to the leg bone,
        //  The leg bone connected to the knee bone,
        //  The knee bone connected to the thigh bone
        //  ... and so on)

        _d2dDeviceContext->SetTarget(_d2dBitmap.Get());

        // We need the AntialiasMode for non-text object to be Aliased to ensure
        //  that background boxes line up with each other and don't leave behind
        //  stray colors.
        // See GH#3626 for more details.
        _d2dDeviceContext->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        _d2dDeviceContext->SetTextAntialiasMode(_antialiasingMode);

        RETURN_IF_FAILED(_d2dDeviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::DarkRed),
            &_d2dBrushBackground));

        RETURN_IF_FAILED(_d2dDeviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),
            &_d2dBrushForeground));

        _strokeStyleProperties = D2D1_STROKE_STYLE_PROPERTIES{
            D2D1_CAP_STYLE_SQUARE, // startCap
            D2D1_CAP_STYLE_SQUARE, // endCap
            D2D1_CAP_STYLE_SQUARE, // dashCap
            D2D1_LINE_JOIN_MITER, // lineJoin
            0.f, // miterLimit
            D2D1_DASH_STYLE_SOLID, // dashStyle
            0.f, // dashOffset
        };
        RETURN_IF_FAILED(_d2dFactory->CreateStrokeStyle(&_strokeStyleProperties, nullptr, 0, &_strokeStyle));

        _dashStrokeStyleProperties = D2D1_STROKE_STYLE_PROPERTIES{
            D2D1_CAP_STYLE_SQUARE, // startCap
            D2D1_CAP_STYLE_SQUARE, // endCap
            D2D1_CAP_STYLE_FLAT, // dashCap
            D2D1_LINE_JOIN_MITER, // lineJoin
            0.f, // miterLimit
            D2D1_DASH_STYLE_CUSTOM, // dashStyle
            0.f, // dashOffset
        };
        // Custom dashes:
        // #   #   #   #
        // 1234123412341234
        static constexpr std::array<float, 2> hyperlinkDashes{ 1.f, 3.f };
        RETURN_IF_FAILED(_d2dFactory->CreateStrokeStyle(&_dashStrokeStyleProperties, hyperlinkDashes.data(), gsl::narrow_cast<UINT32>(hyperlinkDashes.size()), &_dashStrokeStyle));
        _hyperlinkStrokeStyle = _dashStrokeStyle;

        // If in composition mode, apply scaling factor matrix
        if(_chainMode == SwapChainMode::ForComposition)
        {
            DXGI_MATRIX_3X2_F inverseScale = { 0 };
            inverseScale._11 = 1.0f / _scale;
            inverseScale._22 = inverseScale._11;

            ::Microsoft::WRL::ComPtr<IDXGISwapChain2> sc2;
            RETURN_IF_FAILED(_dxgiSwapChain.As(&sc2));
            RETURN_IF_FAILED(sc2->SetMatrixTransform(&inverseScale));
        }

        _prevScale = _scale;
        return S_OK;
    }
    CATCH_RETURN();
}

// Routine Description:
// - Releases device-specific resources (typically held on the GPU)
// Arguments:
// - <none>
// Return Value:
// - <none>
void DxEngine::_ReleaseDeviceResources() noexcept
{
    try
    {
        _haveDeviceResources = false;

        // Destroy Terminal Effect resources
        _renderTargetView.Reset();
        _vertexShader.Reset();
        _pixelShader.Reset();
        _vertexLayout.Reset();
        _screenQuadVertexBuffer.Reset();
        _pixelShaderSettingsBuffer.Reset();
        _samplerState.Reset();
        _framebufferCapture.Reset();

        _d2dBrushForeground.Reset();
        _d2dBrushBackground.Reset();

        _d2dBitmap.Reset();

        if(nullptr != _d2dDeviceContext.Get() && _isPainting)
        {
            _d2dDeviceContext->EndDraw();
        }

        _d2dDeviceContext.Reset();

        _dxgiSurface.Reset();
        _dxgiSwapChain.Reset();
        _swapChainFrameLatencyWaitableObject.reset();

        _d2dDevice.Reset();
        _dxgiDevice.Reset();

        if(nullptr != _d3dDeviceContext.Get())
        {
            // To ensure the swap chain goes away we must unbind any views from the
            // D3D pipeline
            _d3dDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
        }
        _d3dDeviceContext.Reset();

        _d3dDevice.Reset();

        _dxgiFactory2.Reset();
    }
    CATCH_LOG();
}

// Routine Description:
// - Calculates whether or not we should force grayscale AA based on the
//   current renderer state.
// Arguments:
// - <none> - Uses internal state of _antialiasingMode, _defaultTextBackgroundOpacity,
//            _backgroundColor, and _defaultBackgroundColor.
// Return Value:
// - True if we must render this text in grayscale AA as cleartype simply won't work. False otherwise.
[[nodiscard]] bool DxEngine::_ShouldForceGrayscaleAA() noexcept
{
    // GH#5098: If we're rendering with cleartype text, we need to always
    // render onto an opaque background. If our background's opacity is
    // 1.0f, that's great, we can use that. Otherwise, we need to force the
    // text renderer to render this text in grayscale. In
    // UpdateDrawingBrushes, we'll set the backgroundColor's a channel to
    // 1.0 if we're in cleartype mode and the background's opacity is 1.0.
    // Otherwise, at this point, the _backgroundColor's alpha is <1.0.
    //
    // Currently, only text with the default background color uses an alpha
    // of 0, every other background uses 1.0
    //
    // DANGER: Layers slow us down. Only do this in the specific case where
    // someone has chosen the slower ClearType antialiasing (versus the faster
    // grayscale antialiasing)
    const auto usingCleartype = _antialiasingMode == D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE;
    const auto usingTransparency = _defaultBackgroundIsTransparent;
    // Another way of naming "bgIsDefault" is "bgHasTransparency"
    const auto bgIsDefault = (_backgroundColor.a == _defaultBackgroundColor.a) &&
        (_backgroundColor.r == _defaultBackgroundColor.r) &&
        (_backgroundColor.g == _defaultBackgroundColor.g) &&
        (_backgroundColor.b == _defaultBackgroundColor.b);
    const auto forceGrayscaleAA = usingCleartype &&
        usingTransparency &&
        bgIsDefault;

    return forceGrayscaleAA;
}

// Routine Description:
// - Helper to create a DirectWrite text layout object
//   out of a string.
// Arguments:
// - string - The text to attempt to layout
// - stringLength - Length of string above in characters
// - ppTextLayout - Location to receive new layout object
// Return Value:
// - S_OK if layout created successfully, otherwise a DirectWrite error
[[nodiscard]] HRESULT DxEngine::_CreateTextLayout(
    _In_reads_(stringLength) PCWCHAR string,
    _In_ size_t stringLength,
    _Out_ IDWriteTextLayout** ppTextLayout) noexcept
    try
{
    return _dwriteFactory->CreateTextLayout(string,
        gsl::narrow<UINT32>(stringLength),
        _fontRenderData->DefaultTextFormat().Get(),
                                            static_cast<float>(_displaySizePixels.width),
                                            _fontRenderData->GlyphCell().height != 0 ? _fontRenderData->GlyphCell().narrow_height<float>() : _displaySizePixels.narrow_height<float>(),
        ppTextLayout);
}
CATCH_RETURN()

// Routine Description:
// - Sets the target window handle for our display pipeline
// - We will take over the surface of this window for drawing
// Arguments:
// - hwnd - Window handle
// Return Value:
// - S_OK
[[nodiscard]] HRESULT DxEngine::SetHwnd(const HWND hwnd) noexcept
{
    _hwndTarget = hwnd;
    _chainMode = SwapChainMode::ForHwnd;
    return S_OK;
}

[[nodiscard]] HRESULT DxEngine::SetWindowSize(const SIZE Pixels) noexcept
try
{
    _sizeTarget = til::size{ Pixels };
    return S_OK;
}
CATCH_RETURN();

void DxEngine::SetCallback(std::function<void()> pfn) noexcept
{
    _pfn = std::move(pfn);
}

void DxEngine::SetWarningCallback(std::function<void(const HRESULT)> pfn) noexcept
{
    _pfnWarningCallback = std::move(pfn);
}

bool DxEngine::GetRetroTerminalEffect() const noexcept
{
    return _retroTerminalEffect;
}

void DxEngine::SetRetroTerminalEffect(bool enable) noexcept
try
{
    if(_retroTerminalEffect != enable)
    {
        // Enable shader effects if the path isn't empty. Otherwise leave it untouched.
        _terminalEffectsEnabled = enable ? true : _terminalEffectsEnabled;
        _retroTerminalEffect = enable;
        _recreateDeviceRequested = true;
        LOG_IF_FAILED(InvalidateAll());
    }
}
CATCH_LOG()

void DxEngine::SetPixelShaderPath(std::wstring_view value) noexcept
try
{
    if(_pixelShaderPath != value)
    {
        // Enable shader effects if the path isn't empty. Otherwise leave it untouched.
        _terminalEffectsEnabled = value.empty() ? _terminalEffectsEnabled : true;
        _pixelShaderPath = std::wstring{ value };
        _recreateDeviceRequested = true;
        LOG_IF_FAILED(InvalidateAll());
    }
}
CATCH_LOG()

void DxEngine::SetForceFullRepaintRendering(bool enable) noexcept
try
{
    if(_forceFullRepaintRendering != enable)
    {
        _forceFullRepaintRendering = enable;
        LOG_IF_FAILED(InvalidateAll());
    }
}
CATCH_LOG()

void DxEngine::SetSoftwareRendering(bool enable) noexcept
try
{
    if(_softwareRendering != enable)
    {
        _softwareRendering = enable;
        _recreateDeviceRequested = true;
        LOG_IF_FAILED(InvalidateAll());
    }
}
CATCH_LOG()

HANDLE DxEngine::GetSwapChainHandle() noexcept
{
    if(!_swapChainHandle)
    {
#pragma warning(suppress : 26447) // The function is declared 'noexcept' but calls function 'Log_IfFailed()' which may throw exceptions (f.6).
        LOG_IF_FAILED(_CreateDeviceResources(true));
    }

    return _swapChainHandle.get();
}

void DxEngine::_InvalidateRectangle(const til::rect& rc)
{
    const auto size = _invalidMap.size();
    const auto topLeft = til::point{ 0, std::clamp(rc.top, 0, size.height) };
    const auto bottomRight = til::point{ size.width, std::clamp(rc.bottom, 0, size.height) };
    _invalidMap.set(til::rect{ topLeft, bottomRight });
}

bool DxEngine::_IsAllInvalid() const noexcept
{
    return std::abs(_invalidScroll.y) >= _invalidMap.size().height;
}

// Routine Description:
// - Invalidates a rectangle described in characters
// Arguments:
// - psrRegion - Character rectangle
// Return Value:
// - S_OK
[[nodiscard]] HRESULT DxEngine::Invalidate(const SMALL_RECT* const psrRegion) noexcept
try
{
    RETURN_HR_IF_NULL(E_INVALIDARG, psrRegion);

    if(!_allInvalid)
    {
        _InvalidateRectangle(til::rect{ Viewport::FromExclusive(*psrRegion).ToInclusive() });
    }

    return S_OK;
}
CATCH_RETURN()

// Routine Description:
// - Invalidates the cells of the cursor
// Arguments:
// - psrRegion - the region covered by the cursor
// Return Value:
// - S_OK
[[nodiscard]] HRESULT DxEngine::InvalidateCursor(const SMALL_RECT* const psrRegion) noexcept
{
    return Invalidate(psrRegion);
}

// Routine Description:
// - Invalidates a rectangle describing a pixel area on the display
// Arguments:
// - prcDirtyClient - pixel rectangle
// Return Value:
// - S_OK
[[nodiscard]] HRESULT DxEngine::InvalidateSystem(const RECT* const prcDirtyClient) noexcept
try
{
    RETURN_HR_IF_NULL(E_INVALIDARG, prcDirtyClient);

    if(!_allInvalid)
    {
        // Dirty client is in pixels. Use divide specialization against glyph factor to make conversion
        // to cells.
        _InvalidateRectangle(til::rect{ *prcDirtyClient }.scale_down(_fontRenderData->GlyphCell()));
    }

    return S_OK;
}
CATCH_RETURN();

// Routine Description:
// - Invalidates a series of character rectangles
// Arguments:
// - rectangles - One or more rectangles describing character positions on the grid
// Return Value:
// - S_OK
[[nodiscard]] HRESULT DxEngine::InvalidateSelection(const std::vector<SMALL_RECT>& rectangles) noexcept
{
    if(!_allInvalid)
    {
        for(const auto& rect : rectangles)
        {
            RETURN_IF_FAILED(Invalidate(&rect));
        }
    }
    return S_OK;
}

// Routine Description:
// - Scrolls the existing dirty region (if it exists) and
//   invalidates the area that is uncovered in the window.
// Arguments:
// - pcoordDelta - The number of characters to move and uncover.
//               - -Y is up, Y is down, -X is left, X is right.
// Return Value:
// - S_OK
[[nodiscard]] HRESULT DxEngine::InvalidateScroll(const COORD* const pcoordDelta) noexcept
try
{
    RETURN_HR_IF(E_INVALIDARG, !pcoordDelta);

    const til::point deltaCells{ *pcoordDelta };

    if(!_allInvalid)
    {
        if (deltaCells != til::point{})
        {
            // Shift the contents of the map and fill in revealed area.
            _invalidMap.translate(deltaCells, true);
            _invalidScroll += deltaCells;
            _allInvalid = _IsAllInvalid();
        }
    }

    return S_OK;
}
CATCH_RETURN();

// Routine Description:
// - Invalidates the entire window area
// Arguments:
// - <none>
// Return Value:
// - S_OK
[[nodiscard]] HRESULT DxEngine::InvalidateAll() noexcept
try
{
    _invalidMap.set_all();
    _allInvalid = true;

    // Since everything is invalidated here, mark this as a "first frame", so
    // that we won't use incremental drawing on it. The caller of this intended
    // for _everything_ to get redrawn, so setting _firstFrame will force us to
    // redraw the entire frame. This will make sure that things like the gutters
    // get cleared correctly.
    //
    // Invalidating everything is supposed to happen with resizes of the
    // entire canvas, changes of the font, and other such adjustments.
    _firstFrame = true;
    return S_OK;
}
CATCH_RETURN();

// Routine Description:
// - Gets the area in pixels of the surface we are targeting
// Arguments:
// - <none>
// Return Value:
// - X by Y area in pixels of the surface
[[nodiscard]] til::size DxEngine::_GetClientSize() const
{
    switch(_chainMode)
    {
    case SwapChainMode::ForHwnd:
    {
        RECT clientRect = { 0 };
        LOG_IF_WIN32_BOOL_FALSE(GetClientRect(_hwndTarget, &clientRect));

        return til::rect{ clientRect }.size();
    }
    case SwapChainMode::ForComposition:
    {
        return _sizeTarget;
    }
    default:
        FAIL_FAST_HR(E_NOTIMPL);
    }
}

// Routine Description:
// - Helper to multiply all parameters of a rectangle by the font size
//   to convert from characters to pixels.
// Arguments:
// - cellsToPixels - rectangle to update
// - fontSize - scaling factors
// Return Value:
// - <none> - Updates reference
void _ScaleByFont(RECT& cellsToPixels, SIZE fontSize) noexcept
{
    cellsToPixels.left *= fontSize.cx;
    cellsToPixels.right *= fontSize.cx;
    cellsToPixels.top *= fontSize.cy;
    cellsToPixels.bottom *= fontSize.cy;
}

// Routine Description:
// - This is unused by this renderer.
// Arguments:
// - pForcePaint - always filled with false.
// Return Value:
// - S_FALSE because this is unused.
[[nodiscard]] HRESULT DxEngine::PrepareForTeardown(_Out_ bool* const pForcePaint) noexcept
{
    RETURN_HR_IF_NULL(E_INVALIDARG, pForcePaint);

    *pForcePaint = false;
    return S_FALSE;
}

// Routine description:
// - Prepares the surfaces for painting and begins a drawing batch
// Arguments:
// - <none>
// Return Value:
// - Any DirectX error, a memory error, etc.
[[nodiscard]] HRESULT DxEngine::StartPaint() noexcept
try
{
    RETURN_HR_IF(E_NOT_VALID_STATE, _isPainting); // invalid to start a paint while painting.

    // If full repaints are needed then we need to invalidate everything
    // so the entire frame is repainted.
    if(_FullRepaintNeeded())
    {
        RETURN_IF_FAILED(InvalidateAll());
    }

    if(TraceLoggingProviderEnabled(g_hDxRenderProvider, WINEVENT_LEVEL_VERBOSE, TIL_KEYWORD_TRACE))
    {
        const auto invalidatedStr = _invalidMap.to_string();
        const auto invalidated = invalidatedStr.c_str();

#pragma warning(suppress : 26477 26485 26494 26482 26446 26447) // We don't control TraceLoggingWrite
        TraceLoggingWrite(g_hDxRenderProvider,
            "Invalid",
            TraceLoggingWideString(invalidated),
            TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE),
            TraceLoggingKeyword(TIL_KEYWORD_TRACE));
    }

    if(_isEnabled)
    {
        const auto clientSize = _GetClientSize();
        const auto glyphCellSize = _fontRenderData->GlyphCell();

        // If we don't have device resources or if someone has requested that we
        // recreate the device... then make new resources. (Create will dump the old ones.)
        if(!_haveDeviceResources || _recreateDeviceRequested)
        {
            RETURN_IF_FAILED(_CreateDeviceResources(true));
        }
        else if(_displaySizePixels != clientSize || _prevScale != _scale)
        {
            // OK, we're going to play a dangerous game here for the sake of optimizing resize
            // First, set up a complete clear of all device resources if something goes terribly wrong.
            auto resetDeviceResourcesOnFailure = wil::scope_exit([&]() noexcept {
                _ReleaseDeviceResources();
                });

            // Now let go of a few of the device resources that get in the way of resizing buffers in the swap chain
            _dxgiSurface.Reset();
            _d2dDeviceContext->SetTarget(nullptr);
            _d2dBitmap.Reset();

            // Change the buffer size and recreate the render target (and surface)
            RETURN_IF_FAILED(_dxgiSwapChain->ResizeBuffers(2, clientSize.narrow_width<UINT>(), clientSize.narrow_height<UINT>(), _swapChainDesc.Format, _swapChainDesc.Flags));
            RETURN_IF_FAILED(_PrepareRenderTarget());

            // OK we made it past the parts that can cause errors. We can release our failure handler.
            resetDeviceResourcesOnFailure.release();

            // And persist the new size.
            _displaySizePixels = clientSize;
        }

        if(const auto size = clientSize / glyphCellSize; size != _invalidMap.size())
        {
            _invalidMap.resize(size);
            RETURN_IF_FAILED(InvalidateAll());
        }

        _d2dDeviceContext->BeginDraw();
        _isPainting = true;

        {
            // Get the baseline for this font as that's where we draw from
            DWRITE_LINE_SPACING spacing;
            RETURN_IF_FAILED(_fontRenderData->DefaultTextFormat()->GetLineSpacing(&spacing.method, &spacing.height, &spacing.baseline));

            // Assemble the drawing context information
            _drawingContext = std::make_unique<DrawingContext>(_d2dDeviceContext.Get(),
                _d2dBrushForeground.Get(),
                _d2dBrushBackground.Get(),
                _ShouldForceGrayscaleAA(),
                _dwriteFactory.Get(),
                spacing,
                                                               glyphCellSize.to_d2d_size(),
                _d2dDeviceContext->GetSize(),
                std::nullopt,
                D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        }
    }

    return S_OK;
}
CATCH_RETURN()

// Routine Description:
// - Ends batch drawing and captures any state necessary for presentation
// Arguments:
// - <none>
// Return Value:
// - Any DirectX error, a memory error, etc.
[[nodiscard]] HRESULT DxEngine::EndPaint() noexcept
try
{
    RETURN_HR_IF(E_INVALIDARG, !_isPainting); // invalid to end paint when we're not painting

    auto hr = S_OK;

    if(_haveDeviceResources)
    {
        _isPainting = false;

        // If there's still a clip hanging around, remove it. We're all done.
        LOG_IF_FAILED(_customRenderer->EndClip(_drawingContext.get()));

        hr = _d2dDeviceContext->EndDraw();

        if(SUCCEEDED(hr))
        {
            if(_invalidScroll != til::point{ 0, 0 })
            {
                // Copy `til::rects` into RECT map.
                _presentDirty.assign(_invalidMap.begin(), _invalidMap.end());

                // Scale all dirty rectangles into pixels
                std::transform(_presentDirty.begin(), _presentDirty.end(), _presentDirty.begin(), [&](const til::rect& rc) {
                    return rc.scale_up(_fontRenderData->GlyphCell());
                    });

                // Invalid scroll is in characters, convert it to pixels.
                const auto scrollPixels = (_invalidScroll * _fontRenderData->GlyphCell());

                // The scroll rect is the entire field of cells, but in pixels.
                til::rect scrollArea{ _invalidMap.size() * _fontRenderData->GlyphCell() };

                // Reduce the size of the rectangle by the scroll.
                scrollArea -= til::size{} - scrollPixels;

                // Assign the area to the present storage
                _presentScroll = scrollArea.to_win32_rect();

                // Pass the offset.
                _presentOffset = scrollPixels.to_win32_point();

                // Now fill up the parameters structure from the member variables.
                _presentParams.DirtyRectsCount = gsl::narrow<UINT>(_presentDirty.size());

                // It's not nice to use reinterpret_cast between til::rect and RECT,
                // but to be honest... it does save a ton of type juggling.
                static_assert(sizeof(decltype(_presentDirty)::value_type) == sizeof(RECT));
#pragma warning(suppress : 26490) // Don't use reinterpret_cast (type.1).
                _presentParams.pDirtyRects = reinterpret_cast<RECT*>(_presentDirty.data());

                _presentParams.pScrollOffset = &_presentOffset;
                _presentParams.pScrollRect = &_presentScroll;

                // The scroll rect will be empty if we scrolled >= 1 full screen size.
                // Present1 doesn't like that. So clear it out. Everything will be dirty anyway.
                if(IsRectEmpty(&_presentScroll))
                {
                    _presentParams.pScrollRect = nullptr;
                    _presentParams.pScrollOffset = nullptr;
                }
            }

            _presentReady = true;
        }
        else
        {
            _presentReady = false;
            _ReleaseDeviceResources();
        }
    }

    _invalidMap.reset_all();
    _allInvalid = false;

    _invalidScroll = {};

    return hr;
}
CATCH_RETURN()

// Routine Description:
// - Copies the front surface of the swap chain (the one being displayed)
//   to the back surface of the swap chain (the one we draw on next)
//   so we can draw on top of what's already there.
// Arguments:
// - <none>
// Return Value:
// - Any DirectX error, a memory error, etc.
[[nodiscard]] HRESULT DxEngine::_CopyFrontToBack() noexcept
{
    try
    {
        Microsoft::WRL::ComPtr<ID3D11Resource> backBuffer;
        Microsoft::WRL::ComPtr<ID3D11Resource> frontBuffer;

        RETURN_IF_FAILED(_dxgiSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)));
        RETURN_IF_FAILED(_dxgiSwapChain->GetBuffer(1, IID_PPV_ARGS(&frontBuffer)));

        _d3dDeviceContext->CopyResource(backBuffer.Get(), frontBuffer.Get());
    }
    CATCH_RETURN();

    return S_OK;
}

// Method Description:
// - When the shaders are on, say that we need to keep redrawing every
//   possible frame in case they have some smooth action on every frame tick.
//   It is presumed that if you're using shaders, you're not about performance...
//   You're instead about OOH SHINY. And that's OK. But returning true here is 100%
//   a perf detriment.
[[nodiscard]] bool DxEngine::RequiresContinuousRedraw() noexcept
{
    // We're only going to request continuous redraw if someone is using
    // a pixel shader from a path because we cannot tell if those are using the
    // time parameter or not.
    // And if they are using time, they probably need it to tick continuously.
    //
    // By contrast, the in-built retro effect does NOT need it,
    // so let's not tick for it and save some amount of performance.
    //
    // Finally... if we're not using effects at all... let the render thread
    // go to sleep. It deserves it. That thread works hard. Also it sleeping
    // saves battery power and all sorts of related perf things.
    return _terminalEffectsEnabled && !_pixelShaderPath.empty();
}

// Method Description:
// - Blocks until the engine is able to render without blocking.
// - See https://docs.microsoft.com/en-us/windows/uwp/gaming/reduce-latency-with-dxgi-1-3-swap-chains.
void DxEngine::WaitUntilCanRender() noexcept
{
    // Throttle the DxEngine a bit down to ~60 FPS.
    // This improves throughput for rendering complex or colored text.
    Sleep(8);

    if (_swapChainFrameLatencyWaitableObject)
    {
        WaitForSingleObjectEx(_swapChainFrameLatencyWaitableObject.get(), 100, true);
    }
}

// Routine Description:
// - Takes queued drawing information and presents it to the screen.
// - This is separated out so it can be done outside the lock as it's expensive.
// Arguments:
// - <none>
// Return Value:
// - S_OK on success, E_PENDING to indicate a retry or a relevant DirectX error
[[nodiscard]] HRESULT DxEngine::Present() noexcept
{
    if(_presentReady)
    {
        if(_HasTerminalEffects() && _pixelShaderLoaded)
        {
            const auto hr2 = _PaintTerminalEffects();
            if(FAILED(hr2))
            {
                _pixelShaderLoaded = false;
                LOG_HR_MSG(hr2, "Failed to paint terminal effects. Disabling.");
            }
        }

        try
        {
            auto hr = S_OK;

            auto recreate = false;

            // On anything but the first frame, try partial presentation.
            // We'll do it first because if it fails, we'll try again with full presentation.
            if(!_firstFrame)
            {
                hr = _dxgiSwapChain->Present1(1, 0, &_presentParams);

                // These two error codes are indicated for destroy-and-recreate
                // If we were told to destroy-and-recreate, we're going to skip straight into doing that
                // and not try again with full presentation.
                recreate = hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET;

                // Log this as we actually don't expect it to happen, we just will try again
                // below for robustness of our drawing.
                if(FAILED(hr) && !recreate)
                {
                    LOG_HR(hr);
                }
            }

            // If it's the first frame through, we cannot do partial presentation.
            // Also if partial presentation failed above and we weren't told to skip straight to
            // device recreation.
            // In both of these circumstances, do a full presentation.
            if(_firstFrame || (FAILED(hr) && !recreate))
            {
                hr = _dxgiSwapChain->Present(1, 0);
                _firstFrame = false;

                // These two error codes are indicated for destroy-and-recreate
                recreate = hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET;
            }

            // Now check for failure cases from either presentation mode.
            if(FAILED(hr))
            {
                // If we were told to recreate the device surface, do that.
                if(recreate)
                {
                    // We don't need to end painting here, as the renderer has done it for us.
                    _ReleaseDeviceResources();
                    FAIL_FAST_IF_FAILED(InvalidateAll());
                    return E_PENDING; // Indicate a retry to the renderer.
                }
                // Otherwise, we don't know what to do with this error. Report it.
                else
                {
                    FAIL_FAST_HR(hr);
                }
            }

            // If we are doing full repaints we don't need to copy front buffer to back buffer
            if(!_FullRepaintNeeded())
            {
                // Finally copy the front image (being presented now) onto the backing buffer
                // (where we are about to draw the next frame) so we can draw only the differences
                // next frame.
                RETURN_IF_FAILED(_CopyFrontToBack());
            }

            _presentReady = false;

            _presentDirty.clear();
            _presentOffset = { 0 };
            _presentScroll = { 0 };
            _presentParams = { 0 };
        }
        CATCH_RETURN();
    }

    return S_OK;
}

// Routine Description:
// - This is currently unused.
// Arguments:
// - <none>
// Return Value:
// - S_OK
[[nodiscard]] HRESULT DxEngine::ScrollFrame() noexcept
{
    return S_OK;
}

// Routine Description:
// - This paints in the back most layer of the frame with the background color.
// Arguments:
// - <none>
// Return Value:
// - S_OK
[[nodiscard]] HRESULT DxEngine::PaintBackground() noexcept
try
{
    // If the entire thing is invalid, just use one big clear operation.
    if(_invalidMap.all())
    {
        _d2dDeviceContext->Clear(_backgroundColor);
    }
    else
    {
        // Runs are counts of cells.
        // Use a transform by the size of one cell to convert cells-to-pixels
        // as we clear.
        _d2dDeviceContext->SetTransform(D2D1::Matrix3x2F::Scale(_fontRenderData->GlyphCell().to_d2d_size()));
        for(const auto& rect : _invalidMap.runs())
        {
            // Use aliased.
            // For graphics reasons, it'll look better because it will ensure that
            // the edges are cut nice and sharp (not blended by anti-aliasing).
            // For performance reasons, it takes a lot less work to not
            // do anti-alias blending.
            _d2dDeviceContext->PushAxisAlignedClip(rect.to_d2d_rect(), D2D1_ANTIALIAS_MODE_ALIASED);
            _d2dDeviceContext->Clear(_backgroundColor);
            _d2dDeviceContext->PopAxisAlignedClip();
        }
        _d2dDeviceContext->SetTransform(D2D1::Matrix3x2F::Identity());
    }

    return S_OK;
}
CATCH_RETURN()

// Routine Description:
// - Places one line of text onto the screen at the given position
// Arguments:
// - clusters - Iterable collection of cluster information (text and columns it should consume)
// - coord - Character coordinate position in the cell grid
// - fTrimLeft - Whether or not to trim off the left half of a double wide character
// Return Value:
// - S_OK or relevant DirectX error
[[nodiscard]] HRESULT DxEngine::PaintBufferLine(const gsl::span<const Cluster> clusters,
                                                const COORD coord,
    const bool /*trimLeft*/,
    const bool /*lineWrapped*/) noexcept
    try
{
    // Calculate positioning of our origin.
    const auto origin = (til::point{ coord } * _fontRenderData->GlyphCell()).to_d2d_point();

    // Create the text layout
    RETURN_IF_FAILED(_customLayout->Reset());
    RETURN_IF_FAILED(_customLayout->AppendClusters(clusters));

    // Layout then render the text
    RETURN_IF_FAILED(_customLayout->Draw(_drawingContext.get(), _customRenderer.Get(), origin.x, origin.y));

    return S_OK;
}
CATCH_RETURN()

// Routine Description:
// - Paints lines around cells (draws in pieces of the grid)
// Arguments:
// - lines - Which grid lines (top, left, bottom, right) to draw
// - color - The color to use for drawing the lines
// - cchLine - Length of the line to draw in character cells
// - coordTarget - The X,Y character position in the grid where we should start drawing
//               - We will draw rightward (+X) from here
// Return Value:
// - S_OK or relevant DirectX error
[[nodiscard]] HRESULT DxEngine::PaintBufferGridLines(const GridLineSet lines,
    COLORREF const color,
                                                     const size_t cchLine,
                                                     const COORD coordTarget) noexcept
    try
{
    const auto existingColor = _d2dBrushForeground->GetColor();
    const auto restoreBrushOnExit = wil::scope_exit([&]() noexcept { _d2dBrushForeground->SetColor(existingColor); });

    _d2dBrushForeground->SetColor(_ColorFFromColorRef(color | 0xff000000));

    const auto font = _fontRenderData->GlyphCell().to_d2d_size();
    const D2D_POINT_2F target = { coordTarget.X * font.width, coordTarget.Y * font.height };
    const auto fullRunWidth = font.width * gsl::narrow_cast<unsigned>(cchLine);

    const auto DrawLine = [=](const auto x0, const auto y0, const auto x1, const auto y1, const auto strokeWidth) noexcept {
        _d2dDeviceContext->DrawLine({ x0, y0 }, { x1, y1 }, _d2dBrushForeground.Get(), strokeWidth, _strokeStyle.Get());
    };

    const auto DrawHyperlinkLine = [=](const auto x0, const auto y0, const auto x1, const auto y1, const auto strokeWidth) noexcept {
        _d2dDeviceContext->DrawLine({ x0, y0 }, { x1, y1 }, _d2dBrushForeground.Get(), strokeWidth, _hyperlinkStrokeStyle.Get());
    };

    // NOTE: Line coordinates are centered within the line, so they need to be
    // offset by half the stroke width. For the start coordinate we add half
    // the stroke width, and for the end coordinate we subtract half the width.
    const auto lineMetrics = _fontRenderData->GetLineMetrics();
    if (lines.any(GridLines::Left, GridLines::Right))
    {
        const auto halfGridlineWidth = lineMetrics.gridlineWidth / 2.0f;
        const auto startY = target.y + halfGridlineWidth;
        const auto endY = target.y + font.height - halfGridlineWidth;

        if (lines.test(GridLines::Left))
        {
            auto x = target.x + halfGridlineWidth;
            for(size_t i = 0; i < cchLine; i++, x += font.width)
            {
                DrawLine(x, startY, x, endY, lineMetrics.gridlineWidth);
            }
        }

        if (lines.test(GridLines::Right))
        {
            auto x = target.x + font.width - halfGridlineWidth;
            for(size_t i = 0; i < cchLine; i++, x += font.width)
            {
                DrawLine(x, startY, x, endY, lineMetrics.gridlineWidth);
            }
        }
    }

    if (lines.any(GridLines::Top, GridLines::Bottom))
    {
        const auto halfGridlineWidth = lineMetrics.gridlineWidth / 2.0f;
        const auto startX = target.x + halfGridlineWidth;
        const auto endX = target.x + fullRunWidth - halfGridlineWidth;

        if (lines.test(GridLines::Top))
        {
            const auto y = target.y + halfGridlineWidth;
            DrawLine(startX, y, endX, y, lineMetrics.gridlineWidth);
        }

        if (lines.test(GridLines::Bottom))
        {
            const auto y = target.y + font.height - halfGridlineWidth;
            DrawLine(startX, y, endX, y, lineMetrics.gridlineWidth);
        }
    }

    // In the case of the underline and strikethrough offsets, the stroke width
    // is already accounted for, so they don't require further adjustments.

    if (lines.any(GridLines::Underline, GridLines::DoubleUnderline, GridLines::HyperlinkUnderline))
    {
        const auto halfUnderlineWidth = lineMetrics.underlineWidth / 2.0f;
        const auto startX = target.x + halfUnderlineWidth;
        const auto endX = target.x + fullRunWidth - halfUnderlineWidth;
        const auto y = target.y + lineMetrics.underlineOffset;

        if (lines.test(GridLines::Underline))
        {
            DrawLine(startX, y, endX, y, lineMetrics.underlineWidth);
        }

        if (lines.test(GridLines::HyperlinkUnderline))
        {
            DrawHyperlinkLine(startX, y, endX, y, lineMetrics.underlineWidth);
        }

        if (lines.test(GridLines::DoubleUnderline))
        {
            DrawLine(startX, y, endX, y, lineMetrics.underlineWidth);
            const auto y2 = target.y + lineMetrics.underlineOffset2;
            DrawLine(startX, y2, endX, y2, lineMetrics.underlineWidth);
        }
    }

    if (lines.test(GridLines::Strikethrough))
    {
        const auto halfStrikethroughWidth = lineMetrics.strikethroughWidth / 2.0f;
        const auto startX = target.x + halfStrikethroughWidth;
        const auto endX = target.x + fullRunWidth - halfStrikethroughWidth;
        const auto y = target.y + lineMetrics.strikethroughOffset;

        DrawLine(startX, y, endX, y, lineMetrics.strikethroughWidth);
    }

    return S_OK;
}
CATCH_RETURN()

// Routine Description:
// - Paints an overlay highlight on a portion of the frame to represent selected text
// Arguments:
//  - rect - Rectangle to invert or highlight to make the selection area
// Return Value:
// - S_OK or relevant DirectX error.
[[nodiscard]] HRESULT DxEngine::PaintSelection(const SMALL_RECT rect) noexcept
try
{
    // If a clip rectangle is in place from drawing the text layer, remove it here.
    LOG_IF_FAILED(_customRenderer->EndClip(_drawingContext.get()));

    const auto existingColor = _d2dBrushForeground->GetColor();

    _d2dBrushForeground->SetColor(_selectionBackground);
    const auto resetColorOnExit = wil::scope_exit([&]() noexcept { _d2dBrushForeground->SetColor(existingColor); });

    const auto draw = til::rect{ Viewport::FromExclusive(rect).ToInclusive() }.scale_up(_fontRenderData->GlyphCell()).to_d2d_rect();

    _d2dDeviceContext->FillRectangle(draw, _d2dBrushForeground.Get());

    return S_OK;
}
CATCH_RETURN()

// Routine Description:
// - Does nothing. Our cursor is drawn in CustomTextRenderer::DrawGlyphRun,
//   either above or below the text.
// Arguments:
// - options - unused
// Return Value:
// - S_OK
[[nodiscard]] HRESULT DxEngine::PaintCursor(const CursorOptions& /*options*/) noexcept
{
    return S_OK;
}

// Routine Description:
// - Paint terminal effects.
// Arguments:
// Return Value:
// - S_OK or relevant DirectX error.
[[nodiscard]] HRESULT DxEngine::_PaintTerminalEffects() noexcept
try
{
    // Should have been initialized.
    RETURN_HR_IF(E_NOT_VALID_STATE, !_framebufferCapture);

    // Capture current frame in swap chain to a texture.
    ::Microsoft::WRL::ComPtr<ID3D11Texture2D> swapBuffer;
    RETURN_IF_FAILED(_dxgiSwapChain->GetBuffer(0, IID_PPV_ARGS(&swapBuffer)));
    _d3dDeviceContext->CopyResource(_framebufferCapture.Get(), swapBuffer.Get());

    // Prepare captured texture as input resource to shader program.
    D3D11_TEXTURE2D_DESC desc;
    _framebufferCapture->GetDesc(&desc);

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = desc.MipLevels;
    srvDesc.Format = desc.Format;

    ::Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> shaderResource;
    RETURN_IF_FAILED(_d3dDevice->CreateShaderResourceView(_framebufferCapture.Get(), &srvDesc, &shaderResource));

    // Render the screen quad with shader effects.
    const UINT stride = sizeof(ShaderInput);
    const UINT offset = 0;

    _d3dDeviceContext->OMSetRenderTargets(1, _renderTargetView.GetAddressOf(), nullptr);
    _d3dDeviceContext->IASetVertexBuffers(0, 1, _screenQuadVertexBuffer.GetAddressOf(), &stride, &offset);
    _d3dDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    _d3dDeviceContext->IASetInputLayout(_vertexLayout.Get());
    _d3dDeviceContext->VSSetShader(_vertexShader.Get(), nullptr, 0);
    _d3dDeviceContext->PSSetShader(_pixelShader.Get(), nullptr, 0);
    _d3dDeviceContext->PSSetShaderResources(0, 1, shaderResource.GetAddressOf());
    _d3dDeviceContext->PSSetSamplers(0, 1, _samplerState.GetAddressOf());
    _d3dDeviceContext->PSSetConstantBuffers(0, 1, _pixelShaderSettingsBuffer.GetAddressOf());
    _d3dDeviceContext->Draw(ARRAYSIZE(_screenQuadVertices), 0);

    return S_OK;
}
CATCH_RETURN()

[[nodiscard]] bool DxEngine::_FullRepaintNeeded() const noexcept
{
    // If someone explicitly requested differential rendering off, then we need to invalidate everything
    // so the entire frame is repainted.
    //
    // If terminal effects are on, we must invalidate everything for them to draw correctly.
    // Yes, this will further impact the performance of terminal effects.
    // But we're talking about running the entire display pipeline through a shader for
    // cosmetic effect, so performance isn't likely the top concern with this feature.
    return _forceFullRepaintRendering || _HasTerminalEffects();
}

// Routine Description:
// - Updates the default brush colors used for drawing
// Arguments:
// - textAttributes - Text attributes to use for the brush color
// - renderSettings - The color table and modes required for rendering
// - pData - The interface to console data structures required for rendering
// - usingSoftFont - Whether we're rendering characters from a soft font
// - isSettingDefaultBrushes - Lets us know that these are the default brushes to paint the swapchain background or selection
// Return Value:
// - S_OK or relevant DirectX error.
[[nodiscard]] HRESULT DxEngine::UpdateDrawingBrushes(const TextAttribute& textAttributes,
                                                     const RenderSettings& renderSettings,
                                                     const gsl::not_null<IRenderData*> /*pData*/,
    const bool /*usingSoftFont*/,
    const bool isSettingDefaultBrushes) noexcept
{
    const auto [colorForeground, colorBackground] = renderSettings.GetAttributeColorsWithAlpha(textAttributes);

    const auto usingCleartype = _antialiasingMode == D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE;
    const auto usingTransparency = _defaultBackgroundIsTransparent;
    const bool forceOpaqueBG = !IsWindows11OrGreater() || (usingCleartype && !usingTransparency);

    _foregroundColor = _ColorFFromColorRef(OPACITY_OPAQUE | colorForeground);
    // October 2021: small changes were made to the way BG color interacts with
    // grayscale AA, esp. with regards to acrylic and GH#5098. See comment in
    // _ShouldForceGrayscaleAA for more details.
    _backgroundColor = _ColorFFromColorRef((forceOpaqueBG ? OPACITY_OPAQUE : 0) | colorBackground);

    _d2dBrushForeground->SetColor(_foregroundColor);
    _d2dBrushBackground->SetColor(_backgroundColor);

    // If this flag is set, then we need to update the default brushes too and the swap chain background.
    if(isSettingDefaultBrushes)
    {
        _defaultForegroundColor = _foregroundColor;
        _defaultBackgroundColor = _backgroundColor;

        // If we have a swap chain, set the background color there too so the area
        // outside the chain on a resize can be filled in with an appropriate color value.
        /*if (_dxgiSwapChain)
        {
            const auto dxgiColor = s_RgbaFromColorF(_defaultBackgroundColor);
            RETURN_IF_FAILED(_dxgiSwapChain->SetBackgroundColor(&dxgiColor));
        }*/
    }

    // If we have a drawing context, it may be choosing its antialiasing based
    // on the colors. Update it if it exists.
    // Also record whether we need to render the text with an italic font.
    // We only need to do this here because this is called all the time on painting frames
    // and will update it in a timely fashion. Changing the AA mode or opacity do affect
    // it, but we will always hit updating the drawing brushes so we don't
    // need to update this in those locations.
    if(_drawingContext)
    {
        _drawingContext->forceGrayscaleAA = _ShouldForceGrayscaleAA();
        _drawingContext->useBoldFont = textAttributes.IsIntense() && renderSettings.GetRenderMode(RenderSettings::Mode::IntenseIsBold);
        _drawingContext->useItalicFont = textAttributes.IsItalic();
    }

    if(textAttributes.IsHyperlink())
    {
        _hyperlinkStrokeStyle = (textAttributes.GetHyperlinkId() == _hyperlinkHoveredId) ? _strokeStyle : _dashStrokeStyle;
    }

    // Update pixel shader settings as background color might have changed
    _ComputePixelShaderSettings();

    return S_OK;
}

// Routine Description:
// - Updates the font used for drawing
// - This is the version that complies with the IRenderEngine interface
// Arguments:
// - pfiFontInfoDesired - Information specifying the font that is requested
// - fiFontInfo - Filled with the nearest font actually chosen for drawing
// Return Value:
// - S_OK or relevant DirectX error
[[nodiscard]] HRESULT DxEngine::UpdateFont(const FontInfoDesired& pfiFontInfoDesired, FontInfo& fiFontInfo) noexcept
{
    return UpdateFont(pfiFontInfoDesired, fiFontInfo, {}, {});
}

// Routine Description:
// - Updates the font used for drawing
// Arguments:
// - pfiFontInfoDesired - Information specifying the font that is requested
// - fiFontInfo - Filled with the nearest font actually chosen for drawing
// - features - The map of font features to use
// - axes - The map of font axes to use
// Return Value:
// - S_OK or relevant DirectX error
[[nodiscard]] HRESULT DxEngine::UpdateFont(const FontInfoDesired& pfiFontInfoDesired, FontInfo& fiFontInfo, const std::unordered_map<std::wstring_view, uint32_t>& features, const std::unordered_map<std::wstring_view, float>& axes) noexcept
try
{
    RETURN_IF_FAILED(_fontRenderData->UpdateFont(pfiFontInfoDesired, fiFontInfo, _dpi, features, axes));

    // Prepare the text layout.
    _customLayout = WRL::Make<CustomTextLayout>(_fontRenderData.get());

    return S_OK;
}
CATCH_RETURN();

[[nodiscard]] Viewport DxEngine::GetViewportInCharacters(const Viewport& viewInPixels) const noexcept
{
    const auto widthInChars = base::saturated_cast<short>(viewInPixels.Width() / _fontRenderData->GlyphCell().width);
    const auto heightInChars = base::saturated_cast<short>(viewInPixels.Height() / _fontRenderData->GlyphCell().height);

    return Viewport::FromDimensions(viewInPixels.Origin(), { widthInChars, heightInChars });
}

[[nodiscard]] Viewport DxEngine::GetViewportInPixels(const Viewport& viewInCharacters) const noexcept
{
    const auto widthInPixels = base::saturated_cast<short>(viewInCharacters.Width() * _fontRenderData->GlyphCell().width);
    const auto heightInPixels = base::saturated_cast<short>(viewInCharacters.Height() * _fontRenderData->GlyphCell().height);

    return Viewport::FromDimensions(viewInCharacters.Origin(), { widthInPixels, heightInPixels });
}

// Routine Description:
// - Sets the DPI in this renderer
// Arguments:
// - iDpi - DPI
// Return Value:
// - S_OK
[[nodiscard]] HRESULT DxEngine::UpdateDpi(const int iDpi) noexcept
{
    _dpi = iDpi;

    // The scale factor may be necessary for composition contexts, so save it once here.
    _scale = _dpi / static_cast<float>(USER_DEFAULT_SCREEN_DPI);

    RETURN_IF_FAILED(InvalidateAll());

    // Update pixel shader settings as scale might have changed
    _ComputePixelShaderSettings();

    return S_OK;
}

// Method Description:
// - Get the current scale factor of this renderer. The actual DPI the renderer
//   is USER_DEFAULT_SCREEN_DPI * GetScaling()
// Arguments:
// - <none>
// Return Value:
// - the scaling multiplier of this render engine
float DxEngine::GetScaling() const noexcept
{
    return _scale;
}

// Method Description:
// - This method will update our internal reference for how big the viewport is.
//      Does nothing for DX.
// Arguments:
// - srNewViewport - The bounds of the new viewport.
// Return Value:
// - HRESULT S_OK
[[nodiscard]] HRESULT DxEngine::UpdateViewport(const SMALL_RECT /*srNewViewport*/) noexcept
{
    return S_OK;
}

// Routine Description:
// - Currently unused by this renderer
// Arguments:
// - pfiFontInfoDesired - <unused>
// - pfiFontInfo - <unused>
// - iDpi - <unused>
// Return Value:
// - S_OK
[[nodiscard]] HRESULT DxEngine::GetProposedFont(const FontInfoDesired& pfiFontInfoDesired,
    FontInfo& pfiFontInfo,
                                                const int iDpi) noexcept
try
{
    DxFontRenderData fontRenderData(_dwriteFactory);
    return fontRenderData.UpdateFont(pfiFontInfoDesired, pfiFontInfo, iDpi);
}
CATCH_RETURN();

// Routine Description:
// - Gets the area that we currently believe is dirty within the character cell grid
// Arguments:
// - area - Rectangle describing dirty area in characters.
// Return Value:
// - S_OK
[[nodiscard]] HRESULT DxEngine::GetDirtyArea(gsl::span<const til::rect>& area) noexcept
try
{
    area = _invalidMap.runs();
    return S_OK;
}
CATCH_RETURN();

// Routine Description:
// - Gets the current font size
// Arguments:
// - pFontSize - Filled with the font size.
// Return Value:
// - S_OK
[[nodiscard]] HRESULT DxEngine::GetFontSize(_Out_ COORD* const pFontSize) noexcept
try
{
    const auto size = _fontRenderData->GlyphCell();
    pFontSize->X = size.narrow_width<short>();
    pFontSize->Y = size.narrow_height<short>();
    return S_OK;
}
CATCH_RETURN();

// Routine Description:
// - Currently unused by this renderer.
// Arguments:
// - glyph - The glyph run to process for column width.
// - pResult - True if it should take two columns. False if it should take one.
// Return Value:
// - S_OK or relevant DirectWrite error.
[[nodiscard]] HRESULT DxEngine::IsGlyphWideByFont(const std::wstring_view glyph, _Out_ bool* const pResult) noexcept
try
{
    RETURN_HR_IF_NULL(E_INVALIDARG, pResult);

    const Cluster cluster(glyph, 0); // columns don't matter, we're doing analysis not layout.

    RETURN_IF_FAILED(_customLayout->Reset());
    RETURN_IF_FAILED(_customLayout->AppendClusters({ &cluster, 1 }));

    UINT32 columns = 0;
    RETURN_IF_FAILED(_customLayout->GetColumns(&columns));

    *pResult = columns != 1;

    return S_OK;
}
CATCH_RETURN();

// Method Description:
// - Updates the window's title string.
// Arguments:
// - newTitle: the new string to use for the title of the window
// Return Value:
// - S_OK
[[nodiscard]] HRESULT DxEngine::_DoUpdateTitle(_In_ const std::wstring_view /*newTitle*/) noexcept
{
    if(_hwndTarget != INVALID_HANDLE_VALUE)
    {
        return PostMessageW(_hwndTarget, CM_UPDATE_TITLE, 0, 0) ? S_OK : E_FAIL;
    }
    return S_FALSE;
}

// Routine Description:
// - Helps convert a GDI COLORREF into a Direct2D ColorF
// Arguments:
// - color - GDI color
// Return Value:
// - D2D color
[[nodiscard]] D2D1_COLOR_F DxEngine::_ColorFFromColorRef(const COLORREF color) noexcept
{
    // Converts BGR color order to RGB.
    const UINT32 rgb = ((color & 0x0000FF) << 16) | (color & 0x00FF00) | ((color & 0xFF0000) >> 16);

    switch(_chainMode)
    {
    case SwapChainMode::ForHwnd:
    {
        return D2D1::ColorF(rgb);
    }
    case SwapChainMode::ForComposition:
    {
        // Get the A value we've snuck into the highest byte
        const BYTE a = ((color >> 24) & 0xFF);
        const auto aFloat = a / 255.0f;

        return D2D1::ColorF(rgb, aFloat);
    }
    default:
        FAIL_FAST_HR(E_NOTIMPL);
    }
}

// Routine Description:
// - Updates the selection background color of the DxEngine
// Arguments:
// - color - GDI Color
// Return Value:
// - N/A
void DxEngine::SetSelectionBackground(const COLORREF color, const float alpha) noexcept
{
    _selectionBackground = D2D1::ColorF(GetRValue(color) / 255.0f,
        GetGValue(color) / 255.0f,
        GetBValue(color) / 255.0f,
        alpha);
}

// Routine Description:
// - Changes the antialiasing mode of the renderer. This must be called before
//   _PrepareRenderTarget, otherwise the renderer will default to
//   D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE.
// Arguments:
// - antialiasingMode: a value from the D2D1_TEXT_ANTIALIAS_MODE enum. See:
//          https://docs.microsoft.com/en-us/windows/win32/api/d2d1/ne-d2d1-d2d1_text_antialias_mode
// Return Value:
// - N/A
void DxEngine::SetAntialiasingMode(const D2D1_TEXT_ANTIALIAS_MODE antialiasingMode) noexcept
try
{
    if(_antialiasingMode != antialiasingMode)
    {
        _antialiasingMode = antialiasingMode;
        _recreateDeviceRequested = true;
        LOG_IF_FAILED(InvalidateAll());
    }
}
CATCH_LOG()

// Method Description:
// - Update our tracker of the opacity of our background. We can only
//   effectively render cleartype text onto fully-opaque backgrounds. If we're
//   rendering onto a transparent surface (like acrylic), then cleartype won't
//   work correctly, and will actually just additively blend with the
//   background. This is here to support GH#5098.
// - We'll use this, along with whether cleartype was requested, to manually set
//   the alpha channel of the background brush to 1.0. We need to do that to
//   make cleartype work without blending. However, we don't want to do that too
//   often - if we do that on top of a transparent BG, then the entire swap
//   chain will be fully opaque.
// Arguments:
// - isTransparent: true if our BG is transparent (acrylic, or anything that's not fully opaque)
// Return Value:
// - <none>
void DxEngine::EnableTransparentBackground(const bool isTransparent) noexcept
try
{
    _defaultBackgroundIsTransparent = isTransparent;

    // Make sure we redraw all the cells, to update whether they're actually
    // drawn with cleartype or not.
    // We don't terribly care if this fails.
    LOG_IF_FAILED(InvalidateAll());
}
CATCH_LOG()

// Method Description:
// - Updates our internal tracker for which hyperlink ID we are hovering over
//   This is needed for UpdateDrawingBrushes to know where we need to set a different style
// Arguments:
// - The new link ID we are hovering over
void DxEngine::UpdateHyperlinkHoveredId(const uint16_t hoveredId) noexcept
{
    _hyperlinkHoveredId = hoveredId;
}

// Method Description:
// - Informs this render engine about certain state for this frame at the
//   beginning of this frame. We'll use it to get information about the cursor
//   before PaintCursor is called. This enables the DX renderer to draw the
//   cursor underneath the text.
// - This is called every frame. When the cursor is Off or out of frame, the
//   info's cursorInfo will be set to std::nullopt;
// Arguments:
// - info - a RenderFrameInfo with information about the state of the cursor in this frame.
// Return Value:
// - S_OK
[[nodiscard]] HRESULT DxEngine::PrepareRenderInfo(const RenderFrameInfo& info) noexcept
{
    _drawingContext->cursorInfo = info.cursorInfo;
    return S_OK;
}
