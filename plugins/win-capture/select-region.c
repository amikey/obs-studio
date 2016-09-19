#include <windows.h>
#include <windowsx.h>
#include <obs-module.h>
#include <util/platform.h>
#include "select-region.h"

#define CLASS_TEXT L"text_window"
#define CLASS_INVIS L"invis_window"
#define CLASS_SELECT L"select_window"

struct select_region_info {
	HANDLE thread;
	DWORD thread_id;
	bool success;

	HWND text_hwnd;
	HWND invis_hwnd;
	HWND select_hwnd;

	RECT rect;
	select_region_cb cb;
	void *param;

	POINT pos;
	SIZE size;

	POINT final_pos;
	SIZE final_size;
};

static struct select_region_info info = {0};
static HGDIOBJ hfont = NULL;
static wchar_t *text = NULL;
static size_t text_len = 0;
static SIZE extents = {0};
static bool ldown = false;

static LRESULT WINAPI select_proc(HWND hwnd, UINT message,
		WPARAM wparam, LPARAM lparam)
{
	switch (message) {
	case WM_PAINT:
		{
			RECT r;
			GetClientRect(hwnd, &r);

			PAINTSTRUCT ps;
			HDC hdc = BeginPaint(hwnd, &ps);
			if (!hdc)
				break;

			FillRect(hdc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
			EndPaint(hwnd, &ps);
			break;
		}
	case WM_CLOSE:
		PostQuitMessage(0);
		break;
	}

	return DefWindowProc(hwnd, message, wparam, lparam);
}

static LRESULT WINAPI text_proc(HWND hwnd, UINT message,
		WPARAM wparam, LPARAM lparam)
{
	switch (message) {
	case WM_PAINT:
		{
			RECT r;
			GetClientRect(hwnd, &r);

			PAINTSTRUCT ps;
			HDC hdc = BeginPaint(hwnd, &ps);
			if (!hdc)
				break;

			HGDIOBJ hfont_old = SelectObject(hdc, hfont);

			HBRUSH hbrush = (HBRUSH)GetStockObject(BLACK_BRUSH);
			FillRect(hdc, &r, hbrush);

			HGDIOBJ hpen_old = SelectObject(hdc,
					GetStockObject(WHITE_PEN));

			MoveToEx(hdc, 0, 0, NULL);
			LineTo(hdc, r.right - 1, 0);
			LineTo(hdc, r.right - 1, r.bottom - 1);
			LineTo(hdc, 0, r.bottom - 1);
			LineTo(hdc, 0, 0);

			SetBkMode(hdc, TRANSPARENT);
			SetTextAlign(hdc, TA_CENTER);
			SetTextColor(hdc, 0xFFFFFF);

			TextOutW(hdc, r.right / 2, (r.bottom - extents.cy) / 2,
					text, (int)text_len);

			SelectObject(hdc, hpen_old);
			SelectObject(hdc, hfont_old);
			EndPaint(hwnd, &ps);
			break;
		}
	case WM_CLOSE:
		PostQuitMessage(0);
		break;
	}

	return DefWindowProc(hwnd, message, wparam, lparam);
}

static LRESULT WINAPI invis_proc(HWND hwnd, UINT message,
		WPARAM wparam, LPARAM lparam)
{
	switch (message) {
	case WM_KEYDOWN:
		{
			if (wparam == VK_ESCAPE)
				PostQuitMessage(0);
			break;
		}
	case WM_LBUTTONDOWN:
		{
			info.pos.x = GET_X_LPARAM(lparam);
			info.pos.y = GET_Y_LPARAM(lparam);
			info.size.cx = 0;
			info.size.cy = 0;
			ldown = true;

			SetWindowPos(info.select_hwnd, info.text_hwnd,
					0, 0, 0, 0,
					SWP_NOMOVE | SWP_NOSIZE |
					SWP_SHOWWINDOW);
			SetCapture(hwnd);
			break;
		}
	case WM_LBUTTONUP:
		{
			info.success = true;
			PostQuitMessage(0);
			break;
		}
	case WM_MOUSEMOVE:
		{
			if (!ldown)
				break;

			RECT r;
			GetClientRect(hwnd, &r);

			info.size.cx = GET_X_LPARAM(lparam) - info.pos.x;
			info.size.cy = GET_Y_LPARAM(lparam) - info.pos.y;

			POINT pos = info.pos;
			SIZE size = info.size;

			if (size.cx < 0) {
				pos.x = pos.x + size.cx;
				size.cx = -size.cx;
			}
			if (size.cy < 0) {
				pos.y = pos.y + size.cy;
				size.cy = -size.cy;
			}

			if (pos.x < 0) {
				size.cx += pos.x;
				pos.x = 0;
			}
			if (pos.y < 0) {
				size.cy += pos.y;
				pos.y = 0;
			}

			LONG window_cx = (r.right - r.left);
			LONG window_cy = (r.bottom - r.top);

			if ((pos.x + size.cx) > window_cx)
				size.cx = window_cx - pos.x;
			if ((pos.y + size.cy) > window_cy)
				size.cy = window_cy - pos.y;

			info.final_pos = pos;
			info.final_size = size;

			ClientToScreen(hwnd, &pos);
			SetWindowPos(info.select_hwnd, info.text_hwnd,
					pos.x, pos.y, size.cx, size.cy, 0);
			break;
		}
	case WM_CLOSE:
		PostQuitMessage(0);
		break;

	case WM_DESTROY:
		if (ldown)
			ReleaseCapture();
		break;		
	}

	return DefWindowProc(hwnd, message, wparam, lparam);
}

#define BOX_SIZE 10

static DWORD CALLBACK select_thread(void *param)
{
	HINSTANCE hinst = GetModuleHandle(NULL);
	RECT r = info.rect;
	HGDIOBJ hfont_old;
	LONG cx, cy;
	LOGFONT lf;
	LONG x, y;
	MSG msg;
	HDC hdc;
	SIZE e;

	/* -------------------------- */
	/* create text font */

	hfont = GetStockObject(DEFAULT_GUI_FONT);
	GetObject(hfont, sizeof(lf), (void*)&lf);
	lf.lfHeight *= 2;

	hfont = (HFONT)CreateFontIndirectW(&lf);

	const char *atext = obs_module_text("Subregion.Select.DialogText");
	os_utf8_to_wcs_ptr(atext, 0, &text);
	text_len = wcslen(text);

	hdc = CreateCompatibleDC(NULL);
	hfont_old = SelectObject(hdc, hfont);
	GetTextExtentPoint32W(hdc, text, (int)text_len, &extents);
	SelectObject(hdc, hfont_old);
	DeleteDC(hdc);

	/* -------------------------- */
	/* calc text pos */

	e = extents;
	e.cx += BOX_SIZE * 2;
	e.cy += BOX_SIZE * 2;

	cx = r.right - r.left;
	cy = r.bottom - r.top;

	x = r.left + (cx / 2 - e.cx / 2);
	y = r.top  + (cy / 6 - e.cy / 2);

	/* -------------------------- */
	/* create windows */

	info.text_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED,
			CLASS_TEXT, NULL, WS_POPUP | WS_VISIBLE,
			x, y, e.cx, e.cy, NULL, NULL, hinst, NULL);
	SetLayeredWindowAttributes(info.text_hwnd, 0, 0xC0, LWA_ALPHA);

	info.select_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED,
			CLASS_SELECT, NULL, WS_POPUP,
			0, 0, 1, 1, NULL, NULL, hinst, NULL);
	SetLayeredWindowAttributes(info.select_hwnd, 0, 0xC0, LWA_ALPHA);

	info.invis_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED,
			CLASS_INVIS, NULL, WS_POPUP | WS_VISIBLE,
			r.left, r.top, r.right - r.left, r.bottom - r.top,
			NULL, NULL, hinst, NULL);
	SetLayeredWindowAttributes(info.invis_hwnd, 0, 1, LWA_ALPHA);

	while (GetMessage(&msg, NULL, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	/* -------------------------- */
	/* free data */

	DestroyWindow(info.text_hwnd);
	DestroyWindow(info.invis_hwnd);
	DestroyWindow(info.select_hwnd);
	info.text_hwnd = NULL;
	info.invis_hwnd = NULL;
	info.select_hwnd = NULL;

	DeleteObject(hfont);
	hfont = NULL;

	bfree(text);
	text = NULL;

	ldown = false;

	if (info.cb && info.success)
		info.cb(info.param, info.final_pos, info.final_size);

	UNUSED_PARAMETER(param);
	return 0;
}

static void init_select_region(void)
{
	HINSTANCE hinst = GetModuleHandle(NULL);
	WNDCLASS wc = {0};

	wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = invis_proc;
	wc.hInstance = hinst;
	wc.hCursor = LoadCursor(NULL, IDC_CROSS);
	wc.lpszClassName = CLASS_INVIS;
	RegisterClass(&wc);

	wc.lpfnWndProc = text_proc;
	wc.lpszClassName = CLASS_TEXT;
	RegisterClass(&wc);

	wc.lpfnWndProc = select_proc;
	wc.lpszClassName = CLASS_SELECT;
	RegisterClass(&wc);
}

bool select_region_begin(select_region_cb cb, RECT r, void *data)
{
	static bool init = false;
	if (!init) {
		init_select_region();
		init = true;
	}

	select_region_free();

	info.cb = cb;
	info.rect = r;
	info.param = data;
	info.thread = CreateThread(NULL, 0, select_thread, NULL, 0, NULL);
	return !!info.thread;
}

void select_region_free(void)
{
	if (info.thread) {
		PostThreadMessage(info.thread_id, WM_QUIT, 0, 0);
		WaitForSingleObject(info.thread, INFINITE);
		CloseHandle(info.thread);
		info.thread = NULL;
	}

	info.success = false;
	info.cb = NULL;
}
