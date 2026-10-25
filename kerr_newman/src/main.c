/*
 * TON 618 as a Kerr–Newman (rotating, charged) black hole.
 * Native Wayland (xdg-shell) + EGL + desktop OpenGL 3.3 core. No X11/XWayland.
 *
 * Controls:
 *   left drag      orbit camera
 *   right drag     move window
 *   scroll         zoom
 *   1 / 2          spin a  -/+   (negative = retrograde disk)
 *   3 / 4          charge Q -/+
 *   J              toggle jets
 *   E              toggle ergosphere glow
 *   F / F11        toggle fullscreen
 *   Space          pause time
 *   R              reload shaders from disk
 *   Esc / Q        quit
 */
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <linux/input-event-codes.h>

#include <wayland-client.h>
#include <wayland-cursor.h>
#include <wayland-egl.h>

#include <EGL/egl.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#ifndef SHADER_DIR
#define SHADER_DIR "shaders"
#endif

#define DEFAULT_W 1280
#define DEFAULT_H 720

/* keep a^2 + Q^2 < 1 so there is always an event horizon (no naked singularity) */
#define MAX_SPIN_CHARGE2 0.998

struct app {
	/* wayland */
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_seat *seat;
	struct wl_pointer *pointer;
	struct wl_keyboard *keyboard;
	struct wl_shm *shm;
	struct xdg_wm_base *wm_base;
	struct zxdg_decoration_manager_v1 *deco_manager;

	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *toplevel;
	struct zxdg_toplevel_decoration_v1 *decoration;

	struct wl_cursor_theme *cursor_theme;
	struct wl_surface *cursor_surface;

	/* egl / gl */
	EGLDisplay egl_display;
	EGLContext egl_context;
	EGLSurface egl_surface;
	struct wl_egl_window *egl_window;

	GLuint program, vao;
	GLint u_resolution, u_time, u_cam, u_bh, u_flags;

	/* low-res offscreen target for render scaling */
	GLuint fbo, fbo_tex;
	int fbo_w, fbo_h;
	float render_scale;

	/* state */
	int width, height;
	int pending_w, pending_h;
	bool configured, running, fullscreen, paused;

	bool dragging;
	double last_x, last_y, ptr_x, ptr_y;
	float yaw, pitch, dist;

	double sim_time;

	/* black hole, in units of M */
	float spin, charge, r_horizon, r_isco;
	bool jets, ergo;
};

/* ------------------------------------------------------------------ */
/* shaders                                                            */
/* ------------------------------------------------------------------ */

static char *read_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = malloc(n + 1);
	if (buf && fread(buf, 1, n, f) != (size_t)n) {
		free(buf);
		buf = NULL;
	}
	if (buf)
		buf[n] = '\0';
	fclose(f);
	return buf;
}

static GLuint compile_shader(GLenum type, const char *path)
{
	char *src = read_file(path);
	if (!src)
		return 0;
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, (const char *const *)&src, NULL);
	glCompileShader(s);
	free(src);

	GLint ok;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[4096];
		glGetShaderInfoLog(s, sizeof log, NULL, log);
		fprintf(stderr, "%s:\n%s\n", path, log);
		glDeleteShader(s);
		return 0;
	}
	return s;
}

static bool load_program(struct app *app)
{
	GLuint vs = compile_shader(GL_VERTEX_SHADER, SHADER_DIR "/fullscreen.vert");
	GLuint fs = compile_shader(GL_FRAGMENT_SHADER, SHADER_DIR "/ton618_kn.frag");
	if (!vs || !fs) {
		glDeleteShader(vs);
		glDeleteShader(fs);
		return false;
	}

	GLuint prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glLinkProgram(prog);
	glDeleteShader(vs);
	glDeleteShader(fs);

	GLint ok;
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[4096];
		glGetProgramInfoLog(prog, sizeof log, NULL, log);
		fprintf(stderr, "link error:\n%s\n", log);
		glDeleteProgram(prog);
		return false;
	}

	/* only swap on success so a typo during hot-reload keeps the old image */
	if (app->program)
		glDeleteProgram(app->program);
	app->program = prog;
	app->u_resolution = glGetUniformLocation(prog, "uResolution");
	app->u_time = glGetUniformLocation(prog, "uTime");
	app->u_cam = glGetUniformLocation(prog, "uCam");
	app->u_bh = glGetUniformLocation(prog, "uBH");
	app->u_flags = glGetUniformLocation(prog, "uFlags");
	return true;
}

/* ------------------------------------------------------------------ */
/* rendering                                                          */
/* ------------------------------------------------------------------ */

static void ensure_fbo(struct app *app, int w, int h)
{
	if (app->fbo && app->fbo_w == w && app->fbo_h == h)
		return;
	if (!app->fbo) {
		glGenFramebuffers(1, &app->fbo);
		glGenTextures(1, &app->fbo_tex);
	}
	glBindTexture(GL_TEXTURE_2D, app->fbo_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glBindFramebuffer(GL_FRAMEBUFFER, app->fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, app->fbo_tex, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	app->fbo_w = w;
	app->fbo_h = h;
}

static void render(struct app *app)
{
	int rw = app->width, rh = app->height;
	bool scaled = app->render_scale < 0.999f;
	if (scaled) {
		rw = (int)(app->width * app->render_scale);
		rh = (int)(app->height * app->render_scale);
		if (rw < 1) rw = 1;
		if (rh < 1) rh = 1;
		ensure_fbo(app, rw, rh);
		glBindFramebuffer(GL_FRAMEBUFFER, app->fbo);
	}

	glViewport(0, 0, rw, rh);
	glUseProgram(app->program);
	glUniform2f(app->u_resolution, (float)rw, (float)rh);
	glUniform1f(app->u_time, (float)app->sim_time);
	glUniform3f(app->u_cam, app->yaw, app->pitch, app->dist);
	glUniform4f(app->u_bh, app->spin, app->charge, app->r_horizon, app->r_isco);
	glUniform2f(app->u_flags, app->jets ? 1.0f : 0.0f, app->ergo ? 1.0f : 0.0f);
	glBindVertexArray(app->vao);
	glDrawArrays(GL_TRIANGLES, 0, 3);

	if (scaled) {
		glBindFramebuffer(GL_READ_FRAMEBUFFER, app->fbo);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		glBlitFramebuffer(0, 0, rw, rh, 0, 0, app->width, app->height,
				  GL_COLOR_BUFFER_BIT, GL_LINEAR);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}

	eglSwapBuffers(app->egl_display, app->egl_surface);
}

/* ------------------------------------------------------------------ */
/* Kerr–Newman parameters                                             */
/* ------------------------------------------------------------------ */

/*
 * ISCO of a neutral particle on an equatorial Kerr–Newman orbit (M = 1): the
 * radius where the specific energy of circular orbits
 *   E(r) = (r^2 - 2r + Q^2 + a s) / (r sqrt(r^2 - 3r + 2Q^2 + 2a s)),  s = sqrt(r - Q^2)
 * is minimal. a > 0 is prograde, a < 0 retrograde. Reduces to 6 (Schwarzschild),
 * 1 / 9 (extremal Kerr pro/retrograde) and 4 (extremal Reissner–Nordström).
 */
static double kn_isco(double a, double q)
{
	double q2 = q * q;
	double rh = 1.0 + sqrt(1.0 - a * a - q2);
	double best_e = INFINITY, best_r = 6.0;
	for (double r = rh; r < 12.0; r += 1e-4) {
		double s2 = r - q2;
		if (s2 <= 0.0)
			continue;
		double s = sqrt(s2);
		double den = r * r - 3.0 * r + 2.0 * q2 + 2.0 * a * s;
		if (den <= 0.0) /* inside the circular photon orbit */
			continue;
		double e = (r * r - 2.0 * r + q2 + a * s) / (r * sqrt(den));
		if (e < best_e) {
			best_e = e;
			best_r = r;
		}
	}
	return best_r;
}

static void update_black_hole(struct app *app)
{
	app->r_horizon = 1.0f + sqrtf(1.0f - app->spin * app->spin - app->charge * app->charge);
	app->r_isco = (float)kn_isco(app->spin, app->charge);

	char title[128];
	snprintf(title, sizeof title, "TON 618 · Kerr–Newman · a=%.2f Q=%.2f", app->spin, app->charge);
	if (app->toplevel)
		xdg_toplevel_set_title(app->toplevel, title);
	fprintf(stderr, "a=%+.3f Q=%.3f  r+=%.3f  r_isco=%.3f\n",
		app->spin, app->charge, app->r_horizon, app->r_isco);
}

static void adjust_spin(struct app *app, float delta)
{
	float lim = sqrtf(MAX_SPIN_CHARGE2 - app->charge * app->charge);
	app->spin = fminf(fmaxf(app->spin + delta, -lim), lim);
	update_black_hole(app);
}

static void adjust_charge(struct app *app, float delta)
{
	float lim = sqrtf(MAX_SPIN_CHARGE2 - app->spin * app->spin);
	app->charge = fminf(fmaxf(app->charge + delta, 0.0f), lim);
	update_black_hole(app);
}

/* ------------------------------------------------------------------ */
/* xdg-shell                                                          */
/* ------------------------------------------------------------------ */

static void wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
	(void)data;
	xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = wm_base_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial)
{
	struct app *app = data;
	xdg_surface_ack_configure(xdg_surface, serial);

	int w = app->pending_w > 0 ? app->pending_w : (app->width > 0 ? app->width : DEFAULT_W);
	int h = app->pending_h > 0 ? app->pending_h : (app->height > 0 ? app->height : DEFAULT_H);
	if (w != app->width || h != app->height) {
		app->width = w;
		app->height = h;
		if (app->egl_window)
			wl_egl_window_resize(app->egl_window, w, h, 0, 0);
	}
	app->configured = true;
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t w, int32_t h,
			       struct wl_array *states)
{
	(void)toplevel;
	struct app *app = data;
	app->pending_w = w;
	app->pending_h = h;

	bool fs = false;
	uint32_t *s;
	wl_array_for_each(s, states) {
		if (*s == XDG_TOPLEVEL_STATE_FULLSCREEN)
			fs = true;
	}
	app->fullscreen = fs;
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
	(void)toplevel;
	((struct app *)data)->running = false;
}

static void toplevel_configure_bounds(void *data, struct xdg_toplevel *t, int32_t w, int32_t h)
{
	(void)data; (void)t; (void)w; (void)h;
}

static void toplevel_wm_capabilities(void *data, struct xdg_toplevel *t, struct wl_array *caps)
{
	(void)data; (void)t; (void)caps;
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
	.configure_bounds = toplevel_configure_bounds,
	.wm_capabilities = toplevel_wm_capabilities,
};

static void toggle_fullscreen(struct app *app)
{
	if (app->fullscreen)
		xdg_toplevel_unset_fullscreen(app->toplevel);
	else
		xdg_toplevel_set_fullscreen(app->toplevel, NULL);
}

/* ------------------------------------------------------------------ */
/* input                                                              */
/* ------------------------------------------------------------------ */

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
			  struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y)
{
	(void)surface;
	struct app *app = data;
	app->ptr_x = wl_fixed_to_double(x);
	app->ptr_y = wl_fixed_to_double(y);

	/* without this, compositors may show no cursor at all over our surface */
	if (app->cursor_theme && app->cursor_surface) {
		struct wl_cursor *cursor = wl_cursor_theme_get_cursor(app->cursor_theme, "left_ptr");
		if (!cursor)
			cursor = wl_cursor_theme_get_cursor(app->cursor_theme, "default");
		if (cursor) {
			struct wl_cursor_image *img = cursor->images[0];
			wl_pointer_set_cursor(pointer, serial, app->cursor_surface,
					      img->hotspot_x, img->hotspot_y);
			wl_surface_attach(app->cursor_surface, wl_cursor_image_get_buffer(img), 0, 0);
			wl_surface_damage(app->cursor_surface, 0, 0, img->width, img->height);
			wl_surface_commit(app->cursor_surface);
		}
	}
}

static void pointer_leave(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
	(void)p; (void)serial; (void)s;
	((struct app *)data)->dragging = false;
}

static void pointer_motion(void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	(void)p; (void)time;
	struct app *app = data;
	app->ptr_x = wl_fixed_to_double(x);
	app->ptr_y = wl_fixed_to_double(y);
	if (app->dragging) {
		app->yaw -= (float)(app->ptr_x - app->last_x) * 0.006f;
		app->pitch += (float)(app->ptr_y - app->last_y) * 0.006f;
		if (app->pitch > 1.5f) app->pitch = 1.5f;
		if (app->pitch < -1.5f) app->pitch = -1.5f;
	}
	app->last_x = app->ptr_x;
	app->last_y = app->ptr_y;
}

static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time,
			   uint32_t button, uint32_t state)
{
	(void)p; (void)time;
	struct app *app = data;
	bool pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
	if (button == BTN_LEFT) {
		app->dragging = pressed;
		app->last_x = app->ptr_x;
		app->last_y = app->ptr_y;
	} else if (button == BTN_RIGHT && pressed) {
		xdg_toplevel_move(app->toplevel, app->seat, serial);
	}
}

static void pointer_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t value)
{
	(void)p; (void)time;
	struct app *app = data;
	if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
		return;
	app->dist *= expf((float)wl_fixed_to_double(value) * 0.01f);
	if (app->dist < 8.0f) app->dist = 8.0f;
	if (app->dist > 200.0f) app->dist = 200.0f;
}

static void pointer_frame(void *data, struct wl_pointer *p) { (void)data; (void)p; }
static void pointer_axis_source(void *data, struct wl_pointer *p, uint32_t s) { (void)data; (void)p; (void)s; }
static void pointer_axis_stop(void *data, struct wl_pointer *p, uint32_t t, uint32_t a)
{
	(void)data; (void)p; (void)t; (void)a;
}
static void pointer_axis_discrete(void *data, struct wl_pointer *p, uint32_t a, int32_t d)
{
	(void)data; (void)p; (void)a; (void)d;
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
	.frame = pointer_frame,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete,
};

static void kb_keymap(void *data, struct wl_keyboard *k, uint32_t fmt, int32_t fd, uint32_t size)
{
	(void)data; (void)k; (void)fmt; (void)size;
	close(fd); /* raw evdev codes are enough here; no xkbcommon needed */
}

static void kb_enter(void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s,
		     struct wl_array *keys)
{
	(void)data; (void)k; (void)serial; (void)s; (void)keys;
}

static void kb_leave(void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s)
{
	(void)data; (void)k; (void)serial; (void)s;
}

static void kb_key(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t time, uint32_t key,
		   uint32_t state)
{
	(void)k; (void)serial; (void)time;
	struct app *app = data;
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
		return;
	switch (key) {
	case KEY_ESC:
	case KEY_Q:
		app->running = false;
		break;
	case KEY_F:
	case KEY_F11:
		toggle_fullscreen(app);
		break;
	case KEY_SPACE:
		app->paused = !app->paused;
		break;
	case KEY_R:
		if (load_program(app))
			fprintf(stderr, "shaders reloaded\n");
		break;
	case KEY_1:
		adjust_spin(app, -0.05f);
		break;
	case KEY_2:
		adjust_spin(app, 0.05f);
		break;
	case KEY_3:
		adjust_charge(app, -0.05f);
		break;
	case KEY_4:
		adjust_charge(app, 0.05f);
		break;
	case KEY_J:
		app->jets = !app->jets;
		break;
	case KEY_E:
		app->ergo = !app->ergo;
		break;
	}
}

static void kb_modifiers(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t dep,
			 uint32_t lat, uint32_t lock, uint32_t group)
{
	(void)data; (void)k; (void)serial; (void)dep; (void)lat; (void)lock; (void)group;
}

static void kb_repeat_info(void *data, struct wl_keyboard *k, int32_t rate, int32_t delay)
{
	(void)data; (void)k; (void)rate; (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = kb_keymap,
	.enter = kb_enter,
	.leave = kb_leave,
	.key = kb_key,
	.modifiers = kb_modifiers,
	.repeat_info = kb_repeat_info,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
	struct app *app = data;
	bool has_ptr = caps & WL_SEAT_CAPABILITY_POINTER;
	bool has_kb = caps & WL_SEAT_CAPABILITY_KEYBOARD;

	if (has_ptr && !app->pointer) {
		app->pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(app->pointer, &pointer_listener, app);
	} else if (!has_ptr && app->pointer) {
		wl_pointer_release(app->pointer);
		app->pointer = NULL;
	}

	if (has_kb && !app->keyboard) {
		app->keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(app->keyboard, &keyboard_listener, app);
	} else if (!has_kb && app->keyboard) {
		wl_keyboard_release(app->keyboard);
		app->keyboard = NULL;
	}
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
	(void)data; (void)seat; (void)name;
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

/* ------------------------------------------------------------------ */
/* registry                                                           */
/* ------------------------------------------------------------------ */

static uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
			    const char *iface, uint32_t version)
{
	struct app *app = data;
	if (!strcmp(iface, wl_compositor_interface.name)) {
		app->compositor = wl_registry_bind(reg, name, &wl_compositor_interface, min_u32(version, 4));
	} else if (!strcmp(iface, xdg_wm_base_interface.name)) {
		app->wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, min_u32(version, 4));
		xdg_wm_base_add_listener(app->wm_base, &wm_base_listener, app);
	} else if (!strcmp(iface, wl_seat_interface.name) && !app->seat) {
		app->seat = wl_registry_bind(reg, name, &wl_seat_interface, min_u32(version, 5));
		wl_seat_add_listener(app->seat, &seat_listener, app);
	} else if (!strcmp(iface, wl_shm_interface.name)) {
		app->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	} else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name)) {
		app->deco_manager = wl_registry_bind(reg, name, &zxdg_decoration_manager_v1_interface, 1);
	}
}

static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
	(void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* ------------------------------------------------------------------ */
/* setup                                                              */
/* ------------------------------------------------------------------ */

static bool init_egl(struct app *app)
{
	app->egl_display = eglGetDisplay((EGLNativeDisplayType)app->display);
	if (app->egl_display == EGL_NO_DISPLAY || !eglInitialize(app->egl_display, NULL, NULL)) {
		fprintf(stderr, "eglInitialize failed\n");
		return false;
	}
	if (!eglBindAPI(EGL_OPENGL_API)) {
		fprintf(stderr, "eglBindAPI(EGL_OPENGL_API) failed\n");
		return false;
	}

	const EGLint cfg_attrs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 0,
		EGL_NONE,
	};
	EGLConfig config;
	EGLint n = 0;
	if (!eglChooseConfig(app->egl_display, cfg_attrs, &config, 1, &n) || n < 1) {
		fprintf(stderr, "no suitable EGLConfig\n");
		return false;
	}

	const EGLint ctx_attrs[] = {
		EGL_CONTEXT_MAJOR_VERSION, 3,
		EGL_CONTEXT_MINOR_VERSION, 3,
		EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
		EGL_NONE,
	};
	app->egl_context = eglCreateContext(app->egl_display, config, EGL_NO_CONTEXT, ctx_attrs);
	if (app->egl_context == EGL_NO_CONTEXT) {
		fprintf(stderr, "eglCreateContext (GL 3.3 core) failed\n");
		return false;
	}

	app->egl_window = wl_egl_window_create(app->surface, app->width, app->height);
	app->egl_surface = eglCreateWindowSurface(app->egl_display, config,
						  (EGLNativeWindowType)app->egl_window, NULL);
	if (app->egl_surface == EGL_NO_SURFACE) {
		fprintf(stderr, "eglCreateWindowSurface failed\n");
		return false;
	}
	eglMakeCurrent(app->egl_display, app->egl_surface, app->egl_surface, app->egl_context);
	eglSwapInterval(app->egl_display, 1);

	fprintf(stderr, "GL: %s | %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
	return true;
}

static double now_seconds(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* Read and dispatch any pending Wayland events without blocking. */
static bool pump_events(struct wl_display *d)
{
	while (wl_display_prepare_read(d) != 0)
		if (wl_display_dispatch_pending(d) < 0)
			return false;
	wl_display_flush(d);

	struct pollfd pfd = { .fd = wl_display_get_fd(d), .events = POLLIN };
	if (poll(&pfd, 1, 0) > 0) {
		if (wl_display_read_events(d) < 0)
			return false;
	} else {
		wl_display_cancel_read(d);
	}
	return wl_display_dispatch_pending(d) >= 0;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [-s scale] [-a spin] [-q charge] [-f]\n"
		"  -s scale   internal render scale, 0.25..1.0 (default 0.6; lower = faster)\n"
		"  -a spin    dimensionless spin a/M, -1..1 (default 0.9)\n"
		"  -q charge  dimensionless charge Q/M, 0..1 (default 0.3)\n"
		"  -f         start fullscreen\n", argv0);
}

int main(int argc, char **argv)
{
	struct app app = {
		.width = DEFAULT_W,
		.height = DEFAULT_H,
		.running = true,
		.yaw = 0.3f,
		.pitch = 0.1f,
		.dist = 45.0f,
		.render_scale = 0.6f,
		.spin = 0.9f,
		.charge = 0.3f,
		.jets = true,
		.ergo = true,
	};
	bool start_fullscreen = false;

	int opt;
	while ((opt = getopt(argc, argv, "s:a:q:fh")) != -1) {
		switch (opt) {
		case 's':
			app.render_scale = strtof(optarg, NULL);
			if (app.render_scale < 0.25f) app.render_scale = 0.25f;
			if (app.render_scale > 1.0f) app.render_scale = 1.0f;
			break;
		case 'a':
			app.spin = strtof(optarg, NULL);
			break;
		case 'q':
			app.charge = fabsf(strtof(optarg, NULL));
			break;
		case 'f':
			start_fullscreen = true;
			break;
		default:
			usage(argv[0]);
			return opt == 'h' ? 0 : 1;
		}
	}

	app.display = wl_display_connect(NULL);
	if (!app.display) {
		fprintf(stderr, "cannot connect to a Wayland display (is WAYLAND_DISPLAY set?)\n");
		return 1;
	}
	app.registry = wl_display_get_registry(app.display);
	wl_registry_add_listener(app.registry, &registry_listener, &app);
	wl_display_roundtrip(app.display);

	if (!app.compositor || !app.wm_base) {
		fprintf(stderr, "compositor lacks wl_compositor or xdg_wm_base\n");
		return 1;
	}

	if (app.shm) {
		app.cursor_theme = wl_cursor_theme_load(NULL, 24, app.shm);
		app.cursor_surface = wl_compositor_create_surface(app.compositor);
	}

	app.surface = wl_compositor_create_surface(app.compositor);
	app.xdg_surface = xdg_wm_base_get_xdg_surface(app.wm_base, app.surface);
	xdg_surface_add_listener(app.xdg_surface, &xdg_surface_listener, &app);
	app.toplevel = xdg_surface_get_toplevel(app.xdg_surface);
	xdg_toplevel_add_listener(app.toplevel, &toplevel_listener, &app);
	xdg_toplevel_set_app_id(app.toplevel, "ton618-kn");
	app.charge = fminf(app.charge, sqrtf(MAX_SPIN_CHARGE2));
	app.spin = fminf(fmaxf(app.spin, -sqrtf(MAX_SPIN_CHARGE2 - app.charge * app.charge)),
			 sqrtf(MAX_SPIN_CHARGE2 - app.charge * app.charge));
	update_black_hole(&app);  /* computes r+ / ISCO, sets the title */

	/* server-side decorations where supported (KDE, sway, ...); GNOME has none */
	if (app.deco_manager) {
		app.decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(app.deco_manager, app.toplevel);
		zxdg_toplevel_decoration_v1_set_mode(app.decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
	if (start_fullscreen)
		xdg_toplevel_set_fullscreen(app.toplevel, NULL);

	wl_surface_commit(app.surface);
	while (!app.configured && wl_display_dispatch(app.display) != -1)
		;

	if (!init_egl(&app))
		return 1;

	glGenVertexArrays(1, &app.vao);
	if (!load_program(&app))
		return 1;

	double last = now_seconds();
	while (app.running) {
		if (!pump_events(app.display))
			break;

		double t = now_seconds();
		if (!app.paused)
			app.sim_time += t - last;
		last = t;

		render(&app); /* swap interval 1: blocks on the compositor's frame callback */
	}

	/* teardown */
	if (app.fbo) {
		glDeleteFramebuffers(1, &app.fbo);
		glDeleteTextures(1, &app.fbo_tex);
	}
	glDeleteProgram(app.program);
	glDeleteVertexArrays(1, &app.vao);
	eglMakeCurrent(app.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroySurface(app.egl_display, app.egl_surface);
	eglDestroyContext(app.egl_display, app.egl_context);
	wl_egl_window_destroy(app.egl_window);
	eglTerminate(app.egl_display);

	if (app.decoration)
		zxdg_toplevel_decoration_v1_destroy(app.decoration);
	xdg_toplevel_destroy(app.toplevel);
	xdg_surface_destroy(app.xdg_surface);
	wl_surface_destroy(app.surface);
	if (app.cursor_surface)
		wl_surface_destroy(app.cursor_surface);
	if (app.cursor_theme)
		wl_cursor_theme_destroy(app.cursor_theme);
	if (app.pointer)
		wl_pointer_release(app.pointer);
	if (app.keyboard)
		wl_keyboard_release(app.keyboard);
	if (app.seat)
		wl_seat_release(app.seat);
	if (app.deco_manager)
		zxdg_decoration_manager_v1_destroy(app.deco_manager);
	if (app.shm)
		wl_shm_destroy(app.shm);
	xdg_wm_base_destroy(app.wm_base);
	wl_compositor_destroy(app.compositor);
	wl_registry_destroy(app.registry);
	wl_display_disconnect(app.display);
	return 0;
}
