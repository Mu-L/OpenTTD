/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file win32_v.cpp Implementation of the Windows (GDI) video driver. */

#include "../stdafx.h"
#include "../openttd.h"
#include "../error_func.h"
#include "../gfx_func.h"
#include "../os/windows/win32.h"
#include "../blitter/factory.hpp"
#include "../core/geometry_func.hpp"
#include "../core/math_func.hpp"
#include "../core/random_func.hpp"
#include "../texteff.hpp"
#include "../thread.h"
#include "../progress.h"
#include "../window_gui.h"
#include "../window_func.h"
#include "../framerate_type.h"
#include "../library_loader.h"
#include "../core/utf8.hpp"
#include "win32_v.h"
#include <windows.h>
#include <imm.h>
#include <versionhelpers.h>
#if defined(_MSC_VER) && defined(NTDDI_WIN10_RS4)
#include <winrt/Windows.UI.ViewManagement.h>
#endif

#include "../safeguards.h"

/* Missing define in MinGW headers. */
#ifndef MAPVK_VK_TO_CHAR
#define MAPVK_VK_TO_CHAR    (2)
#endif

#ifndef PM_QS_INPUT
#define PM_QS_INPUT 0x20000
#endif

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

bool _window_maximize;
static Dimension _bck_resolution;
DWORD _imm_props;

static Palette _local_palette; ///< Current palette to use for drawing.

bool VideoDriver_Win32Base::ClaimMousePointer()
{
	MyShowCursor(false, true);
	return true;
}

struct Win32VkMapping {
	uint8_t vk_from;
	uint8_t vk_count;
	uint8_t map_to;
};

#define AS(x, z) {x, 1, z}
#define AM(x, y, z, w) {x, y - x + 1, z}

static const Win32VkMapping _vk_mapping[] = {
	/* Pageup stuff + up/down */
	AM(VK_PRIOR, VK_DOWN, WKC_PAGEUP, WKC_DOWN),
	/* Map letters & digits */
	AM('A', 'Z', 'A', 'Z'),
	AM('0', '9', '0', '9'),

	AS(VK_ESCAPE,   WKC_ESC),
	AS(VK_PAUSE,    WKC_PAUSE),
	AS(VK_BACK,     WKC_BACKSPACE),
	AM(VK_INSERT,   VK_DELETE, WKC_INSERT, WKC_DELETE),

	AS(VK_SPACE,    WKC_SPACE),
	AS(VK_RETURN,   WKC_RETURN),
	AS(VK_TAB,      WKC_TAB),

	/* Function keys */
	AM(VK_F1, VK_F12, WKC_F1, WKC_F12),

	/* Numeric part */
	AM(VK_NUMPAD0, VK_NUMPAD9, '0', '9'),
	AS(VK_DIVIDE,   WKC_NUM_DIV),
	AS(VK_MULTIPLY, WKC_NUM_MUL),
	AS(VK_SUBTRACT, WKC_NUM_MINUS),
	AS(VK_ADD,      WKC_NUM_PLUS),
	AS(VK_DECIMAL,  WKC_NUM_DECIMAL),

	/* Other non-letter keys */
	AS(0xBF,  WKC_SLASH),
	AS(0xBA,  WKC_SEMICOLON),
	AS(0xBB,  WKC_EQUALS),
	AS(0xDB,  WKC_L_BRACKET),
	AS(0xDC,  WKC_BACKSLASH),
	AS(0xDD,  WKC_R_BRACKET),

	AS(0xDE,  WKC_SINGLEQUOTE),
	AS(0xBC,  WKC_COMMA),
	AS(0xBD,  WKC_MINUS),
	AS(0xBE,  WKC_PERIOD)
};

static uint MapWindowsKey(uint sym)
{
	uint key = 0;

	for (const auto &map : _vk_mapping) {
		if (IsInsideBS(sym, map.vk_from, map.vk_count)) {
			key = sym - map.vk_from + map.map_to;
			break;
		}
	}

	if (GetAsyncKeyState(VK_SHIFT)   < 0) key |= WKC_SHIFT;
	if (GetAsyncKeyState(VK_CONTROL) < 0) key |= WKC_CTRL;
	if (GetAsyncKeyState(VK_MENU)    < 0) key |= WKC_ALT;
	return key;
}

/** Colour depth to use for fullscreen display modes. */
uint8_t VideoDriver_Win32Base::GetFullscreenBpp()
{
	/* Check modes for the relevant fullscreen bpp */
	return _support8bpp != S8BPP_HARDWARE ? 32 : BlitterFactory::GetCurrentBlitter()->GetScreenDepth();
}

/**
 * Instantiate a new window.
 * @param full_screen Whether to make a full screen window or not.
 * @param resize Whether to change window size.
 * @return True if the window could be created.
 */
bool VideoDriver_Win32Base::MakeWindow(bool full_screen, bool resize)
{
	/* full_screen is whether the new window should be fullscreen,
	 * _wnd.fullscreen is whether the current window is. */
	_fullscreen = full_screen;

	/* recreate window? */
	if ((full_screen != this->fullscreen) && this->main_wnd) {
		DestroyWindow(this->main_wnd);
		this->main_wnd = 0;
	}

	if (full_screen) {
		DEVMODE settings{};
		settings.dmSize = sizeof(settings);
		settings.dmFields =
			DM_BITSPERPEL |
			DM_PELSWIDTH |
			DM_PELSHEIGHT;
		settings.dmBitsPerPel = this->GetFullscreenBpp();
		settings.dmPelsWidth  = this->width_org;
		settings.dmPelsHeight = this->height_org;

		/* Check for 8 bpp support. */
		if (settings.dmBitsPerPel == 8 && ChangeDisplaySettings(&settings, CDS_FULLSCREEN | CDS_TEST) != DISP_CHANGE_SUCCESSFUL) {
			settings.dmBitsPerPel = 32;
		}

		/* Test fullscreen with current resolution, if it fails use desktop resolution. */
		if (ChangeDisplaySettings(&settings, CDS_FULLSCREEN | CDS_TEST) != DISP_CHANGE_SUCCESSFUL) {
			RECT r;
			GetWindowRect(GetDesktopWindow(), &r);
			/* Guard against recursion. If we already failed here once, just fall through to
			 * the next ChangeDisplaySettings call which will fail and error out appropriately. */
			if ((int)settings.dmPelsWidth != r.right - r.left || (int)settings.dmPelsHeight != r.bottom - r.top) {
				return this->ChangeResolution(r.right - r.left, r.bottom - r.top);
			}
		}

		if (ChangeDisplaySettings(&settings, CDS_FULLSCREEN) != DISP_CHANGE_SUCCESSFUL) {
			this->MakeWindow(false, resize);  // don't care about the result
			return false;  // the request failed
		}
	} else if (this->fullscreen) {
		/* restore display? */
		ChangeDisplaySettings(nullptr, 0);
		/* restore the resolution */
		this->width = _bck_resolution.width;
		this->height = _bck_resolution.height;
	}

	{
		RECT r;
		DWORD style, showstyle;
		int w, h;

		showstyle = SW_SHOWNORMAL;
		this->fullscreen = full_screen;
		if (this->fullscreen) {
			style = WS_POPUP;
			SetRect(&r, 0, 0, this->width_org, this->height_org);
		} else {
			style = WS_OVERLAPPEDWINDOW;
			/* On window creation, check if we were in maximize mode before */
			if (_window_maximize) showstyle = SW_SHOWMAXIMIZED;
			SetRect(&r, 0, 0, this->width, this->height);
		}

		AdjustWindowRect(&r, style, FALSE);
		w = r.right - r.left;
		h = r.bottom - r.top;

		if (this->main_wnd != nullptr) {
			if (!_window_maximize && resize) SetWindowPos(this->main_wnd, 0, 0, 0, w, h, SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER | SWP_NOMOVE);
		} else {
			int x = 0;
			int y = 0;

			/* For windowed mode, center on the workspace of the primary display. */
			if (!this->fullscreen) {
				MONITORINFO mi;
				mi.cbSize = sizeof(mi);
				GetMonitorInfo(MonitorFromWindow(0, MONITOR_DEFAULTTOPRIMARY), &mi);

				x = (mi.rcWork.right - mi.rcWork.left - w) / 2;
				y = (mi.rcWork.bottom - mi.rcWork.top - h) / 2;
			}

			std::string caption = VideoDriver::GetCaption();
			this->main_wnd = CreateWindow(L"OTTD", OTTD2FS(caption).c_str(), style, x, y, w, h, 0, 0, GetModuleHandle(nullptr), this);
			if (this->main_wnd == nullptr) UserError("CreateWindow failed");
			ShowWindow(this->main_wnd, showstyle);
		}
	}

	BlitterFactory::GetCurrentBlitter()->PostResize();

	GameSizeChanged();
	return true;
}

/** Forward key presses to the window system. */
static LRESULT HandleCharMsg(uint keycode, char32_t charcode)
{
	static char32_t prev_char = 0;

	/* Did we get a lead surrogate? If yes, store and exit. */
	if (Utf16IsLeadSurrogate(charcode)) {
		if (prev_char != 0) Debug(driver, 1, "Got two UTF-16 lead surrogates, dropping the first one");
		prev_char = charcode;
		return 0;
	}

	/* Stored lead surrogate and incoming trail surrogate? Combine and forward to input handling. */
	if (prev_char != 0) {
		if (Utf16IsTrailSurrogate(charcode)) {
			charcode = Utf16DecodeSurrogate(prev_char, charcode);
		} else {
			Debug(driver, 1, "Got an UTF-16 lead surrogate without a trail surrogate, dropping the lead surrogate");
		}
	}
	prev_char = 0;

	HandleKeypress(keycode, charcode);

	return 0;
}

/** Should we draw the composition string ourself, i.e is this a normal IME? */
static bool DrawIMECompositionString()
{
	return (_imm_props & IME_PROP_AT_CARET) && !(_imm_props & IME_PROP_SPECIAL_UI);
}

/** Set position of the composition window to the caret position. */
static void SetCompositionPos(HWND hwnd)
{
	HIMC hIMC = ImmGetContext(hwnd);
	if (hIMC != nullptr) {
		COMPOSITIONFORM cf;
		cf.dwStyle = CFS_POINT;

		if (EditBoxInGlobalFocus()) {
			/* Get caret position. */
			Point pt = _focused_window->GetCaretPosition();
			cf.ptCurrentPos.x = _focused_window->left + pt.x;
			cf.ptCurrentPos.y = _focused_window->top  + pt.y;
		} else {
			cf.ptCurrentPos.x = 0;
			cf.ptCurrentPos.y = 0;
		}
		ImmSetCompositionWindow(hIMC, &cf);
	}
	ImmReleaseContext(hwnd, hIMC);
}

/** Set the position of the candidate window. */
static void SetCandidatePos(HWND hwnd)
{
	HIMC hIMC = ImmGetContext(hwnd);
	if (hIMC != nullptr) {
		CANDIDATEFORM cf;
		cf.dwIndex = 0;
		cf.dwStyle = CFS_EXCLUDE;

		if (EditBoxInGlobalFocus()) {
			Point pt = _focused_window->GetCaretPosition();
			cf.ptCurrentPos.x = _focused_window->left + pt.x;
			cf.ptCurrentPos.y = _focused_window->top  + pt.y;
			if (_focused_window->window_class == WC_CONSOLE) {
				cf.rcArea.left   = _focused_window->left;
				cf.rcArea.top    = _focused_window->top;
				cf.rcArea.right  = _focused_window->left + _focused_window->width;
				cf.rcArea.bottom = _focused_window->top  + _focused_window->height;
			} else {
				cf.rcArea.left   = _focused_window->left + _focused_window->nested_focus->pos_x;
				cf.rcArea.top    = _focused_window->top  + _focused_window->nested_focus->pos_y;
				cf.rcArea.right  = cf.rcArea.left + _focused_window->nested_focus->current_x;
				cf.rcArea.bottom = cf.rcArea.top  + _focused_window->nested_focus->current_y;
			}
		} else {
			cf.ptCurrentPos.x = 0;
			cf.ptCurrentPos.y = 0;
			SetRectEmpty(&cf.rcArea);
		}
		ImmSetCandidateWindow(hIMC, &cf);
	}
	ImmReleaseContext(hwnd, hIMC);
}

/** Handle WM_IME_COMPOSITION messages. */
static LRESULT HandleIMEComposition(HWND hwnd, WPARAM wParam, LPARAM lParam)
{
	HIMC hIMC = ImmGetContext(hwnd);

	if (hIMC != nullptr) {
		if (lParam & GCS_RESULTSTR) {
			/* Read result string from the IME. */
			LONG len = ImmGetCompositionString(hIMC, GCS_RESULTSTR, nullptr, 0); // Length is always in bytes, even in UNICODE build.
			std::wstring str(len + 1, L'\0');
			len = ImmGetCompositionString(hIMC, GCS_RESULTSTR, str.data(), len);
			str[len / sizeof(wchar_t)] = L'\0';

			/* Transmit text to windowing system. */
			if (len > 0) {
				HandleTextInput({}, true); // Clear marked string.
				HandleTextInput(FS2OTTD(str));
			}
			SetCompositionPos(hwnd);

			/* Don't pass the result string on to the default window proc. */
			lParam &= ~(GCS_RESULTSTR | GCS_RESULTCLAUSE | GCS_RESULTREADCLAUSE | GCS_RESULTREADSTR);
		}

		if ((lParam & GCS_COMPSTR) && DrawIMECompositionString()) {
			/* Read composition string from the IME. */
			LONG len = ImmGetCompositionString(hIMC, GCS_COMPSTR, nullptr, 0); // Length is always in bytes, even in UNICODE build.
			std::wstring str(len + 1, L'\0');
			len = ImmGetCompositionString(hIMC, GCS_COMPSTR, str.data(), len);
			str[len / sizeof(wchar_t)] = L'\0';

			if (len > 0) {
				static char utf8_buf[1024];
				convert_from_fs(str, utf8_buf);

				/* Convert caret position from bytes in the input string to a position in the UTF-8 encoded string. */
				LONG caret_bytes = ImmGetCompositionString(hIMC, GCS_CURSORPOS, nullptr, 0);
				Utf8View view(utf8_buf);
				auto caret = view.begin();
				const auto end = view.end();
				for (const wchar_t *c = str.c_str(); *c != '\0' && caret != end && caret_bytes > 0; c++, caret_bytes--) {
					/* Skip DBCS lead bytes or leading surrogates. */
					if (Utf16IsLeadSurrogate(*c)) {
						c++;
						caret_bytes--;
					}
					++caret;
				}

				HandleTextInput(utf8_buf, true, caret.GetByteOffset());
			} else {
				HandleTextInput({}, true);
			}

			lParam &= ~(GCS_COMPSTR | GCS_COMPATTR | GCS_COMPCLAUSE | GCS_CURSORPOS | GCS_DELTASTART);
		}
	}
	ImmReleaseContext(hwnd, hIMC);

	return lParam != 0 ? DefWindowProc(hwnd, WM_IME_COMPOSITION, wParam, lParam) : 0;
}

/** Clear the current composition string. */
static void CancelIMEComposition(HWND hwnd)
{
	HIMC hIMC = ImmGetContext(hwnd);
	if (hIMC != nullptr) ImmNotifyIME(hIMC, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
	ImmReleaseContext(hwnd, hIMC);
	/* Clear any marked string from the current edit box. */
	HandleTextInput({}, true);
}

#if defined(_MSC_VER) && defined(NTDDI_WIN10_RS4)
/* We only use WinRT functions on Windows 10 or later. Unfortunately, newer Windows SDKs are now
 * linking the two functions below directly instead of using dynamic linking as previously.
 * To avoid any runtime linking errors on Windows 7 or older, we stub in our own dynamic
 * linking trampoline. */

static LibraryLoader _combase("combase.dll");

extern "C" int32_t __stdcall WINRT_IMPL_RoOriginateLanguageException(int32_t error, void *message, void *languageException) noexcept
{
	typedef BOOL(WINAPI *PFNRoOriginateLanguageException)(int32_t, void *, void *);
	static PFNRoOriginateLanguageException RoOriginateLanguageException = _combase.GetFunction("RoOriginateLanguageException");

	if (RoOriginateLanguageException != nullptr) {
		return RoOriginateLanguageException(error, message, languageException);
	} else {
		return TRUE;
	}
}

extern "C" int32_t __stdcall WINRT_IMPL_RoGetActivationFactory(void *classId, winrt::guid const &iid, void **factory) noexcept
{
	typedef BOOL(WINAPI *PFNRoGetActivationFactory)(void *, winrt::guid const &, void **);
	static PFNRoGetActivationFactory RoGetActivationFactory = _combase.GetFunction("RoGetActivationFactory");

	if (RoGetActivationFactory != nullptr) {
		return RoGetActivationFactory(classId, iid, factory);
	} else {
		*factory = nullptr;
		return winrt::impl::error_class_not_available;
	}
}
#endif

static bool IsDarkModeEnabled()
{
	/* Only build if SDK is Windows 10 1803 or later. */
#if defined(_MSC_VER) && defined(NTDDI_WIN10_RS4)
	if (IsWindows10OrGreater()) {
		try {
			/*
			 * The official documented way to find out if the system is running in dark mode is to
			 * check the brightness of the current theme's colour.
			 * See: https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/ui/apply-windows-themes#know-when-dark-mode-is-enabled
			 *
			 * There are other variants floating around on the Internet, but they all rely on internal,
			 * undocumented Windows functions that may or may not work in the future.
			 */
			winrt::Windows::UI::ViewManagement::UISettings settings;
			auto foreground = settings.GetColorValue(winrt::Windows::UI::ViewManagement::UIColorType::Foreground);

			/* If the Foreground colour is a light colour, the system is running in dark mode. */
			return ((5 * foreground.G) + (2 * foreground.R) + foreground.B) > (8 * 128);
		} catch (...) {
			/* Some kind of error, like a too old Windows version. Just return false. */
			return false;
		}
	}
#endif /* defined(_MSC_VER) && defined(NTDDI_WIN10_RS4) */

	return false;
}

static void SetDarkModeForWindow(HWND hWnd, bool dark_mode)
{
	/* Only build if SDK is Windows 10+. */
#if defined(NTDDI_WIN10)
	if (!IsWindows10OrGreater()) return;

	/* This function is documented, but not supported on all Windows 10/11 SDK builds. For this
	 * reason, the code uses dynamic loading and ignores any errors for a best-effort result. */
	static LibraryLoader _dwmapi("dwmapi.dll");
	typedef HRESULT(WINAPI *PFNDWMSETWINDOWATTRIBUTE)(HWND, DWORD, LPCVOID, DWORD);
	static const PFNDWMSETWINDOWATTRIBUTE DwmSetWindowAttribute = _dwmapi.GetFunction("DwmSetWindowAttribute");

	if (DwmSetWindowAttribute != nullptr) {
		/* Contrary to the published documentation, DWMWA_USE_IMMERSIVE_DARK_MODE does not change the
		 * window chrome according to the current theme, but forces it to either light or dark mode.
		 * As such, the set value has to depend on the current theming mode.*/
		BOOL value = dark_mode ? TRUE : FALSE;
		if (DwmSetWindowAttribute(hWnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &value, sizeof(value)) != S_OK) {
			DwmSetWindowAttribute(hWnd, 19 /* DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1 */, &value, sizeof(value)); // Ignore errors. It works or it doesn't.
		}
	}
#endif /* defined(NTDDI_WIN10) */
}

LRESULT CALLBACK WndProcGdi(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	static uint32_t keycode = 0;
	static bool console = false;

	const float SCROLL_BUILTIN_MULTIPLIER = 14.0f / WHEEL_DELTA;

	VideoDriver_Win32Base *video_driver = (VideoDriver_Win32Base *)GetWindowLongPtr(hwnd, GWLP_USERDATA);

	switch (msg) {
		case WM_CREATE:
			SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)((LPCREATESTRUCT)lParam)->lpCreateParams);
			_cursor.in_window = false; // Win32 has mouse tracking.
			SetCompositionPos(hwnd);
			_imm_props = ImmGetProperty(GetKeyboardLayout(0), IGP_PROPERTY);

			/* Enable dark mode theming for window chrome. */
			SetDarkModeForWindow(hwnd, IsDarkModeEnabled());
			break;

		case WM_SETTINGCHANGE:
			/* Synchronize dark mode theming state. */
			SetDarkModeForWindow(hwnd, IsDarkModeEnabled());
			break;

		case WM_PAINT: {
			RECT r;
			GetUpdateRect(hwnd, &r, FALSE);
			video_driver->MakeDirty(r.left, r.top, r.right - r.left, r.bottom - r.top);

			ValidateRect(hwnd, nullptr);
			return 0;
		}

		case WM_PALETTECHANGED:
			if ((HWND)wParam == hwnd) return 0;
			[[fallthrough]];

		case WM_QUERYNEWPALETTE:
			video_driver->PaletteChanged(hwnd);
			return 0;

		case WM_CLOSE:
			HandleExitGameRequest();
			return 0;

		case WM_DESTROY:
			if (_window_maximize) _cur_resolution = _bck_resolution;
			return 0;

		case WM_LBUTTONDOWN:
			SetCapture(hwnd);
			_left_button_down = true;
			HandleMouseEvents();
			return 0;

		case WM_LBUTTONUP:
			ReleaseCapture();
			_left_button_down = false;
			_left_button_clicked = false;
			HandleMouseEvents();
			return 0;

		case WM_RBUTTONDOWN:
			SetCapture(hwnd);
			_right_button_down = true;
			_right_button_clicked = true;
			HandleMouseEvents();
			return 0;

		case WM_RBUTTONUP:
			ReleaseCapture();
			_right_button_down = false;
			HandleMouseEvents();
			return 0;

		case WM_MOUSELEAVE:
			UndrawMouseCursor();
			_cursor.in_window = false;

			if (!_left_button_down && !_right_button_down) MyShowCursor(true);
			return 0;

		case WM_MOUSEMOVE: {
			int x = (int16_t)LOWORD(lParam);
			int y = (int16_t)HIWORD(lParam);

			/* If the mouse was not in the window and it has moved it means it has
			 * come into the window, so start drawing the mouse. Also start
			 * tracking the mouse for exiting the window */
			if (!_cursor.in_window) {
				_cursor.in_window = true;
				TRACKMOUSEEVENT tme;
				tme.cbSize = sizeof(tme);
				tme.dwFlags = TME_LEAVE;
				tme.hwndTrack = hwnd;

				TrackMouseEvent(&tme);
			}

			if (_cursor.fix_at) {
				/* Get all queued mouse events now in case we have to warp the cursor. In the
				 * end, we only care about the current mouse position and not bygone events. */
				MSG m;
				while (PeekMessage(&m, hwnd, WM_MOUSEMOVE, WM_MOUSEMOVE, PM_REMOVE | PM_NOYIELD | PM_QS_INPUT)) {
					x = (int16_t)LOWORD(m.lParam);
					y = (int16_t)HIWORD(m.lParam);
				}
			}

			if (_cursor.UpdateCursorPosition(x, y)) {
				POINT pt;
				pt.x = _cursor.pos.x;
				pt.y = _cursor.pos.y;
				ClientToScreen(hwnd, &pt);
				SetCursorPos(pt.x, pt.y);
			}
			MyShowCursor(false);
			HandleMouseEvents();
			return 0;
		}

		case WM_INPUTLANGCHANGE:
			_imm_props = ImmGetProperty(GetKeyboardLayout(0), IGP_PROPERTY);
			break;

		case WM_IME_SETCONTEXT:
			/* Don't show the composition window if we draw the string ourself. */
			if (DrawIMECompositionString()) lParam &= ~ISC_SHOWUICOMPOSITIONWINDOW;
			break;

		case WM_IME_STARTCOMPOSITION:
			SetCompositionPos(hwnd);
			if (DrawIMECompositionString()) return 0;
			break;

		case WM_IME_COMPOSITION:
			return HandleIMEComposition(hwnd, wParam, lParam);

		case WM_IME_ENDCOMPOSITION:
			/* Clear any pending composition string. */
			HandleTextInput({}, true);
			if (DrawIMECompositionString()) return 0;
			break;

		case WM_IME_NOTIFY:
			if (wParam == IMN_OPENCANDIDATE) SetCandidatePos(hwnd);
			break;

		case WM_DEADCHAR:
			console = GB(lParam, 16, 8) == 41;
			return 0;

		case WM_CHAR: {
			uint scancode = GB(lParam, 16, 8);
			uint charcode = wParam;

			/* If the console key is a dead-key, we need to press it twice to get a WM_CHAR message.
			 * But we then get two WM_CHAR messages, so ignore the first one */
			if (console && scancode == 41) {
				console = false;
				return 0;
			}

			/* IMEs and other input methods sometimes send a WM_CHAR without a WM_KEYDOWN,
			 * clear the keycode so a previous WM_KEYDOWN doesn't become 'stuck'. */
			uint cur_keycode = keycode;
			keycode = 0;

			return HandleCharMsg(cur_keycode, charcode);
		}

		case WM_KEYDOWN: {
			/* No matter the keyboard layout, we will map the '~' to the console. */
			uint scancode = GB(lParam, 16, 8);
			keycode = scancode == 41 ? (uint)WKC_BACKQUOTE : MapWindowsKey(wParam);

			uint charcode = MapVirtualKey(wParam, MAPVK_VK_TO_CHAR);

			/* No character translation? */
			if (charcode == 0) {
				HandleKeypress(keycode, 0);
				return 0;
			}

			/* If an edit box is in focus, wait for the corresponding WM_CHAR message. */
			if (!EditBoxInGlobalFocus()) {
				/* Is the console key a dead key? If yes, ignore the first key down event. */
				if (HasBit(charcode, 31) && !console) {
					if (scancode == 41) {
						console = true;
						return 0;
					}
				}
				console = false;

				/* IMEs and other input methods sometimes send a WM_CHAR without a WM_KEYDOWN,
				 * clear the keycode so a previous WM_KEYDOWN doesn't become 'stuck'. */
				uint cur_keycode = keycode;
				keycode = 0;

				return HandleCharMsg(cur_keycode, LOWORD(charcode));
			}

			return 0;
		}

		case WM_SYSKEYDOWN: // user presses F10 or Alt, both activating the title-menu
			switch (wParam) {
				case VK_RETURN:
				case 'F': // Full Screen on ALT + ENTER/F
					ToggleFullScreen(!video_driver->fullscreen);
					return 0;

				case VK_MENU: // Just ALT
					return 0; // do nothing

				case VK_F10: // F10, ignore activation of menu
					HandleKeypress(MapWindowsKey(wParam), 0);
					return 0;

				default: // ALT in combination with something else
					HandleKeypress(MapWindowsKey(wParam), 0);
					break;
			}
			break;

		case WM_SIZE:
			if (wParam != SIZE_MINIMIZED) {
				/* Set maximized flag when we maximize (obviously), but also when we
				 * switched to fullscreen from a maximized state */
				_window_maximize = (wParam == SIZE_MAXIMIZED || (_window_maximize && _fullscreen));
				if (_window_maximize || _fullscreen) _bck_resolution = _cur_resolution;
				video_driver->ClientSizeChanged(LOWORD(lParam), HIWORD(lParam));
			}
			return 0;

		case WM_SIZING: {
			RECT *r = (RECT*)lParam;
			RECT r2;
			int w, h;

			SetRect(&r2, 0, 0, 0, 0);
			AdjustWindowRect(&r2, GetWindowLong(hwnd, GWL_STYLE), FALSE);

			w = r->right - r->left - (r2.right - r2.left);
			h = r->bottom - r->top - (r2.bottom - r2.top);
			w = std::max(w, 64);
			h = std::max(h, 64);
			SetRect(&r2, 0, 0, w, h);

			AdjustWindowRect(&r2, GetWindowLong(hwnd, GWL_STYLE), FALSE);
			w = r2.right - r2.left;
			h = r2.bottom - r2.top;

			switch (wParam) {
				case WMSZ_BOTTOM:
					r->bottom = r->top + h;
					break;

				case WMSZ_BOTTOMLEFT:
					r->bottom = r->top + h;
					r->left = r->right - w;
					break;

				case WMSZ_BOTTOMRIGHT:
					r->bottom = r->top + h;
					r->right = r->left + w;
					break;

				case WMSZ_LEFT:
					r->left = r->right - w;
					break;

				case WMSZ_RIGHT:
					r->right = r->left + w;
					break;

				case WMSZ_TOP:
					r->top = r->bottom - h;
					break;

				case WMSZ_TOPLEFT:
					r->top = r->bottom - h;
					r->left = r->right - w;
					break;

				case WMSZ_TOPRIGHT:
					r->top = r->bottom - h;
					r->right = r->left + w;
					break;
			}
			return TRUE;
		}

		case WM_DPICHANGED: {
			auto did_adjust = AdjustGUIZoom(true);

			/* Resize the window to match the new DPI setting. */
			RECT *prcNewWindow = (RECT *)lParam;
			SetWindowPos(hwnd,
				nullptr,
				prcNewWindow->left,
				prcNewWindow->top,
				prcNewWindow->right - prcNewWindow->left,
				prcNewWindow->bottom - prcNewWindow->top,
				SWP_NOZORDER | SWP_NOACTIVATE);

			if (did_adjust) ReInitAllWindows(true);

			return 0;
		}

/* needed for wheel */
#if !defined(WM_MOUSEWHEEL)
# define WM_MOUSEWHEEL 0x020A
#endif  /* WM_MOUSEWHEEL */
#if !defined(WM_MOUSEHWHEEL)
# define WM_MOUSEHWHEEL 0x020E
#endif  /* WM_MOUSEHWHEEL */
#if !defined(GET_WHEEL_DELTA_WPARAM)
# define GET_WHEEL_DELTA_WPARAM(wparam) ((short)HIWORD(wparam))
#endif  /* GET_WHEEL_DELTA_WPARAM */

		case WM_MOUSEWHEEL: {
			int delta = GET_WHEEL_DELTA_WPARAM(wParam);

			if (delta < 0) {
				_cursor.wheel++;
			} else if (delta > 0) {
				_cursor.wheel--;
			}

			_cursor.v_wheel -= static_cast<float>(delta) * SCROLL_BUILTIN_MULTIPLIER * _settings_client.gui.scrollwheel_multiplier;
			_cursor.wheel_moved = true;
			HandleMouseEvents();
			return 0;
		}

		case WM_MOUSEHWHEEL: {
			int delta = GET_WHEEL_DELTA_WPARAM(wParam);

			_cursor.h_wheel += static_cast<float>(delta) * SCROLL_BUILTIN_MULTIPLIER * _settings_client.gui.scrollwheel_multiplier;
			_cursor.wheel_moved = true;
			HandleMouseEvents();
			return 0;
		}

		case WM_SETFOCUS:
			video_driver->has_focus = true;
			SetCompositionPos(hwnd);
			break;

		case WM_KILLFOCUS:
			video_driver->has_focus = false;
			break;

		case WM_ACTIVATE: {
			/* Don't do anything if we are closing openttd */
			if (_exit_game) break;

			bool active = (LOWORD(wParam) != WA_INACTIVE);
			bool minimized = (HIWORD(wParam) != 0);
			if (video_driver->fullscreen) {
				if (active && minimized) {
					/* Restore the game window */
					Dimension d = _bck_resolution; // Save current non-fullscreen window size as it will be overwritten by ShowWindow.
					ShowWindow(hwnd, SW_RESTORE);
					_bck_resolution = d;
					video_driver->MakeWindow(true);
				} else if (!active && !minimized) {
					/* Minimise the window and restore desktop */
					ShowWindow(hwnd, SW_MINIMIZE);
					ChangeDisplaySettings(nullptr, 0);
				}
			}
			break;
		}
	}

	return DefWindowProc(hwnd, msg, wParam, lParam);
}

static void RegisterWndClass()
{
	static bool registered = false;

	if (registered) return;

	HINSTANCE hinst = GetModuleHandle(nullptr);
	WNDCLASS wnd = {
		CS_OWNDC,
		WndProcGdi,
		0,
		0,
		hinst,
		LoadIcon(hinst, MAKEINTRESOURCE(100)),
		LoadCursor(nullptr, IDC_ARROW),
		0,
		0,
		L"OTTD"
	};

	registered = true;
	if (!RegisterClass(&wnd)) UserError("RegisterClass failed");
}

static const Dimension default_resolutions[] = {
	{  640,  480 },
	{  800,  600 },
	{ 1024,  768 },
	{ 1152,  864 },
	{ 1280,  800 },
	{ 1280,  960 },
	{ 1280, 1024 },
	{ 1400, 1050 },
	{ 1600, 1200 },
	{ 1680, 1050 },
	{ 1920, 1200 }
};

static void FindResolutions(uint8_t bpp)
{
	_resolutions.clear();

	DEVMODE dm;
	for (uint i = 0; EnumDisplaySettings(nullptr, i, &dm) != 0; i++) {
		if (dm.dmBitsPerPel != bpp || dm.dmPelsWidth < 640 || dm.dmPelsHeight < 480) continue;
		if (std::ranges::find(_resolutions, Dimension(dm.dmPelsWidth, dm.dmPelsHeight)) != _resolutions.end()) continue;
		_resolutions.emplace_back(dm.dmPelsWidth, dm.dmPelsHeight);
	}

	/* We have found no resolutions, show the default list */
	if (_resolutions.empty()) {
		_resolutions.assign(std::begin(default_resolutions), std::end(default_resolutions));
	}

	SortResolutions();
}

void VideoDriver_Win32Base::Initialize()
{
	this->UpdateAutoResolution();

	RegisterWndClass();
	FindResolutions(this->GetFullscreenBpp());

	/* fullscreen uses those */
	this->width  = this->width_org  = _cur_resolution.width;
	this->height = this->height_org = _cur_resolution.height;

	Debug(driver, 2, "Resolution for display: {}x{}", _cur_resolution.width, _cur_resolution.height);
}

void VideoDriver_Win32Base::Stop()
{
	DestroyWindow(this->main_wnd);

	if (this->fullscreen) ChangeDisplaySettings(nullptr, 0);
	MyShowCursor(true);
}
void VideoDriver_Win32Base::MakeDirty(int left, int top, int width, int height)
{
	Rect r = {left, top, left + width, top + height};
	this->dirty_rect = BoundingRect(this->dirty_rect, r);
}

void VideoDriver_Win32Base::CheckPaletteAnim()
{
	if (!CopyPalette(_local_palette)) return;
	this->MakeDirty(0, 0, _screen.width, _screen.height);
}

void VideoDriver_Win32Base::InputLoop()
{
	bool old_ctrl_pressed = _ctrl_pressed;

	_ctrl_pressed = this->has_focus && GetAsyncKeyState(VK_CONTROL) < 0;
	_shift_pressed = this->has_focus && GetAsyncKeyState(VK_SHIFT) < 0;

	/* Speedup when pressing tab, except when using ALT+TAB
	 * to switch to another application. */
	this->fast_forward_key_pressed = this->has_focus && GetAsyncKeyState(VK_TAB) < 0 && GetAsyncKeyState(VK_MENU) >= 0;

	/* Determine which directional keys are down. */
	if (this->has_focus) {
		_dirkeys =
			(GetAsyncKeyState(VK_LEFT) < 0 ? 1 : 0) +
			(GetAsyncKeyState(VK_UP) < 0 ? 2 : 0) +
			(GetAsyncKeyState(VK_RIGHT) < 0 ? 4 : 0) +
			(GetAsyncKeyState(VK_DOWN) < 0 ? 8 : 0);
	} else {
		_dirkeys = 0;
	}

	if (old_ctrl_pressed != _ctrl_pressed) HandleCtrlChanged();
}

bool VideoDriver_Win32Base::PollEvent()
{
	MSG mesg;

	if (!PeekMessage(&mesg, nullptr, 0, 0, PM_REMOVE)) return false;

	/* Convert key messages to char messages if we want text input. */
	if (EditBoxInGlobalFocus()) TranslateMessage(&mesg);
	DispatchMessage(&mesg);

	return true;
}

void VideoDriver_Win32Base::MainLoop()
{
	this->StartGameThread();

	for (;;) {
		if (_exit_game) break;

		this->Tick();
		this->SleepTillNextTick();
	}

	this->StopGameThread();
}

void VideoDriver_Win32Base::ClientSizeChanged(int w, int h, bool force)
{
	/* Allocate backing store of the new size. */
	if (this->AllocateBackingStore(w, h, force)) {
		CopyPalette(_local_palette, true);

		BlitterFactory::GetCurrentBlitter()->PostResize();

		GameSizeChanged();
	}
}

bool VideoDriver_Win32Base::ChangeResolution(int w, int h)
{
	if (_window_maximize) ShowWindow(this->main_wnd, SW_SHOWNORMAL);

	this->width = this->width_org = w;
	this->height = this->height_org = h;

	return this->MakeWindow(_fullscreen); // _wnd.fullscreen screws up ingame resolution switching
}

bool VideoDriver_Win32Base::ToggleFullscreen(bool full_screen)
{
	bool res = this->MakeWindow(full_screen);

	InvalidateWindowClassesData(WC_GAME_OPTIONS, 3);
	return res;
}

void VideoDriver_Win32Base::EditBoxLostFocus()
{
	CancelIMEComposition(this->main_wnd);
	SetCompositionPos(this->main_wnd);
	SetCandidatePos(this->main_wnd);
}

static BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC, LPRECT, LPARAM data)
{
	auto &list = *reinterpret_cast<std::vector<int>*>(data);

	MONITORINFOEX monitorInfo = {};
	monitorInfo.cbSize = sizeof(MONITORINFOEX);
	GetMonitorInfo(hMonitor, &monitorInfo);

	DEVMODE devMode = {};
	devMode.dmSize = sizeof(DEVMODE);
	devMode.dmDriverExtra = 0;
	EnumDisplaySettings(monitorInfo.szDevice, ENUM_CURRENT_SETTINGS, &devMode);

	if (devMode.dmDisplayFrequency != 0) list.push_back(devMode.dmDisplayFrequency);
	return true;
}

std::vector<int> VideoDriver_Win32Base::GetListOfMonitorRefreshRates()
{
	std::vector<int> rates = {};
	EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&rates));
	return rates;
}

Dimension VideoDriver_Win32Base::GetScreenSize() const
{
	return { static_cast<uint>(GetSystemMetrics(SM_CXSCREEN)), static_cast<uint>(GetSystemMetrics(SM_CYSCREEN)) };
}

float VideoDriver_Win32Base::GetDPIScale()
{
	typedef UINT (WINAPI *PFNGETDPIFORWINDOW)(HWND hwnd);
	typedef UINT (WINAPI *PFNGETDPIFORSYSTEM)(VOID);
	typedef HRESULT (WINAPI *PFNGETDPIFORMONITOR)(HMONITOR hMonitor, int dpiType, UINT *dpiX, UINT *dpiY);

	static PFNGETDPIFORWINDOW _GetDpiForWindow = nullptr;
	static PFNGETDPIFORSYSTEM _GetDpiForSystem = nullptr;
	static PFNGETDPIFORMONITOR _GetDpiForMonitor = nullptr;

	static bool init_done = false;
	if (!init_done) {
		init_done = true;
		static LibraryLoader _user32("user32.dll");
		static LibraryLoader _shcore("shcore.dll");
		_GetDpiForWindow = _user32.GetFunction("GetDpiForWindow");
		_GetDpiForSystem = _user32.GetFunction("GetDpiForSystem");
		_GetDpiForMonitor = _shcore.GetFunction("GetDpiForMonitor");
	}

	UINT cur_dpi = 0;

	if (cur_dpi == 0 && _GetDpiForWindow != nullptr && this->main_wnd != nullptr) {
		/* Per window DPI is supported since Windows 10 Ver 1607. */
		cur_dpi = _GetDpiForWindow(this->main_wnd);
	}
	if (cur_dpi == 0 && _GetDpiForMonitor != nullptr && this->main_wnd != nullptr) {
		/* Per monitor is supported since Windows 8.1. */
		UINT dpiX, dpiY;
		if (SUCCEEDED(_GetDpiForMonitor(MonitorFromWindow(this->main_wnd, MONITOR_DEFAULTTOPRIMARY), 0 /* MDT_EFFECTIVE_DPI */, &dpiX, &dpiY))) {
			cur_dpi = dpiX; // X and Y are always identical.
		}
	}
	if (cur_dpi == 0 && _GetDpiForSystem != nullptr) {
		/* Fall back to system DPI. */
		cur_dpi = _GetDpiForSystem();
	}

	return cur_dpi > 0 ? cur_dpi / 96.0f : 1.0f; // Default Windows DPI value is 96.
}

bool VideoDriver_Win32Base::LockVideoBuffer()
{
	if (this->buffer_locked) return false;
	this->buffer_locked = true;

	_screen.dst_ptr = this->GetVideoPointer();
	assert(_screen.dst_ptr != nullptr);

	return true;
}

void VideoDriver_Win32Base::UnlockVideoBuffer()
{
	assert(_screen.dst_ptr != nullptr);
	if (_screen.dst_ptr != nullptr) {
		/* Hand video buffer back to the drawing backend. */
		this->ReleaseVideoPointer();
		_screen.dst_ptr = nullptr;
	}

	this->buffer_locked = false;
}


static FVideoDriver_Win32GDI iFVideoDriver_Win32GDI;

std::optional<std::string_view> VideoDriver_Win32GDI::Start(const StringList &param)
{
	if (BlitterFactory::GetCurrentBlitter()->GetScreenDepth() == 0) return "Only real blitters supported";

	this->Initialize();

	this->MakePalette();
	this->AllocateBackingStore(_cur_resolution.width, _cur_resolution.height);
	this->MakeWindow(_fullscreen);

	MarkWholeScreenDirty();

	this->is_game_threaded = !GetDriverParamBool(param, "no_threads") && !GetDriverParamBool(param, "no_thread");

	return std::nullopt;
}

void VideoDriver_Win32GDI::Stop()
{
	DeleteObject(this->gdi_palette);
	DeleteObject(this->dib_sect);

	this->VideoDriver_Win32Base::Stop();
}

bool VideoDriver_Win32GDI::AllocateBackingStore(int w, int h, bool force)
{
	uint bpp = BlitterFactory::GetCurrentBlitter()->GetScreenDepth();

	w = std::max(w, 64);
	h = std::max(h, 64);

	if (!force && w == _screen.width && h == _screen.height) return false;

	BITMAPINFO *bi = (BITMAPINFO *)new char[sizeof(BITMAPINFOHEADER) + sizeof(RGBQUAD) * 256]();
	bi->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);

	bi->bmiHeader.biWidth = this->width = w;
	bi->bmiHeader.biHeight = -(this->height = h);

	bi->bmiHeader.biPlanes = 1;
	bi->bmiHeader.biBitCount = bpp;
	bi->bmiHeader.biCompression = BI_RGB;

	if (this->dib_sect) DeleteObject(this->dib_sect);

	HDC dc = GetDC(0);
	this->dib_sect = CreateDIBSection(dc, bi, DIB_RGB_COLORS, (VOID **)&this->buffer_bits, nullptr, 0);
	if (this->dib_sect == nullptr) {
		delete[] bi;
		UserError("CreateDIBSection failed");
	}
	ReleaseDC(0, dc);

	_screen.width = w;
	_screen.pitch = (bpp == 8) ? Align(w, 4) : w;
	_screen.height = h;
	_screen.dst_ptr = this->GetVideoPointer();

	delete[] bi;
	return true;
}

bool VideoDriver_Win32GDI::AfterBlitterChange()
{
	assert(BlitterFactory::GetCurrentBlitter()->GetScreenDepth() != 0);
	return this->AllocateBackingStore(_screen.width, _screen.height, true) && this->MakeWindow(_fullscreen, false);
}

void VideoDriver_Win32GDI::MakePalette()
{
	CopyPalette(_local_palette, true);

	LOGPALETTE *pal = (LOGPALETTE *)new char[sizeof(LOGPALETTE) + (256 - 1) * sizeof(PALETTEENTRY)]();

	pal->palVersion = 0x300;
	pal->palNumEntries = 256;

	for (uint i = 0; i != 256; i++) {
		pal->palPalEntry[i].peRed   = _local_palette.palette[i].r;
		pal->palPalEntry[i].peGreen = _local_palette.palette[i].g;
		pal->palPalEntry[i].peBlue  = _local_palette.palette[i].b;
		pal->palPalEntry[i].peFlags = 0;

	}
	this->gdi_palette = CreatePalette(pal);
	delete[] pal;
	if (this->gdi_palette == nullptr) UserError("CreatePalette failed!\n");
}

void VideoDriver_Win32GDI::UpdatePalette(HDC dc, uint start, uint count)
{
	RGBQUAD rgb[256];

	for (uint i = 0; i != count; i++) {
		rgb[i].rgbRed   = _local_palette.palette[start + i].r;
		rgb[i].rgbGreen = _local_palette.palette[start + i].g;
		rgb[i].rgbBlue  = _local_palette.palette[start + i].b;
		rgb[i].rgbReserved = 0;
	}

	SetDIBColorTable(dc, start, count, rgb);
}

void VideoDriver_Win32GDI::PaletteChanged(HWND hWnd)
{
	HDC hDC = GetWindowDC(hWnd);
	HPALETTE hOldPalette = SelectPalette(hDC, this->gdi_palette, FALSE);
	UINT nChanged = RealizePalette(hDC);

	SelectPalette(hDC, hOldPalette, TRUE);
	ReleaseDC(hWnd, hDC);
	if (nChanged != 0) this->MakeDirty(0, 0, _screen.width, _screen.height);
}

void VideoDriver_Win32GDI::Paint()
{
	PerformanceMeasurer framerate(PFE_VIDEO);

	if (IsEmptyRect(this->dirty_rect)) return;

	HDC dc = GetDC(this->main_wnd);
	HDC dc2 = CreateCompatibleDC(dc);

	HBITMAP old_bmp = (HBITMAP)SelectObject(dc2, this->dib_sect);
	HPALETTE old_palette = SelectPalette(dc, this->gdi_palette, FALSE);

	if (_local_palette.count_dirty != 0) {
		Blitter *blitter = BlitterFactory::GetCurrentBlitter();

		switch (blitter->UsePaletteAnimation()) {
			case Blitter::PaletteAnimation::VideoBackend:
				this->UpdatePalette(dc2, _local_palette.first_dirty, _local_palette.count_dirty);
				break;

			case Blitter::PaletteAnimation::Blitter: {
				blitter->PaletteAnimate(_local_palette);
				break;
			}

			case Blitter::PaletteAnimation::None:
				break;

			default:
				NOT_REACHED();
		}
		_local_palette.count_dirty = 0;
	}

	BitBlt(dc, 0, 0, this->width, this->height, dc2, 0, 0, SRCCOPY);
	SelectPalette(dc, old_palette, TRUE);
	SelectObject(dc2, old_bmp);
	DeleteDC(dc2);

	ReleaseDC(this->main_wnd, dc);

	this->dirty_rect = {};
}

#ifdef _DEBUG
/* Keep this function here..
 * It allows you to redraw the screen from within the MSVC debugger */
/* static */ int VideoDriver_Win32GDI::RedrawScreenDebug()
{
	static int _fooctr;

	VideoDriver_Win32GDI *drv = static_cast<VideoDriver_Win32GDI *>(VideoDriver::GetInstance());

	_screen.dst_ptr = drv->GetVideoPointer();
	UpdateWindows();

	drv->Paint();
	GdiFlush();

	return _fooctr++;
}
#endif

#ifdef WITH_OPENGL

#include <GL/gl.h>
#include "../3rdparty/opengl/glext.h"
#include "../3rdparty/opengl/wglext.h"
#include "opengl.h"

#ifndef PFD_SUPPORT_COMPOSITION
#	define PFD_SUPPORT_COMPOSITION 0x00008000
#endif

static PFNWGLCREATECONTEXTATTRIBSARBPROC _wglCreateContextAttribsARB = nullptr;
static PFNWGLSWAPINTERVALEXTPROC _wglSwapIntervalEXT = nullptr;
static bool _hasWGLARBCreateContextProfile = false; ///< Is WGL_ARB_create_context_profile supported?

/** Platform-specific callback to get an OpenGL function pointer. */
static OGLProc GetOGLProcAddressCallback(const char *proc)
{
	OGLProc ret = reinterpret_cast<OGLProc>(wglGetProcAddress(proc));
	if (ret == nullptr) {
		/* Non-extension GL function? Try normal loading. */
		ret = reinterpret_cast<OGLProc>(GetProcAddress(GetModuleHandle(L"opengl32"), proc));
	}
	return ret;
}

/**
 * Set the pixel format of a window-
 * @param dc Device context to set the pixel format of.
 * @return std::nullopt on success, error message otherwise.
 */
static std::optional<std::string_view> SelectPixelFormat(HDC dc)
{
	PIXELFORMATDESCRIPTOR pfd = {
		sizeof(PIXELFORMATDESCRIPTOR), // Size of this struct.
		1,                             // Version of this struct.
		PFD_DRAW_TO_WINDOW |           // Require window support.
		PFD_SUPPORT_OPENGL |           // Require OpenGL support.
		PFD_DOUBLEBUFFER   |           // Use double buffering.
		PFD_DEPTH_DONTCARE,
		PFD_TYPE_RGBA,                 // Request RGBA format.
		24,                            // 24 bpp (excluding alpha).
		0, 0, 0, 0, 0, 0, 0, 0,        // Colour bits and shift ignored.
		0, 0, 0, 0, 0,                 // No accumulation buffer.
		0, 0,                          // No depth/stencil buffer.
		0,                             // No aux buffers.
		PFD_MAIN_PLANE,                // Main layer.
		0, 0, 0, 0                     // Ignored/reserved.
	};

	pfd.dwFlags |= PFD_SUPPORT_COMPOSITION; // Make OpenTTD compatible with Aero.

	/* Choose a suitable pixel format. */
	int format = ChoosePixelFormat(dc, &pfd);
	if (format == 0) return "No suitable pixel format found";
	if (!SetPixelFormat(dc, format, &pfd)) return "Can't set pixel format";

	return std::nullopt;
}

/** Bind all WGL extension functions we need. */
static void LoadWGLExtensions()
{
	/* Querying the supported WGL extensions and loading the matching
	 * functions requires a valid context, even for the extensions
	 * regarding context creation. To get around this, we create
	 * a dummy window with a dummy context. The extension functions
	 * remain valid even after this context is destroyed. */
	HWND wnd = CreateWindow(L"STATIC", L"dummy", WS_OVERLAPPEDWINDOW, 0, 0, 0, 0, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
	HDC dc = GetDC(wnd);

	/* Set pixel format of the window. */
	if (SelectPixelFormat(dc) == std::nullopt) {
		/* Create rendering context. */
		HGLRC rc = wglCreateContext(dc);
		if (rc != nullptr) {
			wglMakeCurrent(dc, rc);

#ifdef __MINGW32__
			/* GCC doesn't understand the expected usage of wglGetProcAddress(). */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif /* __MINGW32__ */

			/* Get list of WGL extensions. */
			PFNWGLGETEXTENSIONSSTRINGARBPROC wglGetExtensionsStringARB = (PFNWGLGETEXTENSIONSSTRINGARBPROC)wglGetProcAddress("wglGetExtensionsStringARB");
			if (wglGetExtensionsStringARB != nullptr) {
				std::string_view wgl_exts = wglGetExtensionsStringARB(dc);
				/* Bind supported functions. */
				if (HasStringInExtensionList(wgl_exts, "WGL_ARB_create_context")) {
					_wglCreateContextAttribsARB = (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");
				}
				_hasWGLARBCreateContextProfile = HasStringInExtensionList(wgl_exts, "WGL_ARB_create_context_profile");
				if (HasStringInExtensionList(wgl_exts, "WGL_EXT_swap_control")) {
					_wglSwapIntervalEXT = (PFNWGLSWAPINTERVALEXTPROC)wglGetProcAddress("wglSwapIntervalEXT");
				}
			}

#ifdef __MINGW32__
#pragma GCC diagnostic pop
#endif
			wglMakeCurrent(nullptr, nullptr);
			wglDeleteContext(rc);
		}
	}

	ReleaseDC(wnd, dc);
	DestroyWindow(wnd);
}

static FVideoDriver_Win32OpenGL iFVideoDriver_Win32OpenGL;

std::optional<std::string_view> VideoDriver_Win32OpenGL::Start(const StringList &param)
{
	if (BlitterFactory::GetCurrentBlitter()->GetScreenDepth() == 0) return "Only real blitters supported";

	Dimension old_res = _cur_resolution; // Save current screen resolution in case of errors, as MakeWindow invalidates it.

	LoadWGLExtensions();

	this->Initialize();
	this->MakeWindow(_fullscreen);

	/* Create and initialize OpenGL context. */
	auto err = this->AllocateContext();
	if (err) {
		this->Stop();
		_cur_resolution = old_res;
		return err;
	}

	this->driver_info = GetName();
	this->driver_info += " (";
	this->driver_info += OpenGLBackend::Get()->GetDriverName();
	this->driver_info += ")";

	this->ClientSizeChanged(this->width, this->height, true);
	/* We should have a valid screen buffer now. If not, something went wrong and we should abort. */
	if (_screen.dst_ptr == nullptr) {
		this->Stop();
		_cur_resolution = old_res;
		return "Can't get pointer to screen buffer";
	}
	/* Main loop expects to start with the buffer unmapped. */
	this->ReleaseVideoPointer();

	MarkWholeScreenDirty();

	this->is_game_threaded = !GetDriverParamBool(param, "no_threads") && !GetDriverParamBool(param, "no_thread");

	return std::nullopt;
}

void VideoDriver_Win32OpenGL::Stop()
{
	this->DestroyContext();
	this->VideoDriver_Win32Base::Stop();
}

void VideoDriver_Win32OpenGL::DestroyContext()
{
	OpenGLBackend::Destroy();

	wglMakeCurrent(nullptr, nullptr);
	if (this->gl_rc != nullptr) {
		wglDeleteContext(this->gl_rc);
		this->gl_rc = nullptr;
	}
	if (this->dc != nullptr) {
		ReleaseDC(this->main_wnd, this->dc);
		this->dc = nullptr;
	}
}

void VideoDriver_Win32OpenGL::ToggleVsync(bool vsync)
{
	if (_wglSwapIntervalEXT != nullptr) {
		_wglSwapIntervalEXT(vsync);
	} else if (vsync) {
		Debug(driver, 0, "OpenGL: Vsync requested, but not supported by driver");
	}
}

std::optional<std::string_view> VideoDriver_Win32OpenGL::AllocateContext()
{
	this->dc = GetDC(this->main_wnd);

	auto err = SelectPixelFormat(this->dc);
	if (err) return err;

	HGLRC rc = nullptr;

	/* Create OpenGL device context. Try to get an 3.2+ context if possible. */
	if (_wglCreateContextAttribsARB != nullptr) {
		/* Try for OpenGL 4.5 first. */
		int attribs[] = {
			WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
			WGL_CONTEXT_MINOR_VERSION_ARB, 5,
			WGL_CONTEXT_FLAGS_ARB, _debug_driver_level >= 8 ? WGL_CONTEXT_DEBUG_BIT_ARB : 0,
			_hasWGLARBCreateContextProfile ? WGL_CONTEXT_PROFILE_MASK_ARB : 0, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, // Terminate list if WGL_ARB_create_context_profile isn't supported.
			0
		};
		rc = _wglCreateContextAttribsARB(this->dc, nullptr, attribs);

		if (rc == nullptr) {
			/* Try again for a 3.2 context. */
			attribs[1] = 3;
			attribs[3] = 2;
			rc = _wglCreateContextAttribsARB(this->dc, nullptr, attribs);
		}
	}

	if (rc == nullptr) {
		/* Old OpenGL or old driver, let's hope for the best. */
		rc = wglCreateContext(this->dc);
		if (rc == nullptr) return "Can't create OpenGL context";
	}
	if (!wglMakeCurrent(this->dc, rc)) return "Can't activate GL context";

	this->ToggleVsync(_video_vsync);

	this->gl_rc = rc;
	return OpenGLBackend::Create(&GetOGLProcAddressCallback, this->GetScreenSize());
}

bool VideoDriver_Win32OpenGL::ToggleFullscreen(bool full_screen)
{
	if (_screen.dst_ptr != nullptr) this->ReleaseVideoPointer();
	this->DestroyContext();
	bool res = this->VideoDriver_Win32Base::ToggleFullscreen(full_screen);
	res &= this->AllocateContext() == std::nullopt;
	this->ClientSizeChanged(this->width, this->height, true);
	return res;
}

bool VideoDriver_Win32OpenGL::AfterBlitterChange()
{
	assert(BlitterFactory::GetCurrentBlitter()->GetScreenDepth() != 0);
	this->ClientSizeChanged(this->width, this->height, true);
	return true;
}

void VideoDriver_Win32OpenGL::PopulateSystemSprites()
{
	OpenGLBackend::Get()->PopulateCursorCache();
}

void VideoDriver_Win32OpenGL::ClearSystemSprites()
{
	OpenGLBackend::Get()->ClearCursorCache();
}

bool VideoDriver_Win32OpenGL::AllocateBackingStore(int w, int h, bool force)
{
	if (!force && w == _screen.width && h == _screen.height) return false;

	this->width = w = std::max(w, 64);
	this->height = h = std::max(h, 64);

	if (this->gl_rc == nullptr) return false;

	if (_screen.dst_ptr != nullptr) this->ReleaseVideoPointer();

	this->dirty_rect = {};
	bool res = OpenGLBackend::Get()->Resize(w, h, force);
	SwapBuffers(this->dc);
	_screen.dst_ptr = this->GetVideoPointer();

	return res;
}

void *VideoDriver_Win32OpenGL::GetVideoPointer()
{
	if (BlitterFactory::GetCurrentBlitter()->NeedsAnimationBuffer()) {
		this->anim_buffer = OpenGLBackend::Get()->GetAnimBuffer();
	}
	return OpenGLBackend::Get()->GetVideoBuffer();
}

void VideoDriver_Win32OpenGL::ReleaseVideoPointer()
{
	if (this->anim_buffer != nullptr) OpenGLBackend::Get()->ReleaseAnimBuffer(this->dirty_rect);
	OpenGLBackend::Get()->ReleaseVideoBuffer(this->dirty_rect);
	this->dirty_rect = {};
	_screen.dst_ptr = nullptr;
	this->anim_buffer = nullptr;
}

void VideoDriver_Win32OpenGL::Paint()
{
	PerformanceMeasurer framerate(PFE_VIDEO);

	if (_local_palette.count_dirty != 0) {
		Blitter *blitter = BlitterFactory::GetCurrentBlitter();

		/* Always push a changed palette to OpenGL. */
		OpenGLBackend::Get()->UpdatePalette(_local_palette.palette, _local_palette.first_dirty, _local_palette.count_dirty);
		if (blitter->UsePaletteAnimation() == Blitter::PaletteAnimation::Blitter) {
			blitter->PaletteAnimate(_local_palette);
		}

		_local_palette.count_dirty = 0;
	}

	OpenGLBackend::Get()->Paint();
	OpenGLBackend::Get()->DrawMouseCursor();

	SwapBuffers(this->dc);
}

#endif /* WITH_OPENGL */
