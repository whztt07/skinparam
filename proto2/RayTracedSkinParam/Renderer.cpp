/**
 * Main renderer class
 */

#include "stdafx.h"

#include "Renderer.h"
#include "D3DHelper.h"
#include "ShaderGroup.h"

using namespace RLSkin;
using namespace Utils;

Renderer::Renderer(HWND hwnd, CRect rectView)
	: m_hwnd(hwnd), m_rectView(rectView),
	  m_pMainFrameShader(nullptr),
	  m_pMainProgram(nullptr),
	  m_psgDirectDraw(nullptr)
{
	initRL();
	initShaders();

	D3DHelper::checkFailure(initDX(), _T("Failed to initialize Direct3D"));
	initDXMiscellaneous();
	initDXShaders();
}

Renderer::~Renderer() {
	uninitDXShaders();
	uninitDX();

	uninitShaders();

	rlDeleteBuffers(1, &m_rlTempBuffer);
	rlDeleteFramebuffers(1, &m_rlMainFramebuffer);
	rlDeleteTextures(1, &m_rlMainTexture);
	// Not friendly in vs2012 debugger
	//OpenRLDestroyContext(m_rlContext);
}

void Renderer::onError(RLenum error, const void* privateData, size_t privateSize, const char* message) {
	TString strMessage = Utils::TStringFromANSIString(message);
	TRACE(_T("[RLSkin ERROR %d] %s"), error, strMessage.c_str());

	TStringStream tss;
	tss << _T("ERROR ") << error << _T(": ") << strMessage;
	MessageBox(m_hwnd, tss.str().c_str(), APP_NAME _T(" ERROR"), MB_OK | MB_ICONERROR);
}

void Renderer::onError(RLenum error, const void* privateData, size_t privateSize, const char* message, void* userData) {
	// dispatch to the Renderer instance specified by userData
	static_cast<Renderer*>(userData)->onError(error, privateData, privateSize, message);
}

void Renderer::initRL() {
	// Create OpenRL context
	OpenRLContextAttribute attributes[] = {
//		kOpenRL_RequireHardwareAcceleration,
		kOpenRL_ExcludeCPUCores, 0,
		kOpenRL_DisableHyperthreading, 0,
		kOpenRL_DisableStats, 0,
		kOpenRL_DisableProfiling, 0,
		kOpenRL_DisableTokenStream, 0,
		NULL
	};
	
	m_rlContext = OpenRLCreateContext(attributes, (OpenRLNotify)onError, this);
	OpenRLSetCurrentContext(m_rlContext);

	// Create the framebuffer texture
	rlGenTextures(1, &m_rlMainTexture);
	rlBindTexture(RL_TEXTURE_2D, m_rlMainTexture);
	rlTexImage2D(RL_TEXTURE_2D, 0, RL_RGBA, m_rectView.Width(), m_rectView.Height(), 0, RL_RGBA, RL_FLOAT, NULL);

	// Create the framebuffer object to render to
	// and attach the texture that will store the rendered image.
	rlGenFramebuffers(1, &m_rlMainFramebuffer);
	rlBindFramebuffer(RL_FRAMEBUFFER, m_rlMainFramebuffer);
	rlFramebufferTexture2D(RL_FRAMEBUFFER, RL_COLOR_ATTACHMENT0, RL_TEXTURE_2D, m_rlMainTexture, 0);

	// Setup the viewport
	rlViewport(0, 0, m_rectView.Width(), m_rectView.Height());

	// Create the buffer to copy the rendered image into
	rlGenBuffers(1, &m_rlTempBuffer);
	rlBindBuffer(RL_PIXEL_PACK_BUFFER, m_rlTempBuffer);
	rlBufferData(RL_PIXEL_PACK_BUFFER,
				 m_rectView.Width() * m_rectView.Height() * 4 * sizeof(float),
				 0,
				 RL_STATIC_DRAW);
}

void Renderer::initShaders() {
	m_vppShaders.push_back(&m_pMainFrameShader);
	m_vppPrograms.push_back(&m_pMainProgram);

	m_pMainFrameShader = new FrameShader(_T("Shaders/frame.rlsl.glsl"));
	m_pMainProgram = new Program(std::vector<Shader*>(&m_pMainFrameShader, &m_pMainFrameShader + 1));
}

void Renderer::uninitShaders() {
	for (Program** ppProgram : m_vppPrograms) {
		delete *ppProgram;
		*ppProgram = nullptr;
	}
	m_vppPrograms.clear();
	for (Shader** ppShader : m_vppShaders) {
		delete *ppShader;
		*ppShader = nullptr;
	}
	m_vppShaders.clear();
}

void Renderer::render() {
	rlClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	rlClear(RL_COLOR_BUFFER_BIT);

	rlBindPrimitive(RL_PRIMITIVE, RL_NULL_PRIMITIVE);
	m_pMainProgram->use();
	rlRenderFrame();

	rlBindBuffer(RL_PIXEL_PACK_BUFFER, m_rlTempBuffer);
	rlBindTexture(RL_TEXTURE_2D, m_rlMainTexture);
	rlGetTexImage(RL_TEXTURE_2D, 0, RL_RGBA, RL_FLOAT, NULL);
	float* pixels = (float*)rlMapBuffer(RL_PIXEL_PACK_BUFFER, RL_READ_ONLY);

	// Here is where you copy the data out.
	CComPtr<ID3D11Resource> pResource;
	m_pSRVResult->GetResource(&pResource);
	m_pd3dDeviceContext->UpdateSubresource(pResource, 0, nullptr, pixels, m_rectView.Width() * sizeof(float) * 4, 0);

	// We no longer need access to the original pixels
	rlUnmapBuffer(RL_PIXEL_PACK_BUFFER);
	rlBindBuffer(RL_PIXEL_PACK_BUFFER, NULL);
	
	// Render using d3d backend
	float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
	m_pd3dDeviceContext->ClearRenderTargetView(m_pd3dRenderTargetView, clearColor);
	m_pd3dDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	UINT zero = 0;
	ID3D11Buffer* nullBuffer = nullptr;
	m_pd3dDeviceContext->IASetVertexBuffers(0, 1, &nullBuffer, &zero, &zero);
	m_pd3dDeviceContext->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);

	m_psgDirectDraw->use(m_pd3dDeviceContext);
	m_pd3dDeviceContext->PSSetShaderResources(0, 1, &m_pSRVResult.p);
	ID3D11SamplerState* samplers[] = { m_pd3dLinearSampler.p, m_pd3dPointSampler.p };
	m_pd3dDeviceContext->PSSetSamplers(0, 2, samplers);

	m_pd3dDeviceContext->Draw(6, 0);

	ID3D11ShaderResourceView* pNullSRV = nullptr;
	m_pd3dDeviceContext->PSSetShaderResources(0, 1, &pNullSRV);

	m_pd3dSwapChain->Present(1, 0);
}

HRESULT Renderer::initDX() {
    UINT createDeviceFlags = 0;
#ifdef _DEBUG
    createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_DRIVER_TYPE driverTypes[] = {
        D3D_DRIVER_TYPE_HARDWARE,
        D3D_DRIVER_TYPE_WARP,
        D3D_DRIVER_TYPE_REFERENCE,
    };
    UINT numDriverTypes = ARRAYSIZE(driverTypes);

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1,
		D3D_FEATURE_LEVEL_10_0
    };
	UINT numFeatureLevels = ARRAYSIZE(featureLevels);

    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 1;
	sd.BufferDesc.Width = m_rectView.Width();
	sd.BufferDesc.Height = m_rectView.Height();
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = m_hwnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;

	HRESULT hr;

    for (UINT driverTypeIndex = 0; driverTypeIndex < numDriverTypes; driverTypeIndex++) {
		m_d3dDriverType = driverTypes[driverTypeIndex];
        hr = D3D11CreateDeviceAndSwapChain(nullptr, m_d3dDriverType, nullptr, createDeviceFlags, featureLevels, numFeatureLevels,
			D3D11_SDK_VERSION, &sd, &m_pd3dSwapChain, &m_pd3dDevice, &m_d3dFeatureLevel, &m_pd3dDeviceContext);
        if (SUCCEEDED(hr))
            break;
    }
    if (FAILED(hr))
        return hr;

    // Create a render target view
    CComPtr<ID3D11Texture2D> pBackBuffer;
	hr = m_pd3dSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer);
	if (FAILED(hr))
		return hr;

	hr = m_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &m_pd3dRenderTargetView);
	if (FAILED(hr))
		return hr;

	m_pd3dDeviceContext->OMSetRenderTargets(1, &m_pd3dRenderTargetView.p, nullptr);

	// Setup the viewport
    D3D11_VIEWPORT& vp = m_d3dScreenViewport;
	vp.Width = (float)m_rectView.Width();
	vp.Height = (float)m_rectView.Height();
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
	vp.TopLeftX = (float)m_rectView.left;
	vp.TopLeftY = (float)m_rectView.top;
    m_pd3dDeviceContext->RSSetViewports(1, &vp);

	return S_OK;
}

void Renderer::initDXMiscellaneous() {
	using namespace D3DHelper;
	// Temporary texture for drawing the result
	checkFailure(createShaderResourceView2D(m_pd3dDevice, m_rectView.Width(), m_rectView.Height(), DXGI_FORMAT_R32G32B32A32_FLOAT,
		&m_pSRVResult, D3D11_BIND_SHADER_RESOURCE), _T("Failed to create SRV for OpenRL result"));

	checkFailure(createSamplerState(m_pd3dDevice, D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP,
		D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP, &m_pd3dLinearSampler),
		_T("Failed to create linear sampler state"));

	checkFailure(createSamplerState(m_pd3dDevice, D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP,
		D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP, &m_pd3dPointSampler),
		_T("Failed to create point sampler state"));
}

void Renderer::initDXShaders() {
	// Screen-space shaders
	D3D11_INPUT_ELEMENT_DESC empty_layout[] = { { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
	m_psgDirectDraw = new ShaderGroup(m_pd3dDevice, _T("DirectDraw.fx"), empty_layout, 0,
		"VS_Quad", nullptr, nullptr, "PS_Point_UpsideDown");
}

void Renderer::uninitDXShaders() {
	delete m_psgDirectDraw;
	m_psgDirectDraw = nullptr;
}

void Renderer::uninitDX() {
	if (m_pd3dDeviceContext)
		m_pd3dDeviceContext->ClearState();
}
