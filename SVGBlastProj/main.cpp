#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <d3d11.h>
#include <tchar.h>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <shellapi.h>
#include <shobjidl.h> // IFileOpenDialog

#include "resource.h"
#include "resvg.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "imgui_internal.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#pragma comment(lib, "resvg.lib")
#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Userenv.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")

static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static bool g_SwapChainOccluded = false;
static UINT g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
static size_t g_MaxTextureSize = 8192; // Will be set based on feature level
const float clear_color[4] = {1.0f, 1.0f, 1.0f, 1.00f};

resvg_render_tree* MainTree = NULL;
size_t MainPixelsBufferSize = 0;
uint8_t* MainPixels = NULL; //this allocated once at maximum size and reused
resvg_size g_MainSize = {0, 0};
resvg_options* g_RESVG_Options = NULL;

bool g_ShowMainWindow = true;
bool g_ShowHelp = false;
std::vector<std::wstring> SVG_Path_List;
std::vector<std::string> SVG_Path_List_UTF8;
std::vector<ImTextureID> g_Textures;

std::filesystem::path* pMainFile = NULL;

// Thumbnail view state
float g_ThumbnailScrollY = 0.0f; // Preserved scroll position for thumbnail view

// Thumbnail loading progress
volatile LONG g_ThumbnailsLoaded = 0;
volatile LONG g_ThumbnailsTotal = 0;

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Thread pool for thumbnail generation
struct ThumbnailResult
{
	int index;
	std::vector<uint8_t> rgbaData;
	int width;
	int height;
};

// Win32 synchronization
CRITICAL_SECTION g_ResultCS;
std::vector<ThumbnailResult> g_ResultQueue;

CRITICAL_SECTION g_JobCS;
HANDLE g_JobSemaphore = NULL; // Signals when jobs are available
std::vector<int> g_JobQueue;  // indices into SVG_Path_List
std::atomic<bool> g_ShutdownWorkers{false};
std::vector<HANDLE> g_WorkerThreads;

// ============================================================================
// Background Re-render System
// ============================================================================
// This system handles high-quality re-rendering of the main SVG at different
// zoom levels without blocking the main thread.
//
// Thread safety model:
// - Main thread: reads g_RenderResult, writes g_RenderRequest
// - Worker thread: reads g_RenderRequest, writes g_RenderResult
// - Atomics and critical sections ensure safe handoff
// ============================================================================

struct RenderRequest
{
	resvg_render_tree* tree; // The tree to render (owned by main thread, read-only for worker)
	int texWidth;			 // Output texture width (pixels)
	int texHeight;			 // Output texture height (pixels)
	float zoom;
	// Viewport in SVG coordinates (the sub-rect of the SVG to render)
	float svgViewX, svgViewY, svgViewW, svgViewH;
	LONG requestId; // Incremented for each new request, used to detect stale results
};

struct RenderResult
{
	int texWidth;
	int texHeight;
	float zoom;
	// Echo back the viewport so main thread knows what region this texture covers
	float svgViewX, svgViewY, svgViewW, svgViewH;
	LONG requestId; // Matches the request this result is for
	bool valid;		// True if rendering succeeded
};

// Synchronization for background re-render
CRITICAL_SECTION g_RenderRequestCS;
CRITICAL_SECTION g_RenderResultCS;
CRITICAL_SECTION g_MainPixelsCS; // Protects access to MainPixels buffer
HANDLE g_RenderSemaphore = NULL; // Signals when a render job is available
HANDLE g_RenderThread = NULL;
std::atomic<bool> g_ShutdownRenderThread{false};
std::atomic<LONG> g_CurrentRequestId{0};	 // Monotonically increasing request ID
std::atomic<bool> g_RenderInProgress{false}; // True while background render is active

// The current pending request (protected by g_RenderRequestCS)
RenderRequest g_RenderRequest = {};
bool g_HasPendingRequest = false;

// The completed result waiting to be picked up (protected by g_RenderResultCS)
RenderResult g_RenderResult = {};
bool g_HasPendingResult = false;

// Forward declarations
DWORD WINAPI BackgroundRenderThread(LPVOID lpParam);
void StartBackgroundRenderThread();
void StopBackgroundRenderThread();
void RequestBackgroundRender(resvg_render_tree* tree, int texWidth, int texHeight, float zoom, float svgViewX,
							 float svgViewY, float svgViewW, float svgViewH);
bool TryGetRenderResult(RenderResult& outResult);
void DrawRenderingEffect(ImVec2 center, float time);

int LaydownThumbnails();

std::wstring OpenSvgFileDialog(HWND hwndOwner)
{
	std::wstring result;

	HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

	if (FAILED(hr))
		return result;

	bool needsUninit = SUCCEEDED(hr);

	IFileOpenDialog* pFileOpen = nullptr;

	hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFileOpen));

	if (SUCCEEDED(hr))
	{
		// File type filter
		COMDLG_FILTERSPEC filters[] = {{L"SVG Files (*.svg)", L"*.svg"}, {L"All Files (*.*)", L"*.*"}};

		pFileOpen->SetFileTypes(_countof(filters), filters);
		pFileOpen->SetDefaultExtension(L"svg");
		pFileOpen->SetTitle(L"Open SVG File");

		// Show dialog
		hr = pFileOpen->Show(hwndOwner);
		if (SUCCEEDED(hr))
		{
			IShellItem* pItem = nullptr;
			hr = pFileOpen->GetResult(&pItem);
			if (SUCCEEDED(hr))
			{
				PWSTR filePath = nullptr;
				hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &filePath);
				if (SUCCEEDED(hr))
				{
					result = filePath;
					CoTaskMemFree(filePath);
				}
				pItem->Release();
			}
		}
		else
		{
			// User cancelled or closed the dialog
			pFileOpen->Release();
			if (needsUninit)
				CoUninitialize();

			exit(0);
		}

		pFileOpen->Release();
	}

	if (needsUninit)
		CoUninitialize();

	return result;
}

// Write callback for stb_image_write
static void stbi_write_callback(void* context, void* data, int size)
{
	FILE* f = (FILE*)context;
	fwrite(data, 1, size, f);
}

std::wstring SavePngFileDialog(HWND hwndOwner, const std::wstring& defaultName)
{
	std::wstring result;

	HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	if (FAILED(hr))
		return result;

	bool needsUninit = SUCCEEDED(hr);

	IFileSaveDialog* pFileSave = nullptr;

	hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFileSave));

	if (SUCCEEDED(hr))
	{
		COMDLG_FILTERSPEC filters[] = {{L"PNG Image (*.png)", L"*.png"}};

		pFileSave->SetFileTypes(_countof(filters), filters);
		pFileSave->SetDefaultExtension(L"png");
		pFileSave->SetTitle(L"Save as PNG");
		pFileSave->SetFileName(defaultName.c_str());

		hr = pFileSave->Show(hwndOwner);
		if (SUCCEEDED(hr))
		{
			IShellItem* pItem = nullptr;
			hr = pFileSave->GetResult(&pItem);
			if (SUCCEEDED(hr))
			{
				PWSTR filePath = nullptr;
				hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &filePath);
				if (SUCCEEDED(hr))
				{
					result = filePath;
					CoTaskMemFree(filePath);
				}
				pItem->Release();
			}
		}

		pFileSave->Release();
	}

	if (needsUninit)
		CoUninitialize();

	return result;
}

void CreateRenderTarget()
{
	ID3D11Texture2D* pBackBuffer = NULL;
	g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
	if (pBackBuffer)
	{
		g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
		if (g_mainRenderTargetView == nullptr)
		{
			MessageBox(NULL, _T("Failed to create render target view."), _T("Error"), MB_OK | MB_ICONERROR);
		}
	}
	pBackBuffer->Release();
}

void CleanupRenderTarget()
{
	if (g_mainRenderTargetView)
	{
		g_mainRenderTargetView->Release();
		g_mainRenderTargetView = nullptr;
	}
}

void CleanupDeviceD3D()
{
	CleanupRenderTarget();
	if (g_pSwapChain)
	{
		g_pSwapChain->Release();
		g_pSwapChain = nullptr;
	}
	if (g_pd3dDeviceContext)
	{
		g_pd3dDeviceContext->Release();
		g_pd3dDeviceContext = nullptr;
	}
	if (g_pd3dDevice)
	{
		g_pd3dDevice->Release();
		g_pd3dDevice = nullptr;
	}
}

bool CreateDeviceD3D(HWND hWnd)
{
	// Setup swap chain
	// This is a basic setup. Optimally could use e.g. DXGI_SWAP_EFFECT_FLIP_DISCARD and handle fullscreen mode differently. See #8979 for suggestions.
	DXGI_SWAP_CHAIN_DESC sd;
	ZeroMemory(&sd, sizeof(sd));
	sd.BufferCount = 2;
	sd.BufferDesc.Width = 0;
	sd.BufferDesc.Height = 0;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferDesc.RefreshRate.Numerator = 60;
	sd.BufferDesc.RefreshRate.Denominator = 1;
	sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = hWnd;
	sd.SampleDesc.Count = 1;
	sd.SampleDesc.Quality = 0;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	UINT createDeviceFlags = 0;
	//createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
	D3D_FEATURE_LEVEL featureLevel;
	const D3D_FEATURE_LEVEL featureLevelArray[2] = {
		D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_0,
	};
	HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
												featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
												&g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
	if (res == DXGI_ERROR_UNSUPPORTED) // Try high-performance WARP software driver if hardware is not available.
		res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags,
											featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice,
											&featureLevel, &g_pd3dDeviceContext);
	if (res != S_OK)
		return false;

	// Set max texture size based on feature level
	g_MaxTextureSize = (featureLevel >= D3D_FEATURE_LEVEL_11_0) ? 16384 : 8192;
	MainPixelsBufferSize = g_MaxTextureSize * g_MaxTextureSize * 4;
	MainPixels = (uint8_t*)malloc(MainPixelsBufferSize);
	if (MainPixels == NULL)
	{
		CleanupDeviceD3D();
		MessageBox(NULL, _T("Failed to allocate memory for main pixel buffer."), _T("Error"), MB_OK | MB_ICONERROR);
		return false;
	}
	CreateRenderTarget();
	return true;
}

static void ReleaseTexture_D3D11(ImTextureID texId)
{
	if (texId)
	{
		ID3D11ShaderResourceView* srv = (ID3D11ShaderResourceView*)(intptr_t)texId;
		srv->Release();
	}
}

static void DrawCheckerBackground(ImVec2 p0, ImVec2 p1, ImU32 colA, ImU32 colB, float cellSize)
{
	ImDrawList* dl = ImGui::GetWindowDrawList();

	if (p1.x <= p0.x || p1.y <= p0.y)
		return;

	// Clip to this region so we don't draw outside it
	dl->PushClipRect(p0, p1, true);

	float startX = p0.x;
	float startY = p0.y;

	int cols = (int)ImCeil((p1.x - p0.x) / cellSize);
	int rows = (int)ImCeil((p1.y - p0.y) / cellSize);

	for (int y = 0; y < rows; ++y)
	{
		for (int x = 0; x < cols; ++x)
		{
			ImU32 col = ((x + y) & 1) ? colA : colB;

			ImVec2 a(startX + x * cellSize, startY + y * cellSize);
			ImVec2 b(a.x + cellSize, a.y + cellSize);

			// Clamp to bounds
			if (b.x > p1.x)
				b.x = p1.x;
			if (b.y > p1.y)
				b.y = p1.y;

			dl->AddRectFilled(a, b, col);
		}
	}

	dl->PopClipRect();
}

std::string WideToUtf8(const std::wstring& wide)
{
	if (wide.empty())
		return {};

	// Convert without relying on a null terminator; this avoids size/off-by-one bugs.
	int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), nullptr, 0, nullptr, nullptr);

	if (size <= 0)
		return {};

	std::string out((size_t)size, ' ');

	WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), out.data(), size, nullptr, nullptr);

	return out;
}

DWORD WINAPI ThumbnailWorkerThread(LPVOID lpParam)
{
	(void)lpParam;

	const int THUMB_SIZE = 256;
	const size_t BUFFER_SIZE = THUMB_SIZE * THUMB_SIZE * 4;
	uint8_t pixels[BUFFER_SIZE];

	while (TRUE)
	{
		// Wait for a job or shutdown
		WaitForSingleObject(g_JobSemaphore, INFINITE);
		if (g_ShutdownWorkers.load(std::memory_order_acquire))
		{
			return 0;
		}

		// Get next job
		int jobIndex = -1;
		EnterCriticalSection(&g_JobCS);
		if (!g_JobQueue.empty())
		{
			jobIndex = g_JobQueue.back();
			g_JobQueue.pop_back();
		}
		LeaveCriticalSection(&g_JobCS);

		if (jobIndex < 0)
			continue;

		if (jobIndex >= SVG_Path_List_UTF8.size())
		{
			MessageBox(NULL, _T("Thumbnail worker received invalid job index. Exiting"), _T("Error"),
					   MB_OK | MB_ICONERROR);
			exit(1);
		}
		std::string utf8Path(SVG_Path_List_UTF8[jobIndex]);

		// Parse the SVG
		resvg_render_tree* tree = NULL;
		int err = resvg_parse_tree_from_file(utf8Path.c_str(), g_RESVG_Options, &tree);

		if (err != RESVG_OK || !tree)
		{
			// Push empty result to maintain index consistency
			ThumbnailResult result;
			result.index = jobIndex;
			result.width = 0;
			result.height = 0;

			EnterCriticalSection(&g_ResultCS);
			g_ResultQueue.push_back(std::move(result));
			LeaveCriticalSection(&g_ResultCS);

			InterlockedIncrement(&g_ThumbnailsLoaded);
			continue;
		}

		// Get the bounding box of actual content (not the viewBox)
		// This gives us the tight bounds around all shapes
		resvg_rect bbox;
		bool hasBbox = resvg_get_image_bbox(tree, &bbox);

		float srcX, srcY, srcW, srcH;

		if (hasBbox && bbox.width > 0 && bbox.height > 0)
		{
			// Use the content bounding box
			srcX = bbox.x;
			srcY = bbox.y;
			srcW = bbox.width;
			srcH = bbox.height;
		}
		else
		{
			// Fallback to full image size
			resvg_size size = resvg_get_image_size(tree);
			srcX = 0;
			srcY = 0;
			srcW = size.width;
			srcH = size.height;
		}

		// Calculate scale to fit content into 256x256 while maintaining aspect ratio
		float scaleX = (float)THUMB_SIZE / srcW;
		float scaleY = (float)THUMB_SIZE / srcH;
		float scale = (scaleX < scaleY) ? scaleX : scaleY; // Use smaller to fit

		// Calculate the size of the scaled content
		float scaledW = srcW * scale;
		float scaledH = srcH * scale;

		// Calculate offset to center the content in the thumbnail
		float offsetX = (THUMB_SIZE - scaledW) * 0.5f;
		float offsetY = (THUMB_SIZE - scaledH) * 0.5f;

		// Build transform: first translate to move bbox origin, then scale, then center
		// Transform order: translate(-srcX, -srcY) -> scale -> translate(offsetX, offsetY)
		// Combined: new_x = (x - srcX) * scale + offsetX
		//           new_y = (y - srcY) * scale + offsetY
		// Matrix form: a=scale, d=scale, e=-srcX*scale+offsetX, f=-srcY*scale+offsetY
		resvg_transform transform;
		transform.a = scale;
		transform.b = 0.0f;
		transform.c = 0.0f;
		transform.d = scale;
		transform.e = -srcX * scale + offsetX;
		transform.f = -srcY * scale + offsetY;

		// Clear buffer to transparent
		memset(pixels, 0, BUFFER_SIZE);

		// Render
		resvg_render(tree, transform, THUMB_SIZE, THUMB_SIZE, (char*)pixels);

		// Clean up the tree
		resvg_tree_destroy(tree);

		// Copy to result
		ThumbnailResult result;
		result.index = jobIndex;
		result.width = THUMB_SIZE;
		result.height = THUMB_SIZE;
		result.rgbaData.resize(BUFFER_SIZE);
		memcpy(result.rgbaData.data(), pixels, BUFFER_SIZE);

		// Push result
		EnterCriticalSection(&g_ResultCS);
		g_ResultQueue.push_back(std::move(result));
		LeaveCriticalSection(&g_ResultCS);

		InterlockedIncrement(&g_ThumbnailsLoaded);
	}
}

void StartThumbnailWorkers()
{
	DWORD cpuCount = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
	DWORD numThreads = (cpuCount > 2) ? (cpuCount - 2) : 1;
	// Keep worker count sane and compatible with WaitForMultipleObjects (MAXIMUM_WAIT_OBJECTS=64)
	if (numThreads > 16)
		numThreads = 16;

	InitializeCriticalSection(&g_ResultCS);
	InitializeCriticalSection(&g_JobCS);

	// Create semaphore with initial count 0, max count = number of SVGs
	g_JobSemaphore = CreateSemaphoreW(NULL, 0, (LONG)SVG_Path_List.size() + numThreads, NULL);

	g_ShutdownWorkers.store(false, std::memory_order_release);

	for (DWORD i = 0; i < numThreads; i++)
	{
		HANDLE hThread = CreateThread(NULL, 0, ThumbnailWorkerThread, NULL, 0, NULL);
		if (hThread)
			g_WorkerThreads.push_back(hThread);
	}
}

void StopThumbnailWorkers()
{
	g_ShutdownWorkers.store(true, std::memory_order_release);

	// Release semaphore for each worker so they can check shutdown flag
	if (g_JobSemaphore)
		ReleaseSemaphore(g_JobSemaphore, (LONG)g_WorkerThreads.size(), NULL);

	// Wait for all workers to finish
	if (!g_WorkerThreads.empty())
	{
		WaitForMultipleObjects((DWORD)g_WorkerThreads.size(), g_WorkerThreads.data(), TRUE, INFINITE);
	}

	// Close handles
	for (HANDLE h : g_WorkerThreads)
		CloseHandle(h);
	g_WorkerThreads.clear();

	if (g_JobSemaphore)
	{
		CloseHandle(g_JobSemaphore);
		g_JobSemaphore = NULL;
	}

	DeleteCriticalSection(&g_ResultCS);
	DeleteCriticalSection(&g_JobCS);
}

// ============================================================================
// Background Re-render Thread Implementation
// ============================================================================

DWORD WINAPI BackgroundRenderThread(LPVOID lpParam)
{
	(void)lpParam;

	while (true)
	{
		// Wait for a render request or shutdown signal
		WaitForSingleObject(g_RenderSemaphore, INFINITE);

		if (g_ShutdownRenderThread.load(std::memory_order_acquire))
			return 0;

		// Get the request
		RenderRequest request = {};
		bool hasRequest = false;

		EnterCriticalSection(&g_RenderRequestCS);
		if (g_HasPendingRequest)
		{
			request = g_RenderRequest;
			g_HasPendingRequest = false;
			hasRequest = true;
		}
		LeaveCriticalSection(&g_RenderRequestCS);

		if (!hasRequest || !request.tree || request.texWidth <= 0 || request.texHeight <= 0)
			continue;

		// Check if this request is still current (not superseded by a newer one)
		LONG currentId = g_CurrentRequestId.load(std::memory_order_acquire);
		if (request.requestId != currentId)
			continue; // Stale request, skip it

		// Signal that rendering is in progress
		g_RenderInProgress.store(true, std::memory_order_release);

		// Build transform that maps the SVG viewport rect to the output texture.
		// We want: svgViewX..svgViewX+svgViewW  ->  0..texWidth
		//          svgViewY..svgViewY+svgViewH  ->  0..texHeight
		float scaleX = (float)request.texWidth / request.svgViewW;
		float scaleY = (float)request.texHeight / request.svgViewH;

		resvg_transform transform;
		transform.a = scaleX;
		transform.b = 0.0f;
		transform.c = 0.0f;
		transform.d = scaleY;
		transform.e = -request.svgViewX * scaleX;
		transform.f = -request.svgViewY * scaleY;

		// Render to MainPixels with mutex protection
		size_t bufferSize = (size_t)request.texWidth * request.texHeight * 4;

		EnterCriticalSection(&g_MainPixelsCS);
		ZeroMemory(MainPixels, bufferSize);
		resvg_render(request.tree, transform, request.texWidth, request.texHeight, (char*)MainPixels);
		LeaveCriticalSection(&g_MainPixelsCS);

		// Signal that rendering is complete
		g_RenderInProgress.store(false, std::memory_order_release);

		// Check again if request is still current after rendering
		currentId = g_CurrentRequestId.load(std::memory_order_acquire);
		if (request.requestId != currentId)
			continue; // Request was superseded during rendering, discard result

		// Store the result metadata (pixels are in MainPixels, protected by g_MainPixelsCS)
		EnterCriticalSection(&g_RenderResultCS);
		g_RenderResult.texWidth = request.texWidth;
		g_RenderResult.texHeight = request.texHeight;
		g_RenderResult.zoom = request.zoom;
		g_RenderResult.svgViewX = request.svgViewX;
		g_RenderResult.svgViewY = request.svgViewY;
		g_RenderResult.svgViewW = request.svgViewW;
		g_RenderResult.svgViewH = request.svgViewH;
		g_RenderResult.requestId = request.requestId;
		g_RenderResult.valid = true;
		g_HasPendingResult = true;
		LeaveCriticalSection(&g_RenderResultCS);
	}
}

void StartBackgroundRenderThread()
{
	InitializeCriticalSection(&g_RenderRequestCS);
	InitializeCriticalSection(&g_RenderResultCS);
	InitializeCriticalSection(&g_MainPixelsCS);

	// Semaphore: initial count 0, max count large enough
	g_RenderSemaphore = CreateSemaphoreW(NULL, 0, 1000, NULL);

	g_ShutdownRenderThread.store(false, std::memory_order_release);
	g_CurrentRequestId.store(0, std::memory_order_release);

	g_RenderThread = CreateThread(NULL, 0, BackgroundRenderThread, NULL, 0, NULL);
}

void StopBackgroundRenderThread()
{
	g_ShutdownRenderThread.store(true, std::memory_order_release);

	// Wake up the thread so it can see the shutdown flag
	if (g_RenderSemaphore)
		ReleaseSemaphore(g_RenderSemaphore, 1, NULL);

	// Wait for thread to finish
	if (g_RenderThread)
	{
		WaitForSingleObject(g_RenderThread, INFINITE);
		CloseHandle(g_RenderThread);
		g_RenderThread = NULL;
	}

	if (g_RenderSemaphore)
	{
		CloseHandle(g_RenderSemaphore);
		g_RenderSemaphore = NULL;
	}

	DeleteCriticalSection(&g_RenderRequestCS);
	DeleteCriticalSection(&g_RenderResultCS);
	DeleteCriticalSection(&g_MainPixelsCS);
}

void RequestBackgroundRender(resvg_render_tree* tree, int texWidth, int texHeight, float zoom, float svgViewX,
							 float svgViewY, float svgViewW, float svgViewH)
{
	// Increment the request ID to invalidate any in-flight render
	LONG newId = g_CurrentRequestId.fetch_add(1, std::memory_order_acq_rel) + 1;

	// Set up the new request
	EnterCriticalSection(&g_RenderRequestCS);
	g_RenderRequest.tree = tree;
	g_RenderRequest.texWidth = texWidth;
	g_RenderRequest.texHeight = texHeight;
	g_RenderRequest.zoom = zoom;
	g_RenderRequest.svgViewX = svgViewX;
	g_RenderRequest.svgViewY = svgViewY;
	g_RenderRequest.svgViewW = svgViewW;
	g_RenderRequest.svgViewH = svgViewH;
	g_RenderRequest.requestId = newId;
	g_HasPendingRequest = true;
	LeaveCriticalSection(&g_RenderRequestCS);

	// Signal the render thread
	ReleaseSemaphore(g_RenderSemaphore, 1, NULL);
}

bool TryGetRenderResult(RenderResult& outResult)
{
	bool hasResult = false;

	EnterCriticalSection(&g_RenderResultCS);
	if (g_HasPendingResult)
	{
		// Only return results that match the current request ID
		LONG currentId = g_CurrentRequestId.load(std::memory_order_acquire);
		if (g_RenderResult.requestId == currentId)
		{
			outResult = g_RenderResult;
			hasResult = true;
		}
		// Clear the result either way
		g_RenderResult = {};
		g_HasPendingResult = false;
	}
	LeaveCriticalSection(&g_RenderResultCS);

	return hasResult;
}

void QueueAllThumbnailJobs()
{
	EnterCriticalSection(&g_JobCS);
	for (int i = 0; i < (int)SVG_Path_List.size(); i++)
	{
		g_JobQueue.push_back(i);
	}
	LeaveCriticalSection(&g_JobCS);

	// Signal that jobs are available
	ReleaseSemaphore(g_JobSemaphore, (LONG)SVG_Path_List.size(), NULL);
}

std::vector<std::wstring> ListSVGFiles(const std::wstring& dir)
{
	namespace fs = std::filesystem;
	std::vector<std::wstring> files;

	for (auto& e : fs::directory_iterator(dir))
	{
		if (!e.is_regular_file())
			continue;
		auto ext = e.path().extension().wstring();
		std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
		if (ext == L".svg")
			files.push_back(e.path().wstring());
	}

	std::sort(files.begin(), files.end());
	return files;
}

// Zoom and pan state
struct ViewState
{
	float zoom = 1.0f;		   // Current zoom level (1.0 = 100%)
	ImVec2 pan = {0.0f, 0.0f}; // Pan offset in screen pixels

	// Original SVG dimensions
	int svgWidth = 0;
	int svgHeight = 0;

	// Zoom limits
	static constexpr float MIN_ZOOM = 0.1f;	  // 10%
	static constexpr float MAX_ZOOM = 256.0f; // 25600% - practically unlimited

	// Re-render timing
	ULONGLONG lastZoomTime = 0;
	bool needsRerender = false;
	float lastRenderedZoom = 1.0f;
	float renderRequestedZoom = 1.0f; // Zoom level of the last background render request

	// Viewport tracking: the SVG-space rect that the current texture covers
	float texSvgViewX = 0, texSvgViewY = 0, texSvgViewW = 0, texSvgViewH = 0;
	bool hasViewportTexture = false; // True when current texture is a viewport render (not full SVG)

	// Dragging state
	bool isDragging = false;
	ImVec2 dragStartPos = {0.0f, 0.0f};
	ImVec2 panAtDragStart = {0.0f, 0.0f};

	// Track last pan for detecting when re-render is needed after panning
	ImVec2 lastRenderedPan = {0.0f, 0.0f};

	void Reset()
	{
		zoom = 1.0f;
		pan = {0.0f, 0.0f};
		needsRerender = false;
		lastRenderedZoom = 1.0f;
		renderRequestedZoom = 1.0f;
		hasViewportTexture = false;
		lastRenderedPan = {0.0f, 0.0f};
	}

	float ClampZoom(float z) const
	{
		if (z < MIN_ZOOM)
			return MIN_ZOOM;
		if (z > MAX_ZOOM)
			return MAX_ZOOM;
		return z;
	}

	// Check if the full zoomed SVG fits within a single texture
	bool FitsInSingleTexture() const
	{
		int zoomedW = (int)(svgWidth * zoom);
		int zoomedH = (int)(svgHeight * zoom);
		return zoomedW <= (int)g_MaxTextureSize && zoomedH <= (int)g_MaxTextureSize;
	}

	// Calculate the visible SVG-space rect given current viewport
	// contentPos/contentSize = screen-space content area
	void CalcVisibleSvgRect(ImVec2 contentPos, ImVec2 contentSize, float& outSvgX, float& outSvgY, float& outSvgW,
							float& outSvgH) const
	{
		// Image top-left in screen space
		float contentCenterX = contentPos.x + contentSize.x * 0.5f;
		float contentCenterY = contentPos.y + contentSize.y * 0.5f;
		float displayW = svgWidth * zoom;
		float displayH = svgHeight * zoom;
		float imgScreenX = contentCenterX - displayW * 0.5f + pan.x;
		float imgScreenY = contentCenterY - displayH * 0.5f + pan.y;

		// Visible screen rect = contentPos .. contentPos+contentSize
		// Convert screen rect to SVG coordinates:
		// svgX = (screenX - imgScreenX) / zoom
		float visLeft = (contentPos.x - imgScreenX) / zoom;
		float visTop = (contentPos.y - imgScreenY) / zoom;
		float visRight = (contentPos.x + contentSize.x - imgScreenX) / zoom;
		float visBottom = (contentPos.y + contentSize.y - imgScreenY) / zoom;

		// Add margin (10% on each side) so we render a bit beyond the viewport
		// This reduces re-renders when panning slightly
		float marginX = (visRight - visLeft) * 0.1f;
		float marginY = (visBottom - visTop) * 0.1f;
		visLeft -= marginX;
		visTop -= marginY;
		visRight += marginX;
		visBottom += marginY;

		outSvgX = visLeft;
		outSvgY = visTop;
		outSvgW = visRight - visLeft;
		outSvgH = visBottom - visTop;
	}
};

// Returns an ImGui texture handle (ImTextureID). On D3D11 backends, this is typically an ID3D11ShaderResourceView*.
static ImTextureID UploadTexture_D3D11(ID3D11Device* device, const unsigned char* rgba, int w, int h)
{
	if (!device || !rgba || w <= 0 || h <= 0)
		return (ImTextureID)0;

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = (UINT)w;
	desc.Height = (UINT)h;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	D3D11_SUBRESOURCE_DATA init = {};
	init.pSysMem = rgba;
	init.SysMemPitch = (UINT)(w * 4);

	ID3D11Texture2D* tex = nullptr;
	HRESULT hr = device->CreateTexture2D(&desc, &init, &tex);
	if (FAILED(hr) || !tex)
		return (ImTextureID)0;

	ID3D11ShaderResourceView* srv = nullptr;
	hr = device->CreateShaderResourceView(tex, nullptr, &srv);
	tex->Release();

	if (FAILED(hr) || !srv)
		return (ImTextureID)0;

	// Cast in a way that works whether ImTextureID is void* or an integer-sized type
	return (ImTextureID)(intptr_t)srv;
}

// Win32 message handler
// You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
// - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application, or clear/overwrite your copy of the mouse data.
// - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application, or clear/overwrite your copy of the keyboard data.
// Generally you may always pass all inputs to dear imgui, and hide them from your application based on those two flags.
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
		return true;

	switch (msg)
	{
	case WM_ENTERSIZEMOVE:
		// Start a timer to keep rendering during resize/move
		SetTimer(hWnd, 1, 10, nullptr); // ~100fps timer
		return 0;
	case WM_EXITSIZEMOVE:
		KillTimer(hWnd, 1);
		return 0;
	case WM_TIMER:
		if (wParam == 1)
		{
			// During modal resize, we need to render here since main loop is blocked
			if (g_pd3dDevice != nullptr && g_mainRenderTargetView != nullptr && !g_ShowMainWindow)
			{
				ImGui_ImplDX11_NewFrame();
				ImGui_ImplWin32_NewFrame();
				ImGui::NewFrame();

				LaydownThumbnails();

				ImGui::Render();
				g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
				g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
				ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
				g_pSwapChain->Present(0, 0);
			}
		}
		return 0;
	case WM_SIZE:
		if (wParam != SIZE_MINIMIZED)
		{
			g_ResizeWidth = (UINT)LOWORD(lParam);
			g_ResizeHeight = (UINT)HIWORD(lParam);

			if (g_pd3dDevice != nullptr && g_ResizeWidth != 0 && g_ResizeHeight != 0)
			{
				CleanupRenderTarget();
				g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
				g_ResizeWidth = g_ResizeHeight = 0;
				CreateRenderTarget();
			}
		}
		return 0;
	case WM_SYSCOMMAND:
		if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
			return 0;
		break;
	case WM_DESTROY:
		::PostQuitMessage(0);
		return 0;
	}
	return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

void NowRenderingModal()
{

	ImGui::OpenPopup("Rendering..");
	ImVec2 center = ImGui::GetMainViewport()->GetCenter();
	ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

	if (ImGui::BeginPopupModal("Rendering..", NULL, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::Text("Rendering....");

		ImGui::SetItemDefaultFocus();

		ImGui::EndPopup();
	}
}

void HelpPopup()
{
	ImGui::OpenPopup("Help");
	ImVec2 center = ImGui::GetMainViewport()->GetCenter();
	ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

	if (ImGui::BeginPopupModal("Help", NULL, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::Text("ZOOM : Mouse Wheel");
		ImGui::Text("PAN : Mouse Left Click");
		ImGui::Text("Explore Folder : Double Click");
		ImGui::Text("Reft Click for Context Menu");
		ImGui::Separator();

		if (ImGui::Button("OK", ImVec2(120, 0)))
		{
			ImGui::CloseCurrentPopup();
			g_ShowHelp = false;
		}
		ImGui::SetItemDefaultFocus();

		ImGui::EndPopup();
	}
}

// Process completed thumbnail results on main thread (uploads to GPU)
// Returns number of textures uploaded this call
int ProcessThumbnailResults(int maxToProcess)
{
	int processed = 0;

	while (processed < maxToProcess)
	{
		ThumbnailResult result;
		bool hasResult = false;

		EnterCriticalSection(&g_ResultCS);
		if (!g_ResultQueue.empty())
		{
			result = std::move(g_ResultQueue.back());
			g_ResultQueue.pop_back();
			hasResult = true;
		}
		LeaveCriticalSection(&g_ResultCS);

		if (!hasResult)
			break;

		// Upload to GPU
		if (result.width > 0 && result.height > 0 && !result.rgbaData.empty())
		{
			ImTextureID texId = UploadTexture_D3D11(g_pd3dDevice, result.rgbaData.data(), result.width, result.height);
			g_Textures[result.index] = texId;
		}

		processed++;
	}

	return processed;
}

bool IsSvgFile(const std::filesystem::path& p)
{
	auto ext = p.extension().wstring();

	std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);

	return ext == L".svg";
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow)
{
	(void)hInstance;
	(void)hPrevInstance;
	(void)pCmdLine;
	(void)nCmdShow;

	int argc;
	LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (argv == NULL || argc < 2)
	{
		while (true)
		{
			std::wstring svgPath = OpenSvgFileDialog(NULL);
			std::filesystem::path SelectedFile(svgPath);

			if (!SelectedFile.empty() && std::filesystem::exists(SelectedFile) && SelectedFile.has_extension() &&
				IsSvgFile(SelectedFile))
			{
				pMainFile = new std::filesystem::path(svgPath);
				break;
			}
			else
			{
				MessageBoxW(NULL, L"Invalid SVG file", L"Invalid File", MB_OK | MB_ICONERROR);
			}
		}
	}
	else
	{
		pMainFile = new std::filesystem::path(argv[1]);
	}

	if (argv)
	{
		LocalFree(argv);
		argv = NULL;
	}

	// Make process DPI aware and obtain main monitor scale
	ImGui_ImplWin32_EnableDpiAwareness();
	float main_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
	HICON hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1));
	// Create application window
	WNDCLASSEXW wc = {
		sizeof(wc), CS_CLASSDC | CS_DBLCLKS,	WndProc, 0L, 0L, GetModuleHandle(nullptr), hIcon, nullptr, nullptr,
		nullptr,	L"SVGBlast Wiaam Suleiman", nullptr};
	::RegisterClassExW(&wc);
	HWND hwnd =
		::CreateWindowW(wc.lpszClassName, L"SVGBlast. Press H for Help", WS_OVERLAPPEDWINDOW, 100, 100,
						(int)(1280 * main_scale), (int)(800 * main_scale), nullptr, nullptr, wc.hInstance, nullptr);

	// Initialize Direct3D
	if (!CreateDeviceD3D(hwnd))
	{
		CleanupDeviceD3D();
		::DestroyWindow(hwnd);
		::UnregisterClassW(wc.lpszClassName, wc.hInstance);
		return 1;
	}

	// Show the window
	::ShowWindow(hwnd, SW_SHOWDEFAULT);
	::UpdateWindow(hwnd);

	// Setup Dear ImGui context
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	(void)io;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // Enable Keyboard Controls
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;  // Enable Gamepad Controls

	// Setup Dear ImGui style
	ImGui::StyleColorsDark();

	// Setup scaling
	ImGuiStyle& style = ImGui::GetStyle();
	style.ScaleAllSizes(
		main_scale); // Bake a fixed style scale. (until we have a solution for dynamic style scaling, changing this requires resetting Style + calling this again)

	style.FontScaleDpi =
		main_scale; // Set initial font scale. (using io.ConfigDpiScaleFonts=true makes this unnecessary. We leave both here for documentation purpose)

	style.FontSizeBase = 32;

	// Font file validation and loading
	std::filesystem::path fontPath = L"C:\\Windows\\Fonts\\arial.ttf";
	while (!std::filesystem::exists(fontPath))
	{
		int result = MessageBoxW(hwnd,
								 L"The font file could not be found:\n\nC:\\Windows\\Fonts\\arial.ttf\n\nWould you "
								 L"like to locate the font file manually?",
								 L"Font Not Found", MB_YESNO | MB_ICONWARNING);

		if (result == IDNO)
		{
			// User chose to exit
			ImGui::DestroyContext();
			CleanupDeviceD3D();
			DestroyWindow(hwnd);
			UnregisterClassW(wc.lpszClassName, wc.hInstance);
			return 1;
		}

		// User chose to locate file - show file open dialog
		IFileOpenDialog* pFileOpen = nullptr;
		HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL, IID_IFileOpenDialog, (void**)&pFileOpen);
		if (SUCCEEDED(hr))
		{
			COMDLG_FILTERSPEC fileTypes[] = {
				{L"TrueType Fonts", L"*.ttf"}, {L"OpenType Fonts", L"*.otf"}, {L"All Files", L"*.*"}};
			pFileOpen->SetFileTypes(ARRAYSIZE(fileTypes), fileTypes);
			pFileOpen->SetTitle(L"Select Font File");

			hr = pFileOpen->Show(hwnd);
			if (SUCCEEDED(hr))
			{
				IShellItem* pItem = nullptr;
				hr = pFileOpen->GetResult(&pItem);
				if (SUCCEEDED(hr))
				{
					PWSTR pszFilePath = nullptr;
					hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);
					if (SUCCEEDED(hr))
					{
						fontPath = pszFilePath;
						CoTaskMemFree(pszFilePath);
					}
					pItem->Release();
				}
			}
			pFileOpen->Release();
		}
	}
	ImFont* font = io.Fonts->AddFontFromFileTTF((const char*)fontPath.u8string().c_str(), 24.0f, nullptr,
												io.Fonts->GetGlyphRangesDefault());

	if (font == nullptr)
	{
		MessageBoxW(hwnd, L"Failed to load font file. The application will now exit.", L"Font Load Error",
					MB_OK | MB_ICONERROR);

		ImGui::DestroyContext();
		CleanupDeviceD3D();
		DestroyWindow(hwnd);
		UnregisterClassW(wc.lpszClassName, wc.hInstance);
		return 1;
	}

	// Setup Platform/Renderer backends
	ImGui_ImplWin32_Init(hwnd);
	ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

	g_RESVG_Options = resvg_options_create();
	resvg_options_load_system_fonts(g_RESVG_Options);

	ViewState view;
	ImTextureID currentTexture = 0;
	uint32_t renderWidth = 0;
	uint32_t renderHeight = 0;

	// Construct a tree from the svg file and pass in some options
	int err = resvg_parse_tree_from_file((const char*)pMainFile->u8string().c_str(), g_RESVG_Options, &MainTree);
	if (err != RESVG_OK)
	{
		MessageBoxW(NULL, L"Unable to Open File", L"Error", MB_OK | MB_ICONERROR);
		exit(1);
	}
	else
	{
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

		g_MainSize = resvg_get_image_size(MainTree);

		renderWidth = (uint32_t)g_MainSize.width;
		renderHeight = (uint32_t)g_MainSize.height;

		view.svgWidth = renderWidth;
		view.svgHeight = renderHeight;

		ZeroMemory(MainPixels, MainPixelsBufferSize);

		// Create transform (scale to fit)
		resvg_transform transform = resvg_transform_identity();
		if (g_MainSize.width > 0)
		{
			float scale = (float)renderWidth / g_MainSize.width;
			transform.a = scale; // scale X
			transform.d = scale; // scale Y
		}

		resvg_render(MainTree, transform, renderWidth, renderHeight, (char*)MainPixels);

		currentTexture = UploadTexture_D3D11(g_pd3dDevice, MainPixels, renderWidth, renderHeight);
	}

	view.lastRenderedZoom = 1.0f;
	view.hasViewportTexture = false;
	view.texSvgViewX = 0;
	view.texSvgViewY = 0;
	view.texSvgViewW = (float)view.svgWidth;
	view.texSvgViewH = (float)view.svgHeight;

	SVG_Path_List = ListSVGFiles(pMainFile->parent_path());
	for (size_t i = 0; i < SVG_Path_List.size(); i++)
	{
		SVG_Path_List_UTF8.push_back(WideToUtf8(SVG_Path_List[i]));
	}
	assert(SVG_Path_List.size() == SVG_Path_List_UTF8.size());

	// Pre-allocate texture slots (nullptr initially)
	g_Textures.resize(SVG_Path_List.size(), (ImTextureID)0);
	InterlockedExchange(&g_ThumbnailsTotal, (LONG)SVG_Path_List.size());
	InterlockedExchange(&g_ThumbnailsLoaded, 0);

	// Start worker threads and queue all jobs
	StartThumbnailWorkers();
	QueueAllThumbnailJobs();

	// Start background render thread for zoom re-renders
	StartBackgroundRenderThread();

	// Main loop
	bool done = false;
	while (!done)
	{
		// Poll and handle messages (inputs, window resize, etc.)
		// See the WndProc() function below for our to dispatch events to the Win32 backend.
		MSG msg;
		while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
		{
			::TranslateMessage(&msg);
			::DispatchMessage(&msg);
			if (msg.message == WM_QUIT)
				done = true;
		}
		if (done)
			break;

		// Handle window being minimized or screen locked
		if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
		{
			::Sleep(10);
			continue;
		}
		g_SwapChainOccluded = false;

		// Handle window resize (we don't resize directly in the WM_SIZE handler)
		if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
		{
			CleanupRenderTarget();
			g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
			g_ResizeWidth = g_ResizeHeight = 0;
			CreateRenderTarget();
		}

		// Process completed thumbnail results (upload to GPU)
		// Process up to 10 per frame to avoid stuttering
		ProcessThumbnailResults(10);

		// Check if we need to request a background re-render of the SVG at new zoom level
		if (view.needsRerender)
		{
			ULONGLONG currentTime = GetTickCount64();
			if (currentTime - view.lastZoomTime >= 500) // 500ms delay after last zoom/pan
			{
				if (MainTree && view.svgWidth > 0 && view.svgHeight > 0)
				{
					ImGuiViewport* vpForCalc = ImGui::GetMainViewport();
					ImVec2 calcPos = vpForCalc->Pos;
					ImVec2 calcSize = vpForCalc->Size;

					if (view.FitsInSingleTexture())
					{
						// Small enough to render the whole SVG into one texture
						int newWidth = (int)(view.svgWidth * view.zoom);
						int newHeight = (int)(view.svgHeight * view.zoom);
						if (newWidth > 0 && newHeight > 0)
						{
							RequestBackgroundRender(MainTree, newWidth, newHeight, view.zoom, 0.0f, 0.0f,
													(float)view.svgWidth, (float)view.svgHeight);
							view.renderRequestedZoom = view.zoom;
						}
					}
					else
					{
						// Viewport mode: render only the visible region
						float svgVX, svgVY, svgVW, svgVH;
						view.CalcVisibleSvgRect(calcPos, calcSize, svgVX, svgVY, svgVW, svgVH);

						// Texture size = viewport SVG rect * zoom, clamped to max texture size
						int texW = (int)(svgVW * view.zoom);
						int texH = (int)(svgVH * view.zoom);
						if (texW > (int)g_MaxTextureSize)
							texW = (int)g_MaxTextureSize;
						if (texH > (int)g_MaxTextureSize)
							texH = (int)g_MaxTextureSize;

						if (texW > 0 && texH > 0)
						{
							// Recompute actual SVG rect from clamped texture size
							// (keeps pixel density consistent)
							float actualSvgW = (float)texW / view.zoom;
							float actualSvgH = (float)texH / view.zoom;
							float centerX = svgVX + svgVW * 0.5f;
							float centerY = svgVY + svgVH * 0.5f;
							float actualSvgX = centerX - actualSvgW * 0.5f;
							float actualSvgY = centerY - actualSvgH * 0.5f;

							RequestBackgroundRender(MainTree, texW, texH, view.zoom, actualSvgX, actualSvgY, actualSvgW,
													actualSvgH);
							view.renderRequestedZoom = view.zoom;
						}
					}
				}

				view.needsRerender = false;
				view.lastRenderedPan = view.pan;
			}
		}

		// Check if background render completed and upload the result
		RenderResult renderResult;
		if (TryGetRenderResult(renderResult))
		{
			if (renderResult.valid)
			{
				// Upload texture from MainPixels (protected by mutex)
				EnterCriticalSection(&g_MainPixelsCS);
				ReleaseTexture_D3D11(currentTexture);
				currentTexture = 0;
				currentTexture =
					UploadTexture_D3D11(g_pd3dDevice, MainPixels, renderResult.texWidth, renderResult.texHeight);
				LeaveCriticalSection(&g_MainPixelsCS);
				view.lastRenderedZoom = renderResult.zoom;

				// Track what SVG region this texture covers
				view.texSvgViewX = renderResult.svgViewX;
				view.texSvgViewY = renderResult.svgViewY;
				view.texSvgViewW = renderResult.svgViewW;
				view.texSvgViewH = renderResult.svgViewH;

				// Is this a viewport render or full SVG?
				bool isFullSvg =
					(renderResult.svgViewX == 0.0f && renderResult.svgViewY == 0.0f &&
					 (int)renderResult.svgViewW == view.svgWidth && (int)renderResult.svgViewH == view.svgHeight);
				view.hasViewportTexture = !isFullSvg;

				renderWidth = renderResult.texWidth;
				renderHeight = renderResult.texHeight;
			}
		}

		// Start the Dear ImGui frame
		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		if (g_ShowMainWindow)
		{
			if (ImGui::IsKeyPressed(ImGuiKey_H, false)) // H key
			{
				g_ShowHelp = !g_ShowHelp;
			}

			if (g_ShowHelp)
			{
				HelpPopup();
			}

			ImGuiViewport* vp = ImGui::GetMainViewport();

			ImGui::SetNextWindowPos(vp->Pos);
			ImGui::SetNextWindowSize(vp->Size);

			ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
									 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
									 ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoScrollbar |
									 ImGuiWindowFlags_NoScrollWithMouse;

			ImGui::Begin("FullScreenWindow", nullptr, flags);

			ImVec2 contentPos = ImGui::GetCursorScreenPos();
			ImVec2 contentSize = ImGui::GetContentRegionAvail();
			ImVec2 contentEnd;
			contentEnd.x = contentPos.x + contentSize.x;
			contentEnd.y = contentPos.y + contentSize.y;

			// Center of the content area
			ImVec2 contentCenter = ImVec2(contentPos.x + contentSize.x * 0.5f, contentPos.y + contentSize.y * 0.5f);

			// Calculate displayed image size based on zoom
			float displayWidth = view.svgWidth * view.zoom;
			float displayHeight = view.svgHeight * view.zoom;

			// Image position (centered + pan offset)
			ImVec2 imagePos = ImVec2(contentCenter.x - displayWidth * 0.5f + view.pan.x,
									 contentCenter.y - displayHeight * 0.5f + view.pan.y);

			ImVec2 imageEnd = ImVec2(imagePos.x + displayWidth, imagePos.y + displayHeight);

			// Pick two greys like Photoshop for checker background
			ImU32 c1 = IM_COL32(200, 200, 200, 255);
			ImU32 c2 = IM_COL32(150, 150, 150, 255);

			// Draw checker background filling the entire content area
			DrawCheckerBackground(contentPos, contentEnd, c1, c2, 16.0f);

			// Draw the image - position depends on whether texture is full SVG or viewport
			if (view.hasViewportTexture)
			{
				// Viewport texture: texture covers texSvgView* in SVG space
				// Map that SVG rect to screen coordinates
				float imgScreenX = contentCenter.x - (view.svgWidth * view.zoom) * 0.5f + view.pan.x;
				float imgScreenY = contentCenter.y - (view.svgHeight * view.zoom) * 0.5f + view.pan.y;

				float vpScreenX = imgScreenX + view.texSvgViewX * view.zoom;
				float vpScreenY = imgScreenY + view.texSvgViewY * view.zoom;
				float vpScreenW = view.texSvgViewW * view.zoom;
				float vpScreenH = view.texSvgViewH * view.zoom;

				ImVec2 vpPos = ImVec2(vpScreenX, vpScreenY);
				ImVec2 vpEnd = ImVec2(vpScreenX + vpScreenW, vpScreenY + vpScreenH);
				ImGui::GetWindowDrawList()->AddImage(currentTexture, vpPos, vpEnd);
			}
			else
			{
				// Full SVG texture: draw as before
				ImGui::GetWindowDrawList()->AddImage(currentTexture, imagePos, imageEnd);
			}

			// Handle input
			ImVec2 mousePos = io.MousePos;
			bool mouseInWindow = (mousePos.x >= contentPos.x && mousePos.x < contentPos.x + contentSize.x &&
								  mousePos.y >= contentPos.y && mousePos.y < contentPos.y + contentSize.y);

			if (mouseInWindow && ImGui::IsWindowHovered())
			{
				// Zoom with mouse wheel
				if (io.MouseWheel != 0.0f)
				{
					float oldZoom = view.zoom;

					// Zoom factor per scroll notch
					float zoomFactor = 1.1f;
					if (io.MouseWheel > 0)
						view.zoom *= zoomFactor;
					else
						view.zoom /= zoomFactor;

					view.zoom = view.ClampZoom(view.zoom);

					if (view.zoom != oldZoom)
					{
						// Zoom towards mouse position
						// Calculate mouse position relative to image center (before zoom)
						ImVec2 imageCenterBefore = ImVec2(contentCenter.x + view.pan.x, contentCenter.y + view.pan.y);

						ImVec2 mouseRelToCenter =
							ImVec2(mousePos.x - imageCenterBefore.x, mousePos.y - imageCenterBefore.y);

						// Scale the offset
						float zoomRatio = view.zoom / oldZoom;
						ImVec2 newMouseRelToCenter =
							ImVec2(mouseRelToCenter.x * zoomRatio, mouseRelToCenter.y * zoomRatio);

						// Adjust pan to keep mouse position stable
						view.pan.x -= (newMouseRelToCenter.x - mouseRelToCenter.x);
						view.pan.y -= (newMouseRelToCenter.y - mouseRelToCenter.y);

						// Mark that we need to re-render
						view.lastZoomTime = GetTickCount64();
						view.needsRerender = true;
					}
				}

				// Double-click to switch to thumbnail view
				if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
				{
					g_ShowMainWindow = false;
				}
				// Pan with left mouse button drag
				else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
				{
					view.isDragging = true;
					view.dragStartPos = mousePos;
					view.panAtDragStart = view.pan;
				}
			}

			// Continue dragging even if mouse leaves window
			if (view.isDragging)
			{
				if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
				{
					ImVec2 delta = ImVec2(mousePos.x - view.dragStartPos.x, mousePos.y - view.dragStartPos.y);
					view.pan.x = view.panAtDragStart.x + delta.x;
					view.pan.y = view.panAtDragStart.y + delta.y;
				}
				else
				{
					view.isDragging = false;

					// If we're in viewport mode, re-render after panning
					// (the user may have panned outside the rendered region)
					if (view.hasViewportTexture)
					{
						float panDeltaX = view.pan.x - view.lastRenderedPan.x;
						float panDeltaY = view.pan.y - view.lastRenderedPan.y;
						float panDist = sqrtf(panDeltaX * panDeltaX + panDeltaY * panDeltaY);
						if (panDist > 10.0f) // Only re-render if panned more than 10 pixels
						{
							view.lastZoomTime = GetTickCount64();
							view.needsRerender = true;
						}
					}
				}
			}

			if (ImGui::BeginPopupContextWindow("WindowContext",
											   ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
			{
				if (ImGui::MenuItem("Reset Zoom"))
				{
					view.Reset();
					view.needsRerender = true;
					view.lastZoomTime = GetTickCount64();
				}
				if (ImGui::MenuItem("File name to Clipboard"))
				{
					ImGui::SetClipboardText(pMainFile->u8string().c_str());
				}
				if (ImGui::MenuItem("Save as PNG"))
				{
					bool isRendering = g_RenderInProgress.load(std::memory_order_acquire);
					if (isRendering == false)
					{
						// Generate default filename from SVG name
						std::wstring defaultName = pMainFile->stem().wstring() + L".png";
						std::wstring savePath = SavePngFileDialog(hwnd, defaultName);

						if (!savePath.empty())
						{
							// Save full SVG at current zoom level
							int saveWidth = (int)(view.svgWidth * view.zoom);
							int saveHeight = (int)(view.svgHeight * view.zoom);

							if (saveWidth > 0 && saveHeight > 0 && MainTree)
							{
								size_t saveBufferSize = (size_t)saveWidth * saveHeight * 4;
								uint8_t* savePixels = (uint8_t*)malloc(saveBufferSize);

								if (savePixels)
								{
									memset(savePixels, 0, saveBufferSize);

									resvg_transform saveTransform = resvg_transform_identity();
									saveTransform.a = view.zoom;
									saveTransform.d = view.zoom;

									resvg_render(MainTree, saveTransform, saveWidth, saveHeight, (char*)savePixels);

									FILE* f = _wfopen(savePath.c_str(), L"wb");
									if (f)
									{
										int result = stbi_write_png_to_func(stbi_write_callback, f, saveWidth,
																			saveHeight, 4, savePixels, saveWidth * 4);
										fclose(f);

										if (result == 0)
										{
											MessageBoxW(hwnd, L"Failed to save PNG file.", L"Error",
														MB_OK | MB_ICONERROR);
										}
									}
									else
									{
										MessageBoxW(hwnd, L"Failed to create file.", L"Error", MB_OK | MB_ICONERROR);
									}

									free(savePixels);
								}
								else
								{
									MessageBoxW(hwnd, L"Not enough memory to save at this zoom level.", L"Error",
												MB_OK | MB_ICONERROR);
								}
							}
						}
					}
					else
					{
						MessageBeep(MB_OK);
					}
				}
				if (ImGui::MenuItem("Explore Directory"))
				{
					g_ShowMainWindow = false;
				}
				ImGui::EndPopup();
			}

			// Show animated effect while rendering is in progress or pending
			bool isRendering = g_RenderInProgress.load(std::memory_order_acquire);
			if (isRendering)
			{
				// Get time for animation
				static ULONGLONG startTime = GetTickCount64();
				float animTime = (GetTickCount64() - startTime) / 1000.0f;

				// Draw the effect at the center of the content area
				DrawRenderingEffect(contentCenter, animTime);
			}

			ImGui::End();
		}
		else //this window shows the grid of thumbnails
		{
			g_ShowHelp = false;

			int clickedIndex = LaydownThumbnails();
			if (clickedIndex >= 0 && clickedIndex < (int)SVG_Path_List.size())
			{
				// Invalidate any in-flight background render since we're loading a new SVG
				g_CurrentRequestId.fetch_add(1, std::memory_order_acq_rel);

				// Wait for background render to finish before destroying MainTree
				// (it may be using MainTree right now)
				while (g_RenderInProgress.load(std::memory_order_acquire))
				{
					Sleep(1);
				}

				if (MainTree)
				{
					resvg_tree_destroy(MainTree);
					MainTree = nullptr;
				}

				int err =
					resvg_parse_tree_from_file(SVG_Path_List_UTF8[clickedIndex].c_str(), g_RESVG_Options, &MainTree);

				if (err != RESVG_OK || MainTree == NULL)
				{
					MessageBoxW(NULL, SVG_Path_List[clickedIndex].c_str(), L"Unable to Open File",
								MB_OK | MB_ICONERROR);
				}
				else
				{
					*pMainFile = SVG_Path_List[clickedIndex];

					g_MainSize = resvg_get_image_size(MainTree);

					renderWidth = (uint32_t)g_MainSize.width;
					renderHeight = (uint32_t)g_MainSize.height;

					// Create transform (scale to fit)
					resvg_transform NewTransform = resvg_transform_identity();
					if (g_MainSize.width > 0)
					{
						float scale = (float)renderWidth / g_MainSize.width;
						NewTransform.a = scale; // scale X
						NewTransform.d = scale; // scale Y
					}

					// Render with mutex protection (background thread may also use MainPixels)
					EnterCriticalSection(&g_MainPixelsCS);
					ZeroMemory(MainPixels, MainPixelsBufferSize);
					resvg_render(MainTree, NewTransform, renderWidth, renderHeight, (char*)MainPixels);

					ReleaseTexture_D3D11(currentTexture);
					currentTexture = 0;

					currentTexture = UploadTexture_D3D11(g_pd3dDevice, MainPixels, renderWidth, renderHeight);
					LeaveCriticalSection(&g_MainPixelsCS);

					view.svgWidth = renderWidth;
					view.svgHeight = renderHeight;
					view.lastRenderedZoom = 1.0f;

					// Reset view state
					view.Reset();

					// Set up full-SVG viewport tracking
					view.texSvgViewX = 0;
					view.texSvgViewY = 0;
					view.texSvgViewW = (float)renderWidth;
					view.texSvgViewH = (float)renderHeight;

					// Switch to main view
					g_ShowMainWindow = true;
				}
			}
		}

		// Rendering
		ImGui::Render();
		g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
		g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

		// Present
		HRESULT hr = g_pSwapChain->Present(1, 0); // Present with vsync
		//HRESULT hr = g_pSwapChain->Present(0, 0); // Present without vsync
		g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
	}

	// --- orderly shutdown ---
	StopBackgroundRenderThread(); // Stop this first since it uses MainTree
	StopThumbnailWorkers();

	// Release main texture
	ReleaseTexture_D3D11(currentTexture);
	currentTexture = 0;

	// Release thumbnails
	for (ImTextureID& t : g_Textures)
	{
		ReleaseTexture_D3D11(t);
		t = 0;
	}
	g_Textures.clear();
	SVG_Path_List.clear();
	SVG_Path_List_UTF8.clear();

	if (MainTree)
	{
		resvg_tree_destroy(MainTree);
		MainTree = nullptr;
	}
	if (g_RESVG_Options)
	{
		resvg_options_destroy(g_RESVG_Options);
		g_RESVG_Options = nullptr;
	}

	if (MainPixels)
	{
		free(MainPixels); // Keeping the 1GB allocation strategy as requested
		MainPixels = nullptr;
	}

	delete pMainFile;
	pMainFile = nullptr;

	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();

	CleanupDeviceD3D();
	::DestroyWindow(hwnd);
	::UnregisterClassW(wc.lpszClassName, wc.hInstance);

	return 0;
}

// ============================================================================
// Animated Rendering Effect
// ============================================================================
// Draws a cool spinning/pulsing effect while background rendering is in progress.
// Uses ImGui's ImDrawList for custom rendering.

void DrawRenderingEffect(ImVec2 center, float time)
{
	ImDrawList* dl = ImGui::GetWindowDrawList();

	// ---- Outer spinning ring with dots ----
	const int numDots = 12;
	const float baseRadius = 40.0f;
	const float dotRadius = 4.0f;
	const float rotationSpeed = 2.0f;

	for (int i = 0; i < numDots; i++)
	{
		float angle = (float)i / numDots * 2.0f * IM_PI + time * rotationSpeed;
		float x = center.x + cosf(angle) * baseRadius;
		float y = center.y + sinf(angle) * baseRadius;

		// Fade dots based on position in the trail
		float trailPos = fmodf((time * rotationSpeed / (2.0f * IM_PI)) * numDots - i + numDots, (float)numDots);
		float alpha = 0.3f + 0.7f * (1.0f - trailPos / numDots);

		// Color cycles through a nice blue-purple gradient
		float hue = fmodf(time * 0.1f + (float)i / numDots, 1.0f);
		float r, g, b;
		// Simple HSV to RGB (saturation=0.7, value=1.0)
		float h = hue * 6.0f;
		float c = 0.7f;
		float x2 = c * (1.0f - fabsf(fmodf(h, 2.0f) - 1.0f));
		if (h < 1)
		{
			r = c;
			g = x2;
			b = 0;
		}
		else if (h < 2)
		{
			r = x2;
			g = c;
			b = 0;
		}
		else if (h < 3)
		{
			r = 0;
			g = c;
			b = x2;
		}
		else if (h < 4)
		{
			r = 0;
			g = x2;
			b = c;
		}
		else if (h < 5)
		{
			r = x2;
			g = 0;
			b = c;
		}
		else
		{
			r = c;
			g = 0;
			b = x2;
		}
		// Add white component for brightness
		r += 0.3f;
		g += 0.3f;
		b += 0.3f;

		ImU32 col = IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255), (int)(alpha * 255));
		dl->AddCircleFilled(ImVec2(x, y), dotRadius, col);
	}

	// ---- Inner pulsing ring ----
	float pulse = 0.5f + 0.5f * sinf(time * 4.0f);
	float innerRadius = 20.0f + pulse * 5.0f;
	float thickness = 2.0f + pulse * 1.0f;
	ImU32 ringColor = IM_COL32(100, 150, 255, (int)(180 * (0.5f + 0.5f * pulse)));
	dl->AddCircle(center, innerRadius, ringColor, 32, thickness);

	// ---- Orbiting particle ----
	float orbitAngle = time * 5.0f;
	float orbitRadius = 28.0f;
	ImVec2 particlePos = ImVec2(center.x + cosf(orbitAngle) * orbitRadius, center.y + sinf(orbitAngle) * orbitRadius);

	// Particle with glow effect
	for (int i = 3; i >= 0; i--)
	{
		float glowRadius = 3.0f + i * 2.0f;
		int glowAlpha = 255 / (i + 1);
		dl->AddCircleFilled(particlePos, glowRadius, IM_COL32(150, 200, 255, glowAlpha));
	}

	// ---- Center icon (SVG-like shape) ----
	float iconPulse = 0.9f + 0.1f * sinf(time * 3.0f);
	float iconSize = 8.0f * iconPulse;
	ImU32 iconColor = IM_COL32(200, 220, 255, 220);

	// Draw a simple "document" icon
	ImVec2 iconTL = ImVec2(center.x - iconSize * 0.7f, center.y - iconSize);
	ImVec2 iconBR = ImVec2(center.x + iconSize * 0.7f, center.y + iconSize);
	dl->AddRectFilled(iconTL, iconBR, iconColor, 2.0f);

	// Corner fold
	ImVec2 foldPoints[3] = {ImVec2(iconBR.x - iconSize * 0.4f, iconTL.y), ImVec2(iconBR.x, iconTL.y + iconSize * 0.4f),
							ImVec2(iconBR.x - iconSize * 0.4f, iconTL.y + iconSize * 0.4f)};
	dl->AddTriangleFilled(foldPoints[0], foldPoints[1], foldPoints[2], IM_COL32(150, 180, 220, 220));

	// ---- "Rendering" text below ----
	const char* text = "Rendering";
	ImVec2 textSize = ImGui::CalcTextSize(text);
	float textAlpha = 0.7f + 0.3f * sinf(time * 2.0f);
	ImVec2 textPos = ImVec2(center.x - textSize.x * 0.5f, center.y + baseRadius + 15.0f);
	dl->AddText(textPos, IM_COL32(255, 255, 255, (int)(textAlpha * 255)), text);

	// Subtle shadow for text
	dl->AddText(ImVec2(textPos.x + 1, textPos.y + 1), IM_COL32(0, 0, 0, (int)(textAlpha * 100)), text);
}

// Returns -1 if no thumbnail clicked, otherwise returns the index of clicked thumbnail
int LaydownThumbnails()
{
	int clickedIndex = -1;

	ImGuiViewport* vp = ImGui::GetMainViewport();

	ImGui::SetNextWindowPos(vp->Pos);
	ImGui::SetNextWindowSize(vp->Size);

	ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
							 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
							 ImGuiWindowFlags_NoNavFocus;

	ImGui::Begin("FullScreenWindow", nullptr, flags);

	// Restore scroll position if we have a saved one
	static bool s_NeedRestoreScroll = false;
	if (s_NeedRestoreScroll)
	{
		ImGui::SetScrollY(g_ThumbnailScrollY);
		s_NeedRestoreScroll = false;
	}

	bool IsLoadingDone = false;
	// Show loading progress
	LONG loaded = g_ThumbnailsLoaded;
	LONG total = g_ThumbnailsTotal;
	if (loaded < total)
	{
		ImGui::Text("Loading thumbnails: %d / %d", loaded, total);
		if (total > 0)
			ImGui::ProgressBar((float)loaded / (float)total, ImVec2(-1, 0));
		ImGui::Separator();
	}
	else
	{
		IsLoadingDone = true;
	}

	float ThumbnailSize = 256.0f;
	int ItemCount = (int)g_Textures.size();
	const float MinPadding = 8.0f;

	if (ItemCount == 0)
	{
		ImGui::End();
		return -1;
	}

	// Account for ImageButton frame padding (default is style.FramePadding, applied on each side)
	ImVec2 framePadding = ImGui::GetStyle().FramePadding;
	float ButtonSize = ThumbnailSize + framePadding.x * 2.0f;

	// Get available width (accounts for scrollbar if present)
	float AvailWidth = ImGui::GetContentRegionAvail().x;

	// Calculate how many columns fit with minimum padding
	int Columns = (int)((AvailWidth + MinPadding) / (ButtonSize + MinPadding));
	Columns = ImClamp(Columns, 1, ItemCount);

	// Calculate padding to distribute remaining space evenly
	float TotalButtonWidth = Columns * ButtonSize;
	float RemainingSpace = AvailWidth - TotalButtonWidth;

	float Padding = (Columns > 0) ? (RemainingSpace / (float)Columns) : MinPadding;
	Padding = ImMax(Padding, MinPadding);

	if (ItemCount == Columns)
	{
		if (Padding > 40.0f)
			Padding = 40.0f;
	}

	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Padding, MinPadding));

	// Use clipper for performance - only process visible rows
	int totalRows = (ItemCount + Columns - 1) / Columns;
	float rowHeight = ButtonSize + MinPadding;

	ImGuiListClipper clipper;
	clipper.Begin(totalRows, rowHeight);

	while (clipper.Step())
	{
		for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
		{
			for (int col = 0; col < Columns; col++)
			{
				int i = row * Columns + col;
				if (i >= ItemCount)
					break;

				ImGui::PushID(i);

				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.6f, 0.6f, 1));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.6f, 0.6f, 0.5f));

				// Check if texture is loaded
				if (g_Textures[i] != (ImTextureID)0)
				{
					if (ImGui::ImageButton("##img", g_Textures[i], ImVec2(ThumbnailSize, ThumbnailSize)))
					{
						clickedIndex = i;
					}
				}
				else
				{
					// Show placeholder button for not-yet-loaded thumbnails
					if (ImGui::Button("##placeholder",
									  ImVec2(ThumbnailSize + framePadding.x * 2, ThumbnailSize + framePadding.y * 2)))
					{
						// Can still click to load even if thumbnail not ready
						clickedIndex = i;
					}

					// Draw "Loading..." text centered on the button
					ImVec2 buttonMin = ImGui::GetItemRectMin();
					ImVec2 buttonMax = ImGui::GetItemRectMax();
					const char* loadingText = (IsLoadingDone ? "INVALID" : "Loading...");
					ImVec2 textSize = ImGui::CalcTextSize(loadingText);
					ImVec2 textPos = ImVec2(buttonMin.x + (buttonMax.x - buttonMin.x - textSize.x) * 0.5f,
											buttonMin.y + (buttonMax.y - buttonMin.y - textSize.y) * 0.5f);
					ImGui::GetWindowDrawList()->AddText(textPos, IM_COL32(100, 100, 100, 255), loadingText);
				}

				if (ImGui::BeginPopupContextItem("##img"))
				{
					if (ImGui::MenuItem("Open"))
					{
						clickedIndex = i;
					}
					if (ImGui::MenuItem("Name to Clipboard"))
					{
						ImGui::SetClipboardText(SVG_Path_List_UTF8[i].c_str());
					}
					ImGui::EndPopup();
				}

				ImGui::PopStyleColor(3);

				ImGui::SetItemTooltip("%s", SVG_Path_List_UTF8[i].c_str());
				ImGui::PopID();

				// SameLine for all but the last column in each row
				if (col < Columns - 1)
				{
					ImGui::SameLine();
				}
			}
		}
	}

	clipper.End();

	// Save scroll position before we potentially switch to main view
	if (clickedIndex >= 0)
	{
		g_ThumbnailScrollY = ImGui::GetScrollY();
		s_NeedRestoreScroll = true; // Flag to restore when we come back
	}

	ImGui::PopStyleVar();
	ImGui::End();

	return clickedIndex;
}