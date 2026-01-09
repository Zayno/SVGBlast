//SVGBlast Developed by Wiaam Suleiman


#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>
#include <float.h>
#include <math.h>
#include"resource.h"
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")

#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

// Checkerboard colors (Photoshop style)
#define CHECK_LIGHT 0xFFCCCCCC
#define CHECK_DARK  0xFF999999
#define CHECK_SIZE  16

// Zoom limits
#define ZOOM_MIN 0.1f
#define ZOOM_MAX 20.0f
#define ZOOM_STEP 1.1f

// Window constraints
#define MAX_WINDOW_WIDTH  1600
#define MAX_WINDOW_HEIGHT 900

// Global state
static struct
{
	NSVGimage* svg;
	NSVGrasterizer* rasterizer;
	unsigned char* svgPixels;
	int svgBaseWidth;      // Original SVG dimensions
	int svgBaseHeight;
	int rasterWidth;       // Current rasterized dimensions
	int rasterHeight;
	float rasterZoom;      // Zoom level at which current raster was made

	HBITMAP backbuffer;
	unsigned int* backbufferPixels;
	int backbufferWidth;
	int backbufferHeight;

	float zoom;
	float panX;
	float panY;

	BOOL isPanning;
	int lastMouseX;
	int lastMouseY;
} g;

static void ResetView(void)
{
	g.zoom = 1.0f;
	g.panX = 0.0f;
	g.panY = 0.0f;
}

static void ClampPan(int windowWidth, int windowHeight)
{
	float scaledWidth = g.svgBaseWidth * g.zoom;
	float scaledHeight = g.svgBaseHeight * g.zoom;

	float maxPanX = fmaxf(0, (scaledWidth - windowWidth) / 2 + scaledWidth / 2);
	float maxPanY = fmaxf(0, (scaledHeight - windowHeight) / 2 + scaledHeight / 2);

	g.panX = fmaxf(-maxPanX, fminf(maxPanX, g.panX));
	g.panY = fmaxf(-maxPanY, fminf(maxPanY, g.panY));
}

static void RasterizeAtZoom(float zoom)
{
	int newWidth = (int)(g.svgBaseWidth * zoom);
	int newHeight = (int)(g.svgBaseHeight * zoom);

	// Clamp to reasonable limits to avoid huge allocations
	if (newWidth < 1) newWidth = 1;
	if (newHeight < 1) newHeight = 1;
	if (newWidth > 16384) newWidth = 16384;
	if (newHeight > 16384) newHeight = 16384;

	unsigned char* newPixels = (unsigned char*)realloc(g.svgPixels, newWidth * newHeight * 4);
	if (newPixels == NULL)
		return;

	g.svgPixels = newPixels;
	g.rasterWidth = newWidth;
	g.rasterHeight = newHeight;
	g.rasterZoom = zoom;

	nsvgRasterize(g.rasterizer, g.svg, 0, 0, zoom, g.svgPixels, newWidth, newHeight, newWidth * 4);
}

static unsigned int BlendPixel(unsigned int bg, unsigned int fg)
{
	unsigned int fgA = (fg >> 24) & 0xFF;

	if (fgA == 255) return fg;
	if (fgA == 0) return bg;

	unsigned int fgR = (fg >> 0) & 0xFF;
	unsigned int fgG = (fg >> 8) & 0xFF;
	unsigned int fgB = (fg >> 16) & 0xFF;

	unsigned int bgR = (bg >> 0) & 0xFF;
	unsigned int bgG = (bg >> 8) & 0xFF;
	unsigned int bgB = (bg >> 16) & 0xFF;

	unsigned int invA = 255 - fgA;
	unsigned int outR = (fgR * fgA + bgR * invA) / 255;
	unsigned int outG = (fgG * fgA + bgG * invA) / 255;
	unsigned int outB = (fgB * fgA + bgB * invA) / 255;

	return 0xFF000000 | (outB << 16) | (outG << 8) | outR;
}

static unsigned int GetCheckerColor(int x, int y)
{
	int cx = x / CHECK_SIZE;
	int cy = y / CHECK_SIZE;
	return ((cx + cy) & 1) ? CHECK_DARK : CHECK_LIGHT;
}

static unsigned int SampleSVG(int x, int y)
{
	if (x < 0 || y < 0 || x >= g.rasterWidth || y >= g.rasterHeight)
		return 0;

	int idx = (y * g.rasterWidth + x) * 4;
	unsigned char r = g.svgPixels[idx + 0];
	unsigned char ga = g.svgPixels[idx + 1];
	unsigned char b = g.svgPixels[idx + 2];
	unsigned char a = g.svgPixels[idx + 3];

	return (a << 24) | (r << 16) | (ga << 8) | b;
}

static void RenderToBackbuffer(int windowWidth, int windowHeight)
{
	if (!g.backbufferPixels || windowWidth != g.backbufferWidth || windowHeight != g.backbufferHeight)
		return;

	// Calculate where the scaled raster should appear
	float displayWidth = g.svgBaseWidth * g.zoom;
	float displayHeight = g.svgBaseHeight * g.zoom;
	float offsetX = (windowWidth - displayWidth) / 2.0f - g.panX;
	float offsetY = (windowHeight - displayHeight) / 2.0f - g.panY;

	// Scale factor from display coordinates to raster coordinates
	float scaleX = (float)g.rasterWidth / displayWidth;
	float scaleY = (float)g.rasterHeight / displayHeight;

	for (int y = 0; y < windowHeight; y++)
	{
		for (int x = 0; x < windowWidth; x++)
		{
			unsigned int checker = GetCheckerColor(x, y);

			// Map screen position to raster position
			int rasterX = (int)((x - offsetX) * scaleX);
			int rasterY = (int)((y - offsetY) * scaleY);

			unsigned int svgColor = SampleSVG(rasterX, rasterY);
			unsigned int finalColor = BlendPixel(checker, svgColor);

			// Flip Y for DIB (bottom-up)
			g.backbufferPixels[(windowHeight - 1 - y) * windowWidth + x] = finalColor;
		}
	}
}

static void EnsureBackbuffer(HWND hwnd, int width, int height)
{
	if (g.backbuffer && g.backbufferWidth == width && g.backbufferHeight == height)
		return;

	if (g.backbuffer)
	{
		DeleteObject(g.backbuffer);
		g.backbuffer = NULL;
		g.backbufferPixels = NULL;
	}

	BITMAPINFO bmi = { 0 };
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = width;
	bmi.bmiHeader.biHeight = height;  // Positive = bottom-up DIB
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;

	HDC hdc = GetDC(hwnd);
	g.backbuffer = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, (void**)&g.backbufferPixels, NULL, 0);
	ReleaseDC(hwnd, hdc);

	g.backbufferWidth = width;
	g.backbufferHeight = height;
}

static void Paint(HWND hwnd)
{
	PAINTSTRUCT ps;
	HDC hdc = BeginPaint(hwnd, &ps);

	RECT rc;
	GetClientRect(hwnd, &rc);
	int width = rc.right - rc.left;
	int height = rc.bottom - rc.top;

	if (width > 0 && height > 0)
	{
		EnsureBackbuffer(hwnd, width, height);
		RenderToBackbuffer(width, height);

		HDC memDC = CreateCompatibleDC(hdc);
		HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, g.backbuffer);

		BitBlt(hdc, 0, 0, width, height, memDC, 0, 0, SRCCOPY);

		SelectObject(memDC, oldBmp);
		DeleteDC(memDC);
	}

	EndPaint(hwnd, &ps);
}

static void OnMouseWheel(HWND hwnd, int x, int y, int delta)
{
	RECT rc;
	GetClientRect(hwnd, &rc);
	int windowWidth = rc.right - rc.left;
	int windowHeight = rc.bottom - rc.top;

	POINT pt = { x, y };
	ScreenToClient(hwnd, &pt);

	float scaledWidth = g.svgBaseWidth * g.zoom;
	float scaledHeight = g.svgBaseHeight * g.zoom;
	float offsetX = (windowWidth - scaledWidth) / 2.0f - g.panX;
	float offsetY = (windowHeight - scaledHeight) / 2.0f - g.panY;

	float svgX = (pt.x - offsetX) / g.zoom;
	float svgY = (pt.y - offsetY) / g.zoom;

	float oldZoom = g.zoom;
	if (delta > 0)
		g.zoom *= ZOOM_STEP;
	else
		g.zoom /= ZOOM_STEP;

	g.zoom = fmaxf(ZOOM_MIN, fminf(ZOOM_MAX, g.zoom));

	float newScaledWidth = g.svgBaseWidth * g.zoom;
	float newScaledHeight = g.svgBaseHeight * g.zoom;
	float newOffsetX = (windowWidth - newScaledWidth) / 2.0f - g.panX;
	float newOffsetY = (windowHeight - newScaledHeight) / 2.0f - g.panY;

	float newScreenX = svgX * g.zoom + newOffsetX;
	float newScreenY = svgY * g.zoom + newOffsetY;

	g.panX += newScreenX - pt.x;
	g.panY += newScreenY - pt.y;

	ClampPan(windowWidth, windowHeight);
	InvalidateRect(hwnd, NULL, FALSE);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
		case WM_PAINT:
			Paint(hwnd);
			return 0;

		case WM_SIZE:
			InvalidateRect(hwnd, NULL, FALSE);
			return 0;

		case WM_MOUSEWHEEL:
			OnMouseWheel(hwnd,
				GET_X_LPARAM(lParam),
				GET_Y_LPARAM(lParam),
				GET_WHEEL_DELTA_WPARAM(wParam));
			return 0;

		case WM_LBUTTONDOWN:
			g.isPanning = TRUE;
			g.lastMouseX = GET_X_LPARAM(lParam);
			g.lastMouseY = GET_Y_LPARAM(lParam);
			SetCapture(hwnd);
			return 0;

		case WM_LBUTTONUP:
			g.isPanning = FALSE;
			ReleaseCapture();
			return 0;

		case WM_MOUSEMOVE:
			if (g.isPanning)
			{
				int x = GET_X_LPARAM(lParam);
				int y = GET_Y_LPARAM(lParam);
				int dx = x - g.lastMouseX;
				int dy = y - g.lastMouseY;

				g.panX -= dx;
				g.panY -= dy;

				RECT rc;
				GetClientRect(hwnd, &rc);
				ClampPan(rc.right - rc.left, rc.bottom - rc.top);

				g.lastMouseX = x;
				g.lastMouseY = y;

				InvalidateRect(hwnd, NULL, FALSE);
			}
			return 0;

		case WM_RBUTTONDOWN:
			ResetView();
			RasterizeAtZoom(g.zoom);
			InvalidateRect(hwnd, NULL, FALSE);
			return 0;

		case WM_KEYDOWN:
			if (wParam == VK_SPACE)
			{
				RasterizeAtZoom(g.zoom);
				InvalidateRect(hwnd, NULL, FALSE);
			}
			return 0;

		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;
	}

	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow)
{
	(void)hInstance;
	(void)hPrevInstance;
	(void)pCmdLine;
	(void)nCmdShow;

	// Parse command line arguments
	int argc;
	LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

	if (argv == NULL || argc < 2)
	{
		MessageBoxW(NULL, L"Usage: SVGBlast.exe <filename.svg>", L"Error", MB_OK | MB_ICONERROR);
		if (argv) LocalFree(argv);
		return 1;
	}

	g.svg = nsvgParseFromFileW(argv[1], "px", 96.0f);
	if (g.svg == NULL)
	{
		MessageBoxW(NULL, L"Could not open SVG file", L"Error", MB_OK | MB_ICONERROR);
		LocalFree(argv);
		return 1;
	}

	g.svgBaseWidth = (int)g.svg->width;
	g.svgBaseHeight = (int)g.svg->height;

	g.rasterizer = nsvgCreateRasterizer();
	if (g.rasterizer == NULL)
	{
		nsvgDelete(g.svg);
		LocalFree(argv);
		return 1;
	}

	ResetView();

	// Initial rasterization at zoom 1.0
	g.svgPixels = NULL;
	g.rasterZoom = 0.0f;  // Force initial rasterization
	RasterizeAtZoom(g.zoom);

	if (g.svgPixels == NULL)
	{
		nsvgDeleteRasterizer(g.rasterizer);
		nsvgDelete(g.svg);
		LocalFree(argv);
		return 1;
	}

	// Calculate initial window size
	int windowWidth = g.svgBaseWidth;
	int windowHeight = g.svgBaseHeight;

	if (windowWidth > MAX_WINDOW_WIDTH)
	{
		float scale = (float)MAX_WINDOW_WIDTH / windowWidth;
		windowWidth = MAX_WINDOW_WIDTH;
		windowHeight = (int)(windowHeight * scale);
	}
	if (windowHeight > MAX_WINDOW_HEIGHT)
	{
		float scale = (float)MAX_WINDOW_HEIGHT / windowHeight;
		windowHeight = MAX_WINDOW_HEIGHT;
		windowWidth = (int)(windowWidth * scale);
	}

	// Ensure minimum size
	if (windowWidth < 400) windowWidth = 400;
	if (windowHeight < 300) windowHeight = 300;

	WNDCLASSW wc = { 0 };
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = WndProc;
	wc.hInstance = GetModuleHandle(NULL);
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.lpszClassName = L"SVGBlast";
	wc.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_ICON1));
	RegisterClassW(&wc);

	RECT wr = { 0, 0, windowWidth, windowHeight };
	AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);

	HWND hwnd = CreateWindowW(
		L"SVGBlast",
		L"SVG Blast - Wheel=Zoom, Left drag=Pan, Right click=Reset, Space=Sharpen",
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT,
		wr.right - wr.left, wr.bottom - wr.top,
		NULL, NULL,
		GetModuleHandle(NULL),
		NULL);

	if (!hwnd)
	{
		free(g.svgPixels);
		nsvgDeleteRasterizer(g.rasterizer);
		nsvgDelete(g.svg);
		LocalFree(argv);
		return 1;
	}

	ShowWindow(hwnd, SW_SHOW);
	UpdateWindow(hwnd);

	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	// Cleanup
	if (g.backbuffer)
		DeleteObject(g.backbuffer);
	free(g.svgPixels);
	nsvgDeleteRasterizer(g.rasterizer);
	nsvgDelete(g.svg);
	LocalFree(argv);

	return 0;
}
