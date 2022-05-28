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
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "thirdparty/glad/glad/glad.h"

void ContextGL_EGL::release_current() {
	eglMakeCurrent(eglDpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

void ContextGL_EGL::make_current() {
	eglMakeCurrent(eglDpy, eglSurf, eglSurf, eglCtx);
}

#define PIXEL_COPY_STRATEGY 2
#define PIXEL_COPY_DEPTH 1
#define PIXEL_COPY_COLOR 1

void ContextGL_EGL::swap_buffers() {
#if PIXEL_COPY_STRATEGY == 0
	eglSwapBuffers(eglDpy, eglSurf);
#elif PIXEL_COPY_STRATEGY == 1
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	#if PIXEL_COPY_DEPTH
		glReadPixels(0, 0, 128, 128, GL_DEPTH_COMPONENT, GL_FLOAT, copy_buffer);
	#endif
	#if PIXEL_COPY_COLOR
		glReadPixels(0, 0, 128, 128, GL_RGB, GL_UNSIGNED_BYTE, (char*)copy_buffer+(128 * 128 * 4));
	#endif
	eglSwapBuffers(eglDpy, eglSurf);
#elif PIXEL_COPY_STRATEGY == 2
	if (pbo_id == 0) {
		glGenBuffers(1, &pbo_id);
		glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_id);
		glBufferData(GL_PIXEL_PACK_BUFFER, 128 * 128 * (4 + 3), NULL, GL_STREAM_READ);
	#if PIXEL_COPY_DEPTH
		glReadPixels(0, 0, 128, 128, GL_DEPTH_COMPONENT, GL_FLOAT, 0);
	#endif
	#if PIXEL_COPY_COLOR
		glReadPixels(0, 0, 128, 128, GL_RGB, GL_UNSIGNED_BYTE, (void*)(128 * 128 * 4));
	#endif
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	}

	glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_id);
	void * ptr = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
	memcpy(copy_buffer, ptr, (128 * 128 * (4 + 3)));
	glUnmapBuffer(GL_PIXEL_PACK_BUFFER);

	eglSwapBuffers(eglDpy, eglSurf);

	glBufferData(GL_PIXEL_PACK_BUFFER, 128 * 128 * (4 + 3), NULL, GL_STREAM_READ);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	#if PIXEL_COPY_DEPTH
		glReadPixels(0, 0, 128, 128, GL_DEPTH_COMPONENT, GL_FLOAT, 0);
	#endif
	#if PIXEL_COPY_COLOR
		glReadPixels(0, 0, 128, 128, GL_RGB, GL_UNSIGNED_BYTE, (void*)(128 * 128 * 4));
	#endif
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
#endif
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

#if PIXEL_COPY_STRATEGY == 0
#elif PIXEL_COPY_STRATEGY == 1
	int buffer_size = 128 * 128 * (4 + 3);
	copy_buffer = malloc(buffer_size);
#elif PIXEL_COPY_STRATEGY == 2
	int buffer_size = 128 * 128 * (4 + 3);
	int buffer_fd = open("/tmp/godot.data", O_RDWR | O_CREAT | O_TRUNC, 0666);
	ftruncate(buffer_fd, buffer_size);
	copy_buffer = mmap(NULL, buffer_size, PROT_READ | PROT_WRITE, MAP_SHARED, buffer_fd, 0);
#endif

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
	pbo_id = 0;
}

ContextGL_EGL::~ContextGL_EGL() {
	release_current();
	eglTerminate(eglDpy);
}

#endif
