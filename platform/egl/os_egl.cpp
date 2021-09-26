/*************************************************************************/
/*  os_egl.cpp                                                           */
/*************************************************************************/
/*                       This file is part of:                           */
/*                           GODOT ENGINE                                */
/*                      https://godotengine.org                          */
/*************************************************************************/
/* Copyright (c) 2007-2021 Juan Linietsky, Ariel Manzur.                 */
/* Copyright (c) 2014-2021 Godot Engine contributors (cf. AUTHORS.md).   */
/*                                                                       */
/* Permission is hereby granted, free of charge, to any person obtaining */
/* a copy of this software and associated documentation files (the       */
/* "Software"), to deal in the Software without restriction, including   */
/* without limitation the rights to use, copy, modify, merge, publish,   */
/* distribute, sublicense, and/or sell copies of the Software, and to    */
/* permit persons to whom the Software is furnished to do so, subject to */
/* the following conditions:                                             */
/*                                                                       */
/* The above copyright notice and this permission notice shall be        */
/* included in all copies or substantial portions of the Software.       */
/*                                                                       */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,       */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF    */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.*/
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY  */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,  */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE     */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                */
/*************************************************************************/

#include "os_egl.h"

#include "core/os/dir_access.h"
#include "core/print_string.h"
#include "drivers/gles2/rasterizer_gles2.h"
#include "drivers/gles3/rasterizer_gles3.h"
#include "main/main.h"
#include "servers/visual/visual_server_raster.h"
#include "servers/visual/visual_server_wrap_mt.h"

#ifdef HAVE_MNTENT
#include <mntent.h>
#endif

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xinerama.h>
#include <X11/extensions/shape.h>

// ICCCM
#define WM_NormalState 1L // window normal state
#define WM_IconicState 3L // window minimized
// EWMH
#define _NET_WM_STATE_REMOVE 0L // remove/unset property
#define _NET_WM_STATE_ADD 1L // add/set property
#define _NET_WM_STATE_TOGGLE 2L // toggle property

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

//stupid linux.h
#ifdef KEY_TAB
#undef KEY_TAB
#endif

#undef CursorShape

#include <X11/XKBlib.h>

// 2.2 is the first release with multitouch
#define XINPUT_CLIENT_VERSION_MAJOR 2
#define XINPUT_CLIENT_VERSION_MINOR 2

#define VALUATOR_ABSX 0
#define VALUATOR_ABSY 1
#define VALUATOR_PRESSURE 2
#define VALUATOR_TILTX 3
#define VALUATOR_TILTY 4

static const double abs_resolution_mult = 10000.0;
static const double abs_resolution_range_mult = 10.0;

void OS_EGL::initialize_core() {

	crash_handler.initialize();

	OS_Unix::initialize_core();
}

int OS_EGL::get_current_video_driver() const {
	return video_driver_index;
}

Error OS_EGL::initialize(const VideoMode &p_desired, int p_video_driver, int p_audio_driver) {

	long im_event_mask = 0;
	last_button_state = 0;

	xmbstring = NULL;
	x11_window = 0;
	last_click_ms = 0;
	last_click_button_index = -1;
	last_click_pos = Point2(-100, -100);
	args = OS::get_singleton()->get_cmdline_args();
	current_videomode = p_desired;
	main_loop = NULL;
	last_timestamp = 0;
	last_mouse_pos_valid = false;
	last_keyrelease_time = 0;
	xdnd_version = 0;

	XInitThreads();

	/** XLIB INITIALIZATION **/
	x11_display = XOpenDisplay(NULL);

	if (!x11_display) {
		ERR_PRINT("X11 Display is not available");
		return ERR_UNAVAILABLE;
	}

	char *modifiers = NULL;
	Bool xkb_dar = False;
	XAutoRepeatOn(x11_display);
	xkb_dar = XkbSetDetectableAutoRepeat(x11_display, True, NULL);

	// Try to support IME if detectable auto-repeat is supported
	if (xkb_dar == True) {

#ifdef X_HAVE_UTF8_STRING
		// Xutf8LookupString will be used later instead of XmbLookupString before
		// the multibyte sequences can be converted to unicode string.
		modifiers = XSetLocaleModifiers("");
#endif
	}

	if (modifiers == NULL) {
		if (is_stdout_verbose()) {
			WARN_PRINT("IME is disabled");
		}
		XSetLocaleModifiers("@im=none");
		WARN_PRINT("Error setting locale modifiers");
	}

	const char *err;
	xrr_get_monitors = NULL;
	xrr_free_monitors = NULL;
	int xrandr_major = 0;
	int xrandr_minor = 0;
	int event_base, error_base;
	xrandr_ext_ok = XRRQueryExtension(x11_display, &event_base, &error_base);
	xrandr_handle = dlopen("libXrandr.so.2", RTLD_LAZY);
	if (!xrandr_handle) {
		err = dlerror();
		// For some arcane reason, NetBSD now ships libXrandr.so.3 while the rest of the world has libXrandr.so.2...
		// In case this happens for other X11 platforms in the future, let's give it a try too before failing.
		xrandr_handle = dlopen("libXrandr.so.3", RTLD_LAZY);
		if (!xrandr_handle) {
			fprintf(stderr, "could not load libXrandr.so.2, Error: %s\n", err);
		}
	} else {
		XRRQueryVersion(x11_display, &xrandr_major, &xrandr_minor);
		if (((xrandr_major << 8) | xrandr_minor) >= 0x0105) {
			xrr_get_monitors = (xrr_get_monitors_t)dlsym(xrandr_handle, "XRRGetMonitors");
			if (!xrr_get_monitors) {
				err = dlerror();
				fprintf(stderr, "could not find symbol XRRGetMonitors\nError: %s\n", err);
			} else {
				xrr_free_monitors = (xrr_free_monitors_t)dlsym(xrandr_handle, "XRRFreeMonitors");
				if (!xrr_free_monitors) {
					err = dlerror();
					fprintf(stderr, "could not find XRRFreeMonitors\nError: %s\n", err);
					xrr_get_monitors = NULL;
				}
			}
		}
	}

// maybe contextgl wants to be in charge of creating the window
#if defined(OPENGL_ENABLED)
	ContextGL_EGL::ContextType opengl_api_type = ContextGL_EGL::GLES_3_0_COMPATIBLE;

	if (p_video_driver == VIDEO_DRIVER_GLES2) {
		opengl_api_type = ContextGL_EGL::GLES_2_0_COMPATIBLE;
	}

	bool editor = Engine::get_singleton()->is_editor_hint();
	bool gl_initialization_error = false;

	context_gl = NULL;
	while (!context_gl) {
		context_gl = memnew(ContextGL_EGL(x11_display, x11_window, current_videomode, opengl_api_type));

		if (context_gl->initialize() != OK) {
			memdelete(context_gl);
			context_gl = NULL;

			if (GLOBAL_GET("rendering/quality/driver/fallback_to_gles2") || editor) {
				if (p_video_driver == VIDEO_DRIVER_GLES2) {
					gl_initialization_error = true;
					break;
				}

				p_video_driver = VIDEO_DRIVER_GLES2;
				opengl_api_type = ContextGL_EGL::GLES_2_0_COMPATIBLE;
			} else {
				gl_initialization_error = true;
				break;
			}
		}
	}

	while (true) {
		if (opengl_api_type == ContextGL_EGL::GLES_3_0_COMPATIBLE) {
			if (RasterizerGLES3::is_viable() == OK) {
				RasterizerGLES3::register_config();
				RasterizerGLES3::make_current();
				break;
			} else {
				if (GLOBAL_GET("rendering/quality/driver/fallback_to_gles2") || editor) {
					p_video_driver = VIDEO_DRIVER_GLES2;
					opengl_api_type = ContextGL_EGL::GLES_2_0_COMPATIBLE;
					continue;
				} else {
					gl_initialization_error = true;
					break;
				}
			}
		}

		if (opengl_api_type == ContextGL_EGL::GLES_2_0_COMPATIBLE) {
			if (RasterizerGLES2::is_viable() == OK) {
				RasterizerGLES2::register_config();
				RasterizerGLES2::make_current();
				break;
			} else {
				gl_initialization_error = true;
				break;
			}
		}
	}

	if (gl_initialization_error) {
		OS::get_singleton()->alert("Your video card driver does not support any of the supported OpenGL versions.\n"
								   "Please update your drivers or if you have a very old or integrated GPU, upgrade it.\n"
								   "Alternatively, you can force software rendering by running Godot with the `LIBGL_ALWAYS_SOFTWARE=1`\n"
								   "environment variable set, but this will be very slow.",
				"Unable to initialize Video driver");
		return ERR_UNAVAILABLE;
	}

	video_driver_index = p_video_driver;

	context_gl->set_use_vsync(current_videomode.use_vsync);

#endif

	ERR_PRINT("GLES3 selected, but we're about to crash... ;_;")

	visual_server = memnew(VisualServerRaster);
	if (get_render_thread_mode() != RENDER_THREAD_UNSAFE) {
		visual_server = memnew(VisualServerWrapMT(visual_server, get_render_thread_mode() == RENDER_SEPARATE_THREAD));
	}

	if (current_videomode.maximized) {
		current_videomode.maximized = false;
		set_window_maximized(true);
		// borderless fullscreen window mode
	} else if (current_videomode.fullscreen) {
		current_videomode.fullscreen = false;
		set_window_fullscreen(true);
	} else if (current_videomode.borderless_window) {
		Hints hints;
		Atom property;
		hints.flags = 2;
		hints.decorations = 0;
		property = XInternAtom(x11_display, "_MOTIF_WM_HINTS", True);
		if (property != None) {
			XChangeProperty(x11_display, x11_window, property, property, 32, PropModeReplace, (unsigned char *)&hints, 5);
		}
	}

	// make PID known to X11
	{
		const long pid = this->get_process_id();
		Atom net_wm_pid = XInternAtom(x11_display, "_NET_WM_PID", False);
		if (net_wm_pid != None) {
			XChangeProperty(x11_display, x11_window, net_wm_pid, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&pid, 1);
		}
	}

	// disable resizable window
	if (!current_videomode.resizable && !current_videomode.fullscreen) {
		XSizeHints *xsh;
		xsh = XAllocSizeHints();
		xsh->flags = PMinSize | PMaxSize;
		XWindowAttributes xwa;
		if (current_videomode.fullscreen) {
			XGetWindowAttributes(x11_display, DefaultRootWindow(x11_display), &xwa);
		} else {
			XGetWindowAttributes(x11_display, x11_window, &xwa);
		}
		xsh->min_width = xwa.width;
		xsh->max_width = xwa.width;
		xsh->min_height = xwa.height;
		xsh->max_height = xwa.height;
		XSetWMNormalHints(x11_display, x11_window, xsh);
		XFree(xsh);
	}

	if (current_videomode.always_on_top) {
		current_videomode.always_on_top = false;
		set_window_always_on_top(true);
	}

	ERR_FAIL_COND_V(!visual_server, ERR_UNAVAILABLE);
	ERR_FAIL_COND_V(x11_window == 0, ERR_UNAVAILABLE);

	XSetWindowAttributes new_attr;

	new_attr.event_mask = im_event_mask;

	XChangeWindowAttributes(x11_display, x11_window, CWEventMask, &new_attr);

	/* set the titlebar name */
	XStoreName(x11_display, x11_window, "Godot");

	wm_delete = XInternAtom(x11_display, "WM_DELETE_WINDOW", true);
	XSetWMProtocols(x11_display, x11_window, &wm_delete, 1);

	im_active = false;
	im_position = Vector2();

	cursor_size = XcursorGetDefaultSize(x11_display);
	cursor_theme = XcursorGetTheme(x11_display);

	if (!cursor_theme) {
		print_verbose("XcursorGetTheme could not get cursor theme");
		cursor_theme = "default";
	}

	for (int i = 0; i < CURSOR_MAX; i++) {

		cursors[i] = None;
		img[i] = NULL;
	}

	current_cursor = CURSOR_ARROW;

	for (int i = 0; i < CURSOR_MAX; i++) {

		static const char *cursor_file[] = {
			"left_ptr",
			"xterm",
			"hand2",
			"cross",
			"watch",
			"left_ptr_watch",
			"fleur",
			"hand1",
			"X_cursor",
			"sb_v_double_arrow",
			"sb_h_double_arrow",
			"size_bdiag",
			"size_fdiag",
			"hand1",
			"sb_v_double_arrow",
			"sb_h_double_arrow",
			"question_arrow"
		};

		img[i] = XcursorLibraryLoadImage(cursor_file[i], cursor_theme, cursor_size);
		if (img[i]) {
			cursors[i] = XcursorImageLoadCursor(x11_display, img[i]);
		} else {
			print_verbose("Failed loading custom cursor: " + String(cursor_file[i]));
		}
	}

	{
		// Creating an empty/transparent cursor

		// Create 1x1 bitmap
		Pixmap cursormask = XCreatePixmap(x11_display,
				RootWindow(x11_display, DefaultScreen(x11_display)), 1, 1, 1);

		// Fill with zero
		XGCValues xgc;
		xgc.function = GXclear;
		GC gc = XCreateGC(x11_display, cursormask, GCFunction, &xgc);
		XFillRectangle(x11_display, cursormask, gc, 0, 0, 1, 1);

		// Color value doesn't matter. Mask zero means no foreground or background will be drawn
		XColor col = {};

		Cursor cursor = XCreatePixmapCursor(x11_display,
				cursormask, // source (using cursor mask as placeholder, since it'll all be ignored)
				cursormask, // mask
				&col, &col, 0, 0);

		XFreePixmap(x11_display, cursormask);
		XFreeGC(x11_display, gc);

		if (cursor == None) {
			ERR_PRINT("FAILED CREATING CURSOR");
		}

		null_cursor = cursor;
	}
	set_cursor_shape(CURSOR_BUSY);

	//Set Xdnd (drag & drop) support
	Atom XdndAware = XInternAtom(x11_display, "XdndAware", False);
	Atom version = 5;
	if (XdndAware != None) {
		XChangeProperty(x11_display, x11_window, XdndAware, XA_ATOM, 32, PropModeReplace, (unsigned char *)&version, 1);
	}

	xdnd_enter = XInternAtom(x11_display, "XdndEnter", False);
	xdnd_position = XInternAtom(x11_display, "XdndPosition", False);
	xdnd_status = XInternAtom(x11_display, "XdndStatus", False);
	xdnd_action_copy = XInternAtom(x11_display, "XdndActionCopy", False);
	xdnd_drop = XInternAtom(x11_display, "XdndDrop", False);
	xdnd_finished = XInternAtom(x11_display, "XdndFinished", False);
	xdnd_selection = XInternAtom(x11_display, "XdndSelection", False);
	requested = None;

	visual_server->init();

	AudioDriverManager::initialize(p_audio_driver);

	input = memnew(InputDefault);

	window_has_focus = true; // Set focus to true at init

	if (p_desired.layered) {
		set_window_per_pixel_transparency_enabled(true);
	}

	XEvent xevent;
	while (XPending(x11_display) > 0) {
		XNextEvent(x11_display, &xevent);
		if (xevent.type == ConfigureNotify) {
			_window_changed(&xevent);
		}
	}

	return OK;
}

void OS_EGL::set_ime_active(const bool p_active) {
	im_active = p_active;
}

void OS_EGL::set_ime_position(const Point2 &p_pos) {
	im_position = p_pos;
}

String OS_EGL::get_unique_id() const {

	static String machine_id;
	if (machine_id.empty()) {
		if (FileAccess *f = FileAccess::open("/etc/machine-id", FileAccess::READ)) {
			while (machine_id.empty() && !f->eof_reached()) {
				machine_id = f->get_line().strip_edges();
			}
			f->close();
			memdelete(f);
		}
	}
	return machine_id;
}

void OS_EGL::finalize() {
	if (main_loop)
		memdelete(main_loop);
	main_loop = NULL;

	/*
	if (debugger_connection_console) {
		memdelete(debugger_connection_console);
	}
	*/

	memdelete(input);

	cursors_cache.clear();
	visual_server->finish();
	memdelete(visual_server);
	//memdelete(rasterizer);

	if (xrandr_handle)
		dlclose(xrandr_handle);

	if (!OS::get_singleton()->is_no_window_mode_enabled()) {
		XUnmapWindow(x11_display, x11_window);
	}
	XDestroyWindow(x11_display, x11_window);

#if defined(OPENGL_ENABLED)
	memdelete(context_gl);
#endif
	for (int i = 0; i < CURSOR_MAX; i++) {
		if (cursors[i] != None)
			XFreeCursor(x11_display, cursors[i]);
		if (img[i] != NULL)
			XcursorImageDestroy(img[i]);
	};

	XCloseDisplay(x11_display);

	if (xmbstring)
		memfree(xmbstring);

	args.clear();
}

void OS_EGL::set_mouse_mode(MouseMode p_mode) {
}

void OS_EGL::warp_mouse_position(const Point2 &p_to) {
}

OS::MouseMode OS_EGL::get_mouse_mode() const {
	return mouse_mode;
}

int OS_EGL::get_mouse_button_state() const {
	return last_button_state;
}

Point2 OS_EGL::get_mouse_position() const {
	return last_mouse_pos;
}

bool OS_EGL::get_window_per_pixel_transparency_enabled() const {

	if (!is_layered_allowed()) return false;
	return layered_window;
}

void OS_EGL::set_window_per_pixel_transparency_enabled(bool p_enabled) {

	if (!is_layered_allowed()) return;
	if (layered_window != p_enabled) {
		if (p_enabled) {
			layered_window = true;
		} else {
			layered_window = false;
		}
	}
}

void OS_EGL::set_window_title(const String &p_title) {
	XStoreName(x11_display, x11_window, p_title.utf8().get_data());

	Atom _net_wm_name = XInternAtom(x11_display, "_NET_WM_NAME", false);
	Atom utf8_string = XInternAtom(x11_display, "UTF8_STRING", false);
	if (_net_wm_name != None && utf8_string != None) {
		XChangeProperty(x11_display, x11_window, _net_wm_name, utf8_string, 8, PropModeReplace, (unsigned char *)p_title.utf8().get_data(), p_title.utf8().length());
	}
}

void OS_EGL::set_window_mouse_passthrough(const PoolVector2Array &p_region) {
	int event_base, error_base;
	const Bool ext_okay = XShapeQueryExtension(x11_display, &event_base, &error_base);
	if (ext_okay) {
		Region region;
		if (p_region.size() == 0) {
			region = XCreateRegion();
			XRectangle rect;
			rect.x = 0;
			rect.y = 0;
			rect.width = get_real_window_size().x;
			rect.height = get_real_window_size().y;
			XUnionRectWithRegion(&rect, region, region);
		} else {
			XPoint *points = (XPoint *)memalloc(sizeof(XPoint) * p_region.size());
			for (int i = 0; i < p_region.size(); i++) {
				points[i].x = p_region[i].x;
				points[i].y = p_region[i].y;
			}
			region = XPolygonRegion(points, p_region.size(), EvenOddRule);
			memfree(points);
		}
		XShapeCombineRegion(x11_display, x11_window, ShapeInput, 0, 0, region, ShapeSet);
		XDestroyRegion(region);
	}
}

void OS_EGL::set_video_mode(const VideoMode &p_video_mode, int p_screen) {
}

OS::VideoMode OS_EGL::get_video_mode(int p_screen) const {
	return current_videomode;
}

void OS_EGL::get_fullscreen_mode_list(List<VideoMode> *p_list, int p_screen) const {
}

void OS_EGL::set_wm_fullscreen(bool p_enabled) {
	if (p_enabled && !get_borderless_window()) {
		// remove decorations if the window is not already borderless
		Hints hints;
		Atom property;
		hints.flags = 2;
		hints.decorations = 0;
		property = XInternAtom(x11_display, "_MOTIF_WM_HINTS", True);
		if (property != None) {
			XChangeProperty(x11_display, x11_window, property, property, 32, PropModeReplace, (unsigned char *)&hints, 5);
		}
	}

	if (p_enabled && !is_window_resizable()) {
		// Set the window as resizable to prevent window managers to ignore the fullscreen state flag.
		XSizeHints *xsh;

		xsh = XAllocSizeHints();
		xsh->flags = 0L;
		XSetWMNormalHints(x11_display, x11_window, xsh);
		XFree(xsh);
	}

	// Using EWMH -- Extended Window Manager Hints
	XEvent xev;
	Atom wm_state = XInternAtom(x11_display, "_NET_WM_STATE", False);
	Atom wm_fullscreen = XInternAtom(x11_display, "_NET_WM_STATE_FULLSCREEN", False);

	memset(&xev, 0, sizeof(xev));
	xev.type = ClientMessage;
	xev.xclient.window = x11_window;
	xev.xclient.message_type = wm_state;
	xev.xclient.format = 32;
	xev.xclient.data.l[0] = p_enabled ? _NET_WM_STATE_ADD : _NET_WM_STATE_REMOVE;
	xev.xclient.data.l[1] = wm_fullscreen;
	xev.xclient.data.l[2] = 0;

	XSendEvent(x11_display, DefaultRootWindow(x11_display), False, SubstructureRedirectMask | SubstructureNotifyMask, &xev);

	// set bypass compositor hint
	Atom bypass_compositor = XInternAtom(x11_display, "_NET_WM_BYPASS_COMPOSITOR", False);
	unsigned long compositing_disable_on = p_enabled ? 1 : 0;
	if (bypass_compositor != None) {
		XChangeProperty(x11_display, x11_window, bypass_compositor, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&compositing_disable_on, 1);
	}

	XFlush(x11_display);

	if (!p_enabled) {
		// Reset the non-resizable flags if we un-set these before.
		Size2 size = get_window_size();
		XSizeHints *xsh;
		xsh = XAllocSizeHints();
		if (!is_window_resizable()) {
			xsh->flags = PMinSize | PMaxSize;
			xsh->min_width = size.x;
			xsh->max_width = size.x;
			xsh->min_height = size.y;
			xsh->max_height = size.y;
		} else {
			xsh->flags = 0L;
			if (min_size != Size2()) {
				xsh->flags |= PMinSize;
				xsh->min_width = min_size.x;
				xsh->min_height = min_size.y;
			}
			if (max_size != Size2()) {
				xsh->flags |= PMaxSize;
				xsh->max_width = max_size.x;
				xsh->max_height = max_size.y;
			}
		}
		XSetWMNormalHints(x11_display, x11_window, xsh);
		XFree(xsh);

		// put back or remove decorations according to the last set borderless state
		Hints hints;
		Atom property;
		hints.flags = 2;
		hints.decorations = current_videomode.borderless_window ? 0 : 1;
		property = XInternAtom(x11_display, "_MOTIF_WM_HINTS", True);
		if (property != None) {
			XChangeProperty(x11_display, x11_window, property, property, 32, PropModeReplace, (unsigned char *)&hints, 5);
		}
	}
}

void OS_EGL::set_wm_above(bool p_enabled) {
	Atom wm_state = XInternAtom(x11_display, "_NET_WM_STATE", False);
	Atom wm_above = XInternAtom(x11_display, "_NET_WM_STATE_ABOVE", False);

	XClientMessageEvent xev;
	memset(&xev, 0, sizeof(xev));
	xev.type = ClientMessage;
	xev.window = x11_window;
	xev.message_type = wm_state;
	xev.format = 32;
	xev.data.l[0] = p_enabled ? _NET_WM_STATE_ADD : _NET_WM_STATE_REMOVE;
	xev.data.l[1] = wm_above;
	xev.data.l[3] = 1;
	XSendEvent(x11_display, DefaultRootWindow(x11_display), False, SubstructureRedirectMask | SubstructureNotifyMask, (XEvent *)&xev);
}

int OS_EGL::get_screen_count() const {
	// Using Xinerama Extension
	int event_base, error_base;
	const Bool ext_okay = XineramaQueryExtension(x11_display, &event_base, &error_base);
	if (!ext_okay) return 0;

	int count;
	XineramaScreenInfo *xsi = XineramaQueryScreens(x11_display, &count);
	XFree(xsi);
	return count;
}

int OS_EGL::get_current_screen() const {
	int x, y;
	Window child;
	XTranslateCoordinates(x11_display, x11_window, DefaultRootWindow(x11_display), 0, 0, &x, &y, &child);

	int count = get_screen_count();
	for (int i = 0; i < count; i++) {
		Point2i pos = get_screen_position(i);
		Size2i size = get_screen_size(i);
		if ((x >= pos.x && x < pos.x + size.width) && (y >= pos.y && y < pos.y + size.height))
			return i;
	}
	return 0;
}

void OS_EGL::set_current_screen(int p_screen) {
	if (p_screen == -1) {
		p_screen = get_current_screen();
	}

	// Check if screen is valid
	ERR_FAIL_INDEX(p_screen, get_screen_count());

	if (current_videomode.fullscreen) {
		Point2i position = get_screen_position(p_screen);
		Size2i size = get_screen_size(p_screen);

		XMoveResizeWindow(x11_display, x11_window, position.x, position.y, size.x, size.y);
	} else {
		if (p_screen != get_current_screen()) {
			Point2i position = get_screen_position(p_screen);
			XMoveWindow(x11_display, x11_window, position.x, position.y);
		}
	}
}

Point2 OS_EGL::get_screen_position(int p_screen) const {
	if (p_screen == -1) {
		p_screen = get_current_screen();
	}

	// Using Xinerama Extension
	int event_base, error_base;
	const Bool ext_okay = XineramaQueryExtension(x11_display, &event_base, &error_base);
	if (!ext_okay) {
		return Point2i(0, 0);
	}

	int count;
	XineramaScreenInfo *xsi = XineramaQueryScreens(x11_display, &count);

	// Check if screen is valid
	ERR_FAIL_INDEX_V(p_screen, count, Point2i(0, 0));

	Point2i position = Point2i(xsi[p_screen].x_org, xsi[p_screen].y_org);

	XFree(xsi);

	return position;
}

Size2 OS_EGL::get_screen_size(int p_screen) const {
	if (p_screen == -1) {
		p_screen = get_current_screen();
	}

	// Using Xinerama Extension
	int event_base, error_base;
	const Bool ext_okay = XineramaQueryExtension(x11_display, &event_base, &error_base);
	if (!ext_okay) return Size2i(0, 0);

	int count;
	XineramaScreenInfo *xsi = XineramaQueryScreens(x11_display, &count);

	// Check if screen is valid
	ERR_FAIL_INDEX_V(p_screen, count, Size2i(0, 0));

	Size2i size = Point2i(xsi[p_screen].width, xsi[p_screen].height);
	XFree(xsi);
	return size;
}

int OS_EGL::get_screen_dpi(int p_screen) const {
	if (p_screen == -1) {
		p_screen = get_current_screen();
	}

	//invalid screen?
	ERR_FAIL_INDEX_V(p_screen, get_screen_count(), 0);

	//Get physical monitor Dimensions through XRandR and calculate dpi
	Size2 sc = get_screen_size(p_screen);
	if (xrandr_ext_ok) {
		int count = 0;
		if (xrr_get_monitors) {
			xrr_monitor_info *monitors = xrr_get_monitors(x11_display, x11_window, true, &count);
			if (p_screen < count) {
				double xdpi = sc.width / (double)monitors[p_screen].mwidth * 25.4;
				double ydpi = sc.height / (double)monitors[p_screen].mheight * 25.4;
				xrr_free_monitors(monitors);
				return (xdpi + ydpi) / 2;
			}
			xrr_free_monitors(monitors);
		} else if (p_screen == 0) {
			XRRScreenSize *sizes = XRRSizes(x11_display, 0, &count);
			if (sizes) {
				double xdpi = sc.width / (double)sizes[0].mwidth * 25.4;
				double ydpi = sc.height / (double)sizes[0].mheight * 25.4;
				return (xdpi + ydpi) / 2;
			}
		}
	}

	int width_mm = DisplayWidthMM(x11_display, p_screen);
	int height_mm = DisplayHeightMM(x11_display, p_screen);
	double xdpi = (width_mm ? sc.width / (double)width_mm * 25.4 : 0);
	double ydpi = (height_mm ? sc.height / (double)height_mm * 25.4 : 0);
	if (xdpi || ydpi)
		return (xdpi + ydpi) / (xdpi && ydpi ? 2 : 1);

	//could not get dpi
	return 96;
}

Point2 OS_EGL::get_window_position() const {
	int x, y;
	Window child;
	XTranslateCoordinates(x11_display, x11_window, DefaultRootWindow(x11_display), 0, 0, &x, &y, &child);
	return Point2i(x, y);
}

void OS_EGL::set_window_position(const Point2 &p_position) {
	int x = 0;
	int y = 0;
	if (!get_borderless_window()) {
		//exclude window decorations
		XSync(x11_display, False);
		Atom prop = XInternAtom(x11_display, "_NET_FRAME_EXTENTS", True);
		if (prop != None) {
			Atom type;
			int format;
			unsigned long len;
			unsigned long remaining;
			unsigned char *data = NULL;
			if (XGetWindowProperty(x11_display, x11_window, prop, 0, 4, False, AnyPropertyType, &type, &format, &len, &remaining, &data) == Success) {
				if (format == 32 && len == 4) {
					long *extents = (long *)data;
					x = extents[0];
					y = extents[2];
				}
				XFree(data);
			}
		}
	}
	XMoveWindow(x11_display, x11_window, p_position.x - x, p_position.y - y);
}

Size2 OS_EGL::get_window_size() const {
	// Use current_videomode width and height instead of XGetWindowAttributes
	// since right after a XResizeWindow the attributes may not be updated yet
	return Size2i(current_videomode.width, current_videomode.height);
}

Size2 OS_EGL::get_real_window_size() const {
	XWindowAttributes xwa;
	XSync(x11_display, False);
	XGetWindowAttributes(x11_display, x11_window, &xwa);
	int w = xwa.width;
	int h = xwa.height;
	Atom prop = XInternAtom(x11_display, "_NET_FRAME_EXTENTS", True);
	if (prop != None) {
		Atom type;
		int format;
		unsigned long len;
		unsigned long remaining;
		unsigned char *data = NULL;
		if (XGetWindowProperty(x11_display, x11_window, prop, 0, 4, False, AnyPropertyType, &type, &format, &len, &remaining, &data) == Success) {
			if (format == 32 && len == 4) {
				long *extents = (long *)data;
				w += extents[0] + extents[1]; // left, right
				h += extents[2] + extents[3]; // top, bottom
			}
			XFree(data);
		}
	}
	return Size2(w, h);
}

Size2 OS_EGL::get_max_window_size() const {
	return max_size;
}

Size2 OS_EGL::get_min_window_size() const {
	return min_size;
}

void OS_EGL::set_min_window_size(const Size2 p_size) {

	if ((p_size != Size2()) && (max_size != Size2()) && ((p_size.x > max_size.x) || (p_size.y > max_size.y))) {
		ERR_PRINT("Minimum window size can't be larger than maximum window size!");
		return;
	}
	min_size = p_size;

	if (is_window_resizable()) {
		XSizeHints *xsh;
		xsh = XAllocSizeHints();
		xsh->flags = 0L;
		if (min_size != Size2()) {
			xsh->flags |= PMinSize;
			xsh->min_width = min_size.x;
			xsh->min_height = min_size.y;
		}
		if (max_size != Size2()) {
			xsh->flags |= PMaxSize;
			xsh->max_width = max_size.x;
			xsh->max_height = max_size.y;
		}
		XSetWMNormalHints(x11_display, x11_window, xsh);
		XFree(xsh);

		XFlush(x11_display);
	}
}

void OS_EGL::set_max_window_size(const Size2 p_size) {

	if ((p_size != Size2()) && ((p_size.x < min_size.x) || (p_size.y < min_size.y))) {
		ERR_PRINT("Maximum window size can't be smaller than minimum window size!");
		return;
	}
	max_size = p_size;

	if (is_window_resizable()) {
		XSizeHints *xsh;
		xsh = XAllocSizeHints();
		xsh->flags = 0L;
		if (min_size != Size2()) {
			xsh->flags |= PMinSize;
			xsh->min_width = min_size.x;
			xsh->min_height = min_size.y;
		}
		if (max_size != Size2()) {
			xsh->flags |= PMaxSize;
			xsh->max_width = max_size.x;
			xsh->max_height = max_size.y;
		}
		XSetWMNormalHints(x11_display, x11_window, xsh);
		XFree(xsh);

		XFlush(x11_display);
	}
}

void OS_EGL::set_window_size(const Size2 p_size) {

	if (current_videomode.width == p_size.width && current_videomode.height == p_size.height)
		return;

	XWindowAttributes xwa;
	XSync(x11_display, False);
	XGetWindowAttributes(x11_display, x11_window, &xwa);
	int old_w = xwa.width;
	int old_h = xwa.height;

	Size2 size = p_size;
	size.x = MAX(1, size.x);
	size.y = MAX(1, size.y);

	// If window resizable is disabled we need to update the attributes first
	XSizeHints *xsh;
	xsh = XAllocSizeHints();
	if (!is_window_resizable()) {
		xsh->flags = PMinSize | PMaxSize;
		xsh->min_width = size.x;
		xsh->max_width = size.x;
		xsh->min_height = size.y;
		xsh->max_height = size.y;
	} else {
		xsh->flags = 0L;
		if (min_size != Size2()) {
			xsh->flags |= PMinSize;
			xsh->min_width = min_size.x;
			xsh->min_height = min_size.y;
		}
		if (max_size != Size2()) {
			xsh->flags |= PMaxSize;
			xsh->max_width = max_size.x;
			xsh->max_height = max_size.y;
		}
	}
	XSetWMNormalHints(x11_display, x11_window, xsh);
	XFree(xsh);

	// Resize the window
	XResizeWindow(x11_display, x11_window, size.x, size.y);

	// Update our videomode width and height
	current_videomode.width = size.x;
	current_videomode.height = size.y;

	for (int timeout = 0; timeout < 50; ++timeout) {
		XSync(x11_display, False);
		XGetWindowAttributes(x11_display, x11_window, &xwa);

		if (old_w != xwa.width || old_h != xwa.height)
			break;

		usleep(10000);
	}
}

void OS_EGL::set_window_fullscreen(bool p_enabled) {

	if (current_videomode.fullscreen == p_enabled)
		return;

	if (layered_window)
		set_window_per_pixel_transparency_enabled(false);

	if (p_enabled && current_videomode.always_on_top) {
		// Fullscreen + Always-on-top requires a maximized window on some window managers (Metacity)
		set_window_maximized(true);
	}
	set_wm_fullscreen(p_enabled);
	if (!p_enabled && current_videomode.always_on_top) {
		// Restore
		set_window_maximized(false);
	}
	if (!p_enabled) {
		set_window_position(last_position_before_fs);
	} else {
		last_position_before_fs = get_window_position();
	}
	current_videomode.fullscreen = p_enabled;
}

bool OS_EGL::is_window_fullscreen() const {
	return current_videomode.fullscreen;
}

void OS_EGL::set_window_resizable(bool p_enabled) {

	XSizeHints *xsh;
	xsh = XAllocSizeHints();
	if (!p_enabled) {
		Size2 size = get_window_size();

		xsh->flags = PMinSize | PMaxSize;
		xsh->min_width = size.x;
		xsh->max_width = size.x;
		xsh->min_height = size.y;
		xsh->max_height = size.y;
	} else {
		xsh->flags = 0L;
		if (min_size != Size2()) {
			xsh->flags |= PMinSize;
			xsh->min_width = min_size.x;
			xsh->min_height = min_size.y;
		}
		if (max_size != Size2()) {
			xsh->flags |= PMaxSize;
			xsh->max_width = max_size.x;
			xsh->max_height = max_size.y;
		}
	}

	XSetWMNormalHints(x11_display, x11_window, xsh);
	XFree(xsh);

	current_videomode.resizable = p_enabled;

	XFlush(x11_display);
}

bool OS_EGL::is_window_resizable() const {
	return current_videomode.resizable;
}

void OS_EGL::set_window_minimized(bool p_enabled) {
	if (is_no_window_mode_enabled()) {
		return;
	}
	// Using ICCCM -- Inter-Client Communication Conventions Manual
	XEvent xev;
	Atom wm_change = XInternAtom(x11_display, "WM_CHANGE_STATE", False);

	memset(&xev, 0, sizeof(xev));
	xev.type = ClientMessage;
	xev.xclient.window = x11_window;
	xev.xclient.message_type = wm_change;
	xev.xclient.format = 32;
	xev.xclient.data.l[0] = p_enabled ? WM_IconicState : WM_NormalState;

	XSendEvent(x11_display, DefaultRootWindow(x11_display), False, SubstructureRedirectMask | SubstructureNotifyMask, &xev);

	Atom wm_state = XInternAtom(x11_display, "_NET_WM_STATE", False);
	Atom wm_hidden = XInternAtom(x11_display, "_NET_WM_STATE_HIDDEN", False);

	memset(&xev, 0, sizeof(xev));
	xev.type = ClientMessage;
	xev.xclient.window = x11_window;
	xev.xclient.message_type = wm_state;
	xev.xclient.format = 32;
	xev.xclient.data.l[0] = _NET_WM_STATE_ADD;
	xev.xclient.data.l[1] = wm_hidden;

	XSendEvent(x11_display, DefaultRootWindow(x11_display), False, SubstructureRedirectMask | SubstructureNotifyMask, &xev);
}

bool OS_EGL::is_window_minimized() const {
	// Using ICCCM -- Inter-Client Communication Conventions Manual
	Atom property = XInternAtom(x11_display, "WM_STATE", True);
	if (property == None) {
		return false;
	}
	Atom type;
	int format;
	unsigned long len;
	unsigned long remaining;
	unsigned char *data = NULL;
	bool retval = false;

	int result = XGetWindowProperty(
			x11_display,
			x11_window,
			property,
			0,
			32,
			False,
			AnyPropertyType,
			&type,
			&format,
			&len,
			&remaining,
			&data);

	if (result == Success) {
		long *state = (long *)data;
		if (state[0] == WM_IconicState) {
			retval = true;
		}
		XFree(data);
	}

	return retval;
}

void OS_EGL::set_window_maximized(bool p_enabled) {
	if (is_no_window_mode_enabled()) {
		return;
	}
	if (is_window_maximized() == p_enabled)
		return;

	// Using EWMH -- Extended Window Manager Hints
	XEvent xev;
	Atom wm_state = XInternAtom(x11_display, "_NET_WM_STATE", False);
	Atom wm_max_horz = XInternAtom(x11_display, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
	Atom wm_max_vert = XInternAtom(x11_display, "_NET_WM_STATE_MAXIMIZED_VERT", False);

	memset(&xev, 0, sizeof(xev));
	xev.type = ClientMessage;
	xev.xclient.window = x11_window;
	xev.xclient.message_type = wm_state;
	xev.xclient.format = 32;
	xev.xclient.data.l[0] = p_enabled ? _NET_WM_STATE_ADD : _NET_WM_STATE_REMOVE;
	xev.xclient.data.l[1] = wm_max_horz;
	xev.xclient.data.l[2] = wm_max_vert;

	XSendEvent(x11_display, DefaultRootWindow(x11_display), False, SubstructureRedirectMask | SubstructureNotifyMask, &xev);

	if (p_enabled && is_window_maximize_allowed()) {
		// Wait for effective resizing (so the GLX context is too).
		// Give up after 0.5s, it's not going to happen on this WM.
		// https://github.com/godotengine/godot/issues/19978
		for (int attempt = 0; !is_window_maximized() && attempt < 50; attempt++) {
			usleep(10000);
		}
	}

	maximized = p_enabled;
}

// Just a helper to reduce code duplication in `is_window_maximize_allowed`
// and `is_window_maximized`.
bool OS_EGL::window_maximize_check(const char *p_atom_name) const {
	Atom property = XInternAtom(x11_display, p_atom_name, False);
	Atom type;
	int format;
	unsigned long len;
	unsigned long remaining;
	unsigned char *data = NULL;
	bool retval = false;

	if (property == None) {
		return false;
	}

	int result = XGetWindowProperty(
			x11_display,
			x11_window,
			property,
			0,
			1024,
			False,
			XA_ATOM,
			&type,
			&format,
			&len,
			&remaining,
			&data);

	if (result == Success) {
		Atom *atoms = (Atom *)data;
		Atom wm_act_max_horz = XInternAtom(x11_display, "_NET_WM_ACTION_MAXIMIZE_HORZ", False);
		Atom wm_act_max_vert = XInternAtom(x11_display, "_NET_WM_ACTION_MAXIMIZE_VERT", False);
		bool found_wm_act_max_horz = false;
		bool found_wm_act_max_vert = false;

		for (uint64_t i = 0; i < len; i++) {
			if (atoms[i] == wm_act_max_horz)
				found_wm_act_max_horz = true;
			if (atoms[i] == wm_act_max_vert)
				found_wm_act_max_vert = true;

			if (found_wm_act_max_horz || found_wm_act_max_vert) {
				retval = true;
				break;
			}
		}

		XFree(data);
	}

	return retval;
}

bool OS_EGL::is_window_maximize_allowed() const {
	return window_maximize_check("_NET_WM_ALLOWED_ACTIONS");
}

bool OS_EGL::is_window_maximized() const {
	// Using EWMH -- Extended Window Manager Hints
	return window_maximize_check("_NET_WM_STATE");
}

void OS_EGL::set_window_always_on_top(bool p_enabled) {
	if (is_window_always_on_top() == p_enabled)
		return;

	if (p_enabled && current_videomode.fullscreen) {
		// Fullscreen + Always-on-top requires a maximized window on some window managers (Metacity)
		set_window_maximized(true);
	}
	set_wm_above(p_enabled);
	if (!p_enabled && !current_videomode.fullscreen) {
		// Restore
		set_window_maximized(false);
	}

	current_videomode.always_on_top = p_enabled;
}

bool OS_EGL::is_window_always_on_top() const {
	return current_videomode.always_on_top;
}

bool OS_EGL::is_window_focused() const {
	return window_focused;
}

void OS_EGL::set_borderless_window(bool p_borderless) {

	if (get_borderless_window() == p_borderless)
		return;

	current_videomode.borderless_window = p_borderless;

	Hints hints;
	Atom property;
	hints.flags = 2;
	hints.decorations = current_videomode.borderless_window ? 0 : 1;
	property = XInternAtom(x11_display, "_MOTIF_WM_HINTS", True);
	if (property != None) {
		XChangeProperty(x11_display, x11_window, property, property, 32, PropModeReplace, (unsigned char *)&hints, 5);
	}

	// Preserve window size
	set_window_size(Size2(current_videomode.width, current_videomode.height));
}

bool OS_EGL::get_borderless_window() {

	bool borderless = current_videomode.borderless_window;
	Atom prop = XInternAtom(x11_display, "_MOTIF_WM_HINTS", True);
	if (prop != None) {

		Atom type;
		int format;
		unsigned long len;
		unsigned long remaining;
		unsigned char *data = NULL;
		if (XGetWindowProperty(x11_display, x11_window, prop, 0, sizeof(Hints), False, AnyPropertyType, &type, &format, &len, &remaining, &data) == Success) {
			if (data && (format == 32) && (len >= 5)) {
				borderless = !((Hints *)data)->decorations;
			}
			XFree(data);
		}
	}
	return borderless;
}

void OS_EGL::request_attention() {
	// Using EWMH -- Extended Window Manager Hints
	//
	// Sets the _NET_WM_STATE_DEMANDS_ATTENTION atom for WM_STATE
	// Will be unset by the window manager after user react on the request for attention

	XEvent xev;
	Atom wm_state = XInternAtom(x11_display, "_NET_WM_STATE", False);
	Atom wm_attention = XInternAtom(x11_display, "_NET_WM_STATE_DEMANDS_ATTENTION", False);

	memset(&xev, 0, sizeof(xev));
	xev.type = ClientMessage;
	xev.xclient.window = x11_window;
	xev.xclient.message_type = wm_state;
	xev.xclient.format = 32;
	xev.xclient.data.l[0] = _NET_WM_STATE_ADD;
	xev.xclient.data.l[1] = wm_attention;

	XSendEvent(x11_display, DefaultRootWindow(x11_display), False, SubstructureRedirectMask | SubstructureNotifyMask, &xev);
	XFlush(x11_display);
}

void *OS_EGL::get_native_handle(int p_handle_type) {
	switch (p_handle_type) {
		case APPLICATION_HANDLE: return NULL; // Do we have a value to return here?
		case DISPLAY_HANDLE: return (void *)x11_display;
		case WINDOW_HANDLE: return (void *)x11_window;
		case WINDOW_VIEW: return NULL; // Do we have a value to return here?
		case OPENGL_CONTEXT: return context_gl->get_glx_context();
		default: return NULL;
	}
}

void OS_EGL::get_key_modifier_state(unsigned int p_x11_state, Ref<InputEventWithModifiers> state) {

	state->set_shift((p_x11_state & ShiftMask));
	state->set_control((p_x11_state & ControlMask));
	state->set_alt((p_x11_state & Mod1Mask /*|| p_x11_state&Mod5Mask*/)); //altgr should not count as alt
	state->set_metakey((p_x11_state & Mod4Mask));
}

unsigned int OS_EGL::get_mouse_button_state(unsigned int p_x11_button, int p_x11_type) {

	unsigned int mask = 1 << (p_x11_button - 1);

	if (p_x11_type == ButtonPress) {
		last_button_state |= mask;
	} else {
		last_button_state &= ~mask;
	}

	return last_button_state;
}

Atom OS_EGL::_process_selection_request_target(Atom p_target, Window p_requestor, Atom p_property) const {
	if (p_target == XInternAtom(x11_display, "TARGETS", 0)) {
		// Request to list all supported targets.
		Atom data[9];
		data[0] = XInternAtom(x11_display, "TARGETS", 0);
		data[1] = XInternAtom(x11_display, "SAVE_TARGETS", 0);
		data[2] = XInternAtom(x11_display, "MULTIPLE", 0);
		data[3] = XInternAtom(x11_display, "UTF8_STRING", 0);
		data[4] = XInternAtom(x11_display, "COMPOUND_TEXT", 0);
		data[5] = XInternAtom(x11_display, "TEXT", 0);
		data[6] = XA_STRING;
		data[7] = XInternAtom(x11_display, "text/plain;charset=utf-8", 0);
		data[8] = XInternAtom(x11_display, "text/plain", 0);

		XChangeProperty(x11_display,
				p_requestor,
				p_property,
				XA_ATOM,
				32,
				PropModeReplace,
				(unsigned char *)&data,
				sizeof(data) / sizeof(data[0]));

		return p_property;
	} else if (p_target == XInternAtom(x11_display, "SAVE_TARGETS", 0)) {
		// Request to check if SAVE_TARGETS is supported, nothing special to do.
		XChangeProperty(x11_display,
				p_requestor,
				p_property,
				XInternAtom(x11_display, "NULL", False),
				32,
				PropModeReplace,
				NULL,
				0);
		return p_property;
	} else if (p_target == XInternAtom(x11_display, "UTF8_STRING", 0) ||
			   p_target == XInternAtom(x11_display, "COMPOUND_TEXT", 0) ||
			   p_target == XInternAtom(x11_display, "TEXT", 0) ||
			   p_target == XA_STRING ||
			   p_target == XInternAtom(x11_display, "text/plain;charset=utf-8", 0) ||
			   p_target == XInternAtom(x11_display, "text/plain", 0)) {
		// Directly using internal clipboard because we know our window
		// is the owner during a selection request.
		CharString clip = OS::get_clipboard().utf8();
		XChangeProperty(x11_display,
				p_requestor,
				p_property,
				p_target,
				8,
				PropModeReplace,
				(unsigned char *)clip.get_data(),
				clip.length());
		return p_property;
	} else {
		char *target_name = XGetAtomName(x11_display, p_target);
		printf("Target '%s' not supported.\n", target_name);
		if (target_name) {
			XFree(target_name);
		}
		return None;
	}
}

void OS_EGL::_handle_selection_request_event(XSelectionRequestEvent *p_event) const {
	XEvent respond;
	if (p_event->target == XInternAtom(x11_display, "MULTIPLE", 0)) {
		// Request for multiple target conversions at once.
		Atom atom_pair = XInternAtom(x11_display, "ATOM_PAIR", False);
		respond.xselection.property = None;

		Atom type;
		int format;
		unsigned long len;
		unsigned long remaining;
		unsigned char *data = NULL;
		if (XGetWindowProperty(x11_display, p_event->requestor, p_event->property, 0, LONG_MAX, False, atom_pair, &type, &format, &len, &remaining, &data) == Success) {
			if ((len >= 2) && data) {
				Atom *targets = (Atom *)data;
				for (uint64_t i = 0; i < len; i += 2) {
					Atom target = targets[i];
					Atom &property = targets[i + 1];
					property = _process_selection_request_target(target, p_event->requestor, property);
				}

				XChangeProperty(x11_display,
						p_event->requestor,
						p_event->property,
						atom_pair,
						32,
						PropModeReplace,
						(unsigned char *)targets,
						len);

				respond.xselection.property = p_event->property;
			}
			XFree(data);
		}
	} else {
		// Request for target conversion.
		respond.xselection.property = _process_selection_request_target(p_event->target, p_event->requestor, p_event->property);
	}

	respond.xselection.type = SelectionNotify;
	respond.xselection.display = p_event->display;
	respond.xselection.requestor = p_event->requestor;
	respond.xselection.selection = p_event->selection;
	respond.xselection.target = p_event->target;
	respond.xselection.time = p_event->time;

	XSendEvent(x11_display, p_event->requestor, True, NoEventMask, &respond);
	XFlush(x11_display);
}

void OS_EGL::_window_changed(XEvent *event) {

	set_ime_position(Point2(0, 1));

	if ((event->xconfigure.width == current_videomode.width) &&
			(event->xconfigure.height == current_videomode.height))
		return;

	current_videomode.width = event->xconfigure.width;
	current_videomode.height = event->xconfigure.height;
}


Bool OS_EGL::_predicate_all_events(Display *display, XEvent *event, XPointer arg) {
	// Just accept all events.
	return True;
}

MainLoop *OS_EGL::get_main_loop() const {

	return main_loop;
}

void OS_EGL::delete_main_loop() {
	if (main_loop)
		memdelete(main_loop);
	main_loop = NULL;
}

void OS_EGL::set_main_loop(MainLoop *p_main_loop) {
	main_loop = p_main_loop;
	input->set_main_loop(p_main_loop);
}

bool OS_EGL::can_draw() const {
	return !minimized;
};

void OS_EGL::set_clipboard(const String &p_text) {
};

String OS_EGL::get_clipboard() const {
	return "";
}

String OS_EGL::get_name() const {

	return "X11";
}

Error OS_EGL::shell_open(String p_uri) {
	Error ok;
	int err_code;
	List<String> args;
	args.push_back(p_uri);

	// Agnostic
	ok = execute("xdg-open", args, true, NULL, NULL, &err_code);
	if (ok == OK && !err_code) {
		return OK;
	} else if (err_code == 2) {
		return ERR_FILE_NOT_FOUND;
	}
	// GNOME
	args.push_front("open"); // The command is `gio open`, so we need to add it to args
	ok = execute("gio", args, true, NULL, NULL, &err_code);
	if (ok == OK && !err_code) {
		return OK;
	} else if (err_code == 2) {
		return ERR_FILE_NOT_FOUND;
	}
	args.pop_front();
	ok = execute("gvfs-open", args, true, NULL, NULL, &err_code);
	if (ok == OK && !err_code) {
		return OK;
	} else if (err_code == 2) {
		return ERR_FILE_NOT_FOUND;
	}
	// KDE
	ok = execute("kde-open5", args, true, NULL, NULL, &err_code);
	if (ok == OK && !err_code) {
		return OK;
	}
	ok = execute("kde-open", args, true, NULL, NULL, &err_code);
	return !err_code ? ok : FAILED;
}

bool OS_EGL::_check_internal_feature_support(const String &p_feature) {

	return p_feature == "pc";
}

String OS_EGL::get_config_path() const {

	if (has_environment("XDG_CONFIG_HOME")) {
		return get_environment("XDG_CONFIG_HOME");
	} else if (has_environment("HOME")) {
		return get_environment("HOME").plus_file(".config");
	} else {
		return ".";
	}
}

String OS_EGL::get_data_path() const {

	if (has_environment("XDG_DATA_HOME")) {
		return get_environment("XDG_DATA_HOME");
	} else if (has_environment("HOME")) {
		return get_environment("HOME").plus_file(".local/share");
	} else {
		return get_config_path();
	}
}

String OS_EGL::get_cache_path() const {

	if (has_environment("XDG_CACHE_HOME")) {
		return get_environment("XDG_CACHE_HOME");
	} else if (has_environment("HOME")) {
		return get_environment("HOME").plus_file(".cache");
	} else {
		return get_config_path();
	}
}

String OS_EGL::get_system_dir(SystemDir p_dir, bool p_shared_storage) const {
	String xdgparam;

	switch (p_dir) {
		case SYSTEM_DIR_DESKTOP: {

			xdgparam = "DESKTOP";
		} break;
		case SYSTEM_DIR_DCIM: {

			xdgparam = "PICTURES";

		} break;
		case SYSTEM_DIR_DOCUMENTS: {

			xdgparam = "DOCUMENTS";

		} break;
		case SYSTEM_DIR_DOWNLOADS: {

			xdgparam = "DOWNLOAD";

		} break;
		case SYSTEM_DIR_MOVIES: {

			xdgparam = "VIDEOS";

		} break;
		case SYSTEM_DIR_MUSIC: {

			xdgparam = "MUSIC";

		} break;
		case SYSTEM_DIR_PICTURES: {

			xdgparam = "PICTURES";

		} break;
		case SYSTEM_DIR_RINGTONES: {

			xdgparam = "MUSIC";

		} break;
	}

	String pipe;
	List<String> arg;
	arg.push_back(xdgparam);
	Error err = const_cast<OS_EGL *>(this)->execute("xdg-user-dir", arg, true, NULL, &pipe);
	if (err != OK)
		return ".";
	return pipe.strip_edges();
}

void OS_EGL::move_window_to_foreground() {

	XEvent xev;
	Atom net_active_window = XInternAtom(x11_display, "_NET_ACTIVE_WINDOW", False);

	memset(&xev, 0, sizeof(xev));
	xev.type = ClientMessage;
	xev.xclient.window = x11_window;
	xev.xclient.message_type = net_active_window;
	xev.xclient.format = 32;
	xev.xclient.data.l[0] = 1;
	xev.xclient.data.l[1] = CurrentTime;

	XSendEvent(x11_display, DefaultRootWindow(x11_display), False, SubstructureRedirectMask | SubstructureNotifyMask, &xev);
	XFlush(x11_display);
}

void OS_EGL::set_cursor_shape(CursorShape p_shape) {

	ERR_FAIL_INDEX(p_shape, CURSOR_MAX);

	if (p_shape == current_cursor) {
		return;
	}

	if (mouse_mode == MOUSE_MODE_VISIBLE || mouse_mode == MOUSE_MODE_CONFINED) {
		if (cursors[p_shape] != None) {
			XDefineCursor(x11_display, x11_window, cursors[p_shape]);
		} else if (cursors[CURSOR_ARROW] != None) {
			XDefineCursor(x11_display, x11_window, cursors[CURSOR_ARROW]);
		}
	}

	current_cursor = p_shape;
}

OS::CursorShape OS_EGL::get_cursor_shape() const {

	return current_cursor;
}

void OS_EGL::set_custom_mouse_cursor(const RES &p_cursor, CursorShape p_shape, const Vector2 &p_hotspot) {

	if (p_cursor.is_valid()) {

		Map<CursorShape, Vector<Variant> >::Element *cursor_c = cursors_cache.find(p_shape);

		if (cursor_c) {
			if (cursor_c->get()[0] == p_cursor && cursor_c->get()[1] == p_hotspot) {
				set_cursor_shape(p_shape);
				return;
			}

			cursors_cache.erase(p_shape);
		}

		Ref<Texture> texture = p_cursor;
		Ref<AtlasTexture> atlas_texture = p_cursor;
		Ref<Image> image;
		Size2 texture_size;
		Rect2 atlas_rect;

		if (texture.is_valid()) {
			image = texture->get_data();
		}

		if (!image.is_valid() && atlas_texture.is_valid()) {
			texture = atlas_texture->get_atlas();

			atlas_rect.size.width = texture->get_width();
			atlas_rect.size.height = texture->get_height();
			atlas_rect.position.x = atlas_texture->get_region().position.x;
			atlas_rect.position.y = atlas_texture->get_region().position.y;

			texture_size.width = atlas_texture->get_region().size.x;
			texture_size.height = atlas_texture->get_region().size.y;
		} else if (image.is_valid()) {
			texture_size.width = texture->get_width();
			texture_size.height = texture->get_height();
		}

		ERR_FAIL_COND(!texture.is_valid());
		ERR_FAIL_COND(p_hotspot.x < 0 || p_hotspot.y < 0);
		ERR_FAIL_COND(texture_size.width > 256 || texture_size.height > 256);
		ERR_FAIL_COND(p_hotspot.x > texture_size.width || p_hotspot.y > texture_size.height);

		image = texture->get_data();

		ERR_FAIL_COND(!image.is_valid());

		// Create the cursor structure
		XcursorImage *cursor_image = XcursorImageCreate(texture_size.width, texture_size.height);
		XcursorUInt image_size = texture_size.width * texture_size.height;
		XcursorDim size = sizeof(XcursorPixel) * image_size;

		cursor_image->version = 1;
		cursor_image->size = size;
		cursor_image->xhot = p_hotspot.x;
		cursor_image->yhot = p_hotspot.y;

		// allocate memory to contain the whole file
		cursor_image->pixels = (XcursorPixel *)memalloc(size);

		image->lock();

		for (XcursorPixel index = 0; index < image_size; index++) {
			int row_index = floor(index / texture_size.width) + atlas_rect.position.y;
			int column_index = (index % int(texture_size.width)) + atlas_rect.position.x;

			if (atlas_texture.is_valid()) {
				column_index = MIN(column_index, atlas_rect.size.width - 1);
				row_index = MIN(row_index, atlas_rect.size.height - 1);
			}

			*(cursor_image->pixels + index) = image->get_pixel(column_index, row_index).to_argb32();
		}

		image->unlock();

		ERR_FAIL_COND(cursor_image->pixels == NULL);

		// Save it for a further usage
		cursors[p_shape] = XcursorImageLoadCursor(x11_display, cursor_image);

		Vector<Variant> params;
		params.push_back(p_cursor);
		params.push_back(p_hotspot);
		cursors_cache.insert(p_shape, params);

		if (p_shape == current_cursor) {
			if (mouse_mode == MOUSE_MODE_VISIBLE || mouse_mode == MOUSE_MODE_CONFINED) {
				XDefineCursor(x11_display, x11_window, cursors[p_shape]);
			}
		}

		memfree(cursor_image->pixels);
		XcursorImageDestroy(cursor_image);
	} else {
		// Reset to default system cursor
		if (img[p_shape]) {
			cursors[p_shape] = XcursorImageLoadCursor(x11_display, img[p_shape]);
		}

		CursorShape c = current_cursor;
		current_cursor = CURSOR_MAX;
		set_cursor_shape(c);

		cursors_cache.erase(p_shape);
	}
}

void OS_EGL::release_rendering_thread() {

#if defined(OPENGL_ENABLED)
	context_gl->release_current();
#endif
}

void OS_EGL::make_rendering_thread() {

#if defined(OPENGL_ENABLED)
	context_gl->make_current();
#endif
}

void OS_EGL::swap_buffers() {

#if defined(OPENGL_ENABLED)
	context_gl->swap_buffers();
#endif
}

void OS_EGL::alert(const String &p_alert, const String &p_title) {

	if (is_no_window_mode_enabled()) {
		print_line("ALERT: " + p_title + ": " + p_alert);
		return;
	}

	const char *message_programs[] = { "zenity", "kdialog", "Xdialog", "xmessage" };

	String path = get_environment("PATH");
	Vector<String> path_elems = path.split(":", false);
	String program;

	for (int i = 0; i < path_elems.size(); i++) {
		for (uint64_t k = 0; k < sizeof(message_programs) / sizeof(char *); k++) {
			String tested_path = path_elems[i].plus_file(message_programs[k]);

			if (FileAccess::exists(tested_path)) {
				program = tested_path;
				break;
			}
		}

		if (program.length())
			break;
	}

	List<String> args;

	if (program.ends_with("zenity")) {
		args.push_back("--error");
		args.push_back("--width");
		args.push_back("500");
		args.push_back("--title");
		args.push_back(p_title);
		args.push_back("--text");
		args.push_back(p_alert);
	}

	if (program.ends_with("kdialog")) {
		args.push_back("--error");
		args.push_back(p_alert);
		args.push_back("--title");
		args.push_back(p_title);
	}

	if (program.ends_with("Xdialog")) {
		args.push_back("--title");
		args.push_back(p_title);
		args.push_back("--msgbox");
		args.push_back(p_alert);
		args.push_back("0");
		args.push_back("0");
	}

	if (program.ends_with("xmessage")) {
		args.push_back("-center");
		args.push_back("-title");
		args.push_back(p_title);
		args.push_back(p_alert);
	}

	if (program.length()) {
		execute(program, args, true);
	} else {
		print_line(p_alert);
	}
}

bool g_set_icon_error = false;
int set_icon_errorhandler(Display *dpy, XErrorEvent *ev) {
	g_set_icon_error = true;
	return 0;
}

void OS_EGL::set_icon(const Ref<Image> &p_icon) {
	int (*oldHandler)(Display *, XErrorEvent *) = XSetErrorHandler(&set_icon_errorhandler);

	Atom net_wm_icon = XInternAtom(x11_display, "_NET_WM_ICON", False);

	if (p_icon.is_valid()) {
		Ref<Image> img = p_icon->duplicate();
		img->convert(Image::FORMAT_RGBA8);

		while (true) {
			int w = img->get_width();
			int h = img->get_height();

			if (g_set_icon_error) {
				g_set_icon_error = false;

				WARN_PRINT("Icon too large, attempting to resize icon.");

				int new_width, new_height;
				if (w > h) {
					new_width = w / 2;
					new_height = h * new_width / w;
				} else {
					new_height = h / 2;
					new_width = w * new_height / h;
				}

				w = new_width;
				h = new_height;

				if (!w || !h) {
					WARN_PRINT("Unable to set icon.");
					break;
				}

				img->resize(w, h, Image::INTERPOLATE_CUBIC);
			}

			// We're using long to have wordsize (32Bit build -> 32 Bits, 64 Bit build -> 64 Bits
			Vector<long> pd;

			pd.resize(2 + w * h);

			pd.write[0] = w;
			pd.write[1] = h;

			PoolVector<uint8_t>::Read r = img->get_data().read();

			long *wr = &pd.write[2];
			uint8_t const *pr = r.ptr();

			for (int i = 0; i < w * h; i++) {
				long v = 0;
				//    A             R             G            B
				v |= pr[3] << 24 | pr[0] << 16 | pr[1] << 8 | pr[2];
				*wr++ = v;
				pr += 4;
			}

			if (net_wm_icon != None) {
				XChangeProperty(x11_display, x11_window, net_wm_icon, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)pd.ptr(), pd.size());
			}

			if (!g_set_icon_error)
				break;
		}
	} else {
		XDeleteProperty(x11_display, x11_window, net_wm_icon);
	}

	XFlush(x11_display);
	XSetErrorHandler(oldHandler);
}

void OS_EGL::force_process_input() {
}

void OS_EGL::run() {

	force_quit = false;

	if (!main_loop)
		return;

	main_loop->init();

	//uint64_t last_ticks=get_ticks_usec();

	//int frames=0;
	//uint64_t frame=0;

	while (!force_quit) {
		if (Main::iteration())
			break;
	};

	main_loop->finish();
}

bool OS_EGL::is_joy_known(int p_device) {
	return input->is_joy_mapped(p_device);
}

String OS_EGL::get_joy_guid(int p_device) const {
	return input->get_joy_guid_remapped(p_device);
}

void OS_EGL::_set_use_vsync(bool p_enable) {
#if defined(OPENGL_ENABLED)
	if (context_gl)
		context_gl->set_use_vsync(p_enable);
#endif
}
/*
bool OS_EGL::is_vsync_enabled() const {

	if (context_gl)
		return context_gl->is_using_vsync();

	return true;
}
*/
void OS_EGL::set_context(int p_context) {

	XClassHint *classHint = XAllocClassHint();

	if (classHint) {

		CharString name_str;
		switch (p_context) {
			case CONTEXT_EDITOR:
				name_str = "Godot_Editor";
				break;
			case CONTEXT_PROJECTMAN:
				name_str = "Godot_ProjectList";
				break;
			case CONTEXT_ENGINE:
				name_str = "Godot_Engine";
				break;
		}

		CharString class_str;
		if (p_context == CONTEXT_ENGINE) {
			String config_name = GLOBAL_GET("application/config/name");
			if (config_name.length() == 0) {
				class_str = "Godot_Engine";
			} else {
				class_str = config_name.utf8();
			}
		} else {
			class_str = "Godot";
		}

		classHint->res_class = class_str.ptrw();
		classHint->res_name = name_str.ptrw();

		XSetClassHint(x11_display, x11_window, classHint);
		XFree(classHint);
	}
}

OS::PowerState OS_EGL::get_power_state() {
	return POWERSTATE_NO_BATTERY;
}

int OS_EGL::get_power_seconds_left() {
	return -1;
}

int OS_EGL::get_power_percent_left() {
	return -1;
}

void OS_EGL::disable_crash_handler() {
	crash_handler.disable();
}

bool OS_EGL::is_disable_crash_handler() const {
	return crash_handler.is_disabled();
}

Error OS_EGL::move_to_trash(const String &p_path) {
	DirAccess *dir_access = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	Error err = dir_access->remove(p_path);
	memdelete(dir_access);
	return err;
}

OS::LatinKeyboardVariant OS_EGL::get_latin_keyboard_variant() const {
	return LATIN_KEYBOARD_QWERTY;
}

int OS_EGL::keyboard_get_layout_count() const {
	return 0;
}

int OS_EGL::keyboard_get_current_layout() const {
	return 0;
}

void OS_EGL::keyboard_set_current_layout(int p_index) {
}

String OS_EGL::keyboard_get_layout_language(int p_index) const {
	return "";
}

String OS_EGL::keyboard_get_layout_name(int p_index) const {
	return "";
}


OS_EGL::OS_EGL() {
	layered_window = false;
	minimized = false;
	window_focused = true;
	mouse_mode = MOUSE_MODE_VISIBLE;
	last_position_before_fs = Vector2();
}
