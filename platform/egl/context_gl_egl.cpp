/*************************************************************************/
/*  context_gl_egl.cpp                                                   */
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

#include "context_gl_egl.h"

#ifdef EGL_ENABLED
#include <unistd.h>

void ContextGL_EGL::release_current() {
	eglMakeCurrent(eglDpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

void ContextGL_EGL::make_current() {
	eglMakeCurrent(eglDpy, eglSurf, eglSurf, eglCtx);
}

void ContextGL_EGL::swap_buffers() {
	eglSwapBuffers(eglDpy, eglSurf);
}

void ContextGL_EGL::set_buffer_size(const int width, const int height) {
	eglMakeCurrent(eglDpy, EGL_NO_SURFACE, EGL_NO_SURFACE, eglCtx);
	eglDestroySurface(eglDpy, eglSurf);

	static const EGLint pbufferAttribs[] = {
		EGL_WIDTH, width,
		EGL_HEIGHT, height,
		EGL_NONE,
	};
	eglSurf = eglCreatePbufferSurface(eglDpy, eglCfg, pbufferAttribs);
	eglMakeCurrent(eglDpy, eglSurf, eglSurf, eglCtx);
}

Error ContextGL_EGL::initialize(const int width, const int height) {
	EGLint numConfigs;

	static const EGLint visual_attribs_layers[] = {
		EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT | EGL_OPENGL_ES2_BIT,
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
		EGL_DEPTH_SIZE, 24,
		EGL_NONE
	};

	static const EGLint visual_attribs_simple[] = {
		EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT | EGL_OPENGL_ES2_BIT,
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_DEPTH_SIZE, 24,
		EGL_NONE
	};

	static const EGLint pbufferAttribs[] = {
		EGL_WIDTH, width,
		EGL_HEIGHT, height,
		EGL_NONE,
	};

	static const EGLint contextAttribs[] = {
		EGL_CONTEXT_MAJOR_VERSION, (context_type == ContextType::GLES_3_0_COMPATIBLE) ? 3 : 2,
		EGL_CONTEXT_MINOR_VERSION, (context_type == ContextType::GLES_3_0_COMPATIBLE) ? 3 : 0,
		EGL_NONE,
	};

   	PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT = (PFNEGLGETPLATFORMDISPLAYEXTPROC) eglGetProcAddress("eglGetPlatformDisplayEXT");
	eglDpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT, EGL_DEFAULT_DISPLAY, NULL);
	eglInitialize(eglDpy, &egl_major, &egl_minor);

	if (OS::get_singleton()->is_layered_allowed()) {
		eglChooseConfig(eglDpy, visual_attribs_layers, &eglCfg, 1, &numConfigs);
	} else {
		eglChooseConfig(eglDpy, visual_attribs_simple, &eglCfg, 1, &numConfigs);
	}

	eglSurf = eglCreatePbufferSurface(eglDpy, eglCfg, pbufferAttribs);
	eglBindAPI(EGL_OPENGL_API);

	eglCtx = eglCreateContext(eglDpy, eglCfg, EGL_NO_CONTEXT, contextAttribs);

	eglMakeCurrent(eglDpy, eglSurf, eglSurf, eglCtx);

	set_use_vsync(false);
	return OK;
}

int ContextGL_EGL::get_window_width() {
	EGLint size = 0;
	eglQuerySurface(eglDpy, eglSurf, EGL_WIDTH, &size);
	return size;
}

int ContextGL_EGL::get_window_height() {
	EGLint size = 0;
	eglQuerySurface(eglDpy, eglSurf, EGL_HEIGHT, &size);
	return size;
}

void *ContextGL_EGL::get_glx_context() {
	return eglCtx;
}

void ContextGL_EGL::set_use_vsync(bool p_use) {
	eglSwapInterval(eglDpy, p_use);
	use_vsync = p_use;
}
bool ContextGL_EGL::is_using_vsync() const {
	return use_vsync;
}

ContextGL_EGL::ContextGL_EGL(const OS::VideoMode &p_default_video_mode, ContextType p_context_type) {
	context_type = p_context_type;
	double_buffer = false;
	direct_render = false;
	egl_major = 0;
	egl_minor = 0;
}

ContextGL_EGL::~ContextGL_EGL() {
	release_current();
	eglTerminate(eglDpy);
}

#endif
