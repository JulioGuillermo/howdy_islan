#include "wayland.h"

#include <cairo.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "fractional-scale-v1-client-protocol.h"
#include "render.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define NAMESPACE "howdy-island"
#define BUFFER_COUNT 2
#define FRACTIONAL_SCALE_DENOMINATOR 120
#define MIN_COMPOSITOR_VERSION 4
#define WANTED_COMPOSITOR_VERSION 6
#define WANTED_LAYER_SHELL_VERSION 4
#define WANTED_OUTPUT_VERSION 4
#define LAYER_SWITCH_VERSION 2

static const char *const INTERNAL_PANEL_PREFIXES[] = {"eDP", "LVDS", "DSI"};

struct buffer {
	struct wl_buffer *wl_buffer;
	cairo_surface_t *cairo_surface;
	void *data;
	size_t size;
	int width;
	int height;
	bool busy;
};

struct output {
	struct display *display;
	struct wl_output *wl_output;
	uint32_t global_name;
	uint32_t version;
	char *name;
	struct island_surface *surface;
	struct wl_list link;
};

struct island_surface {
	struct display *display;
	struct output *output;
	struct wl_surface *wl_surface;
	struct zwlr_layer_surface_v1 *layer_surface;
	struct wp_viewport *viewport;
	struct wp_fractional_scale_v1 *fractional_scale;
	struct wl_callback *frame_callback;
	uint32_t scale_120;
	int32_t integer_scale;
	bool configured;
	bool dirty;
	bool raised;
	struct buffer buffers[BUFFER_COUNT];
	struct wl_list link;
};

struct display {
	struct wl_display *wl_display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	uint32_t compositor_version;
	struct wl_shm *shm;
	struct zwlr_layer_shell_v1 *layer_shell;
	uint32_t layer_shell_version;
	struct wp_viewporter *viewporter;
	struct wp_fractional_scale_manager_v1 *fractional_scale_manager;
	struct wl_list outputs;
	struct wl_list surfaces;
	struct island_surface *unassigned_surface;
	bool ready;
};

static uint32_t min_version(uint32_t offered, uint32_t wanted)
{
	return offered < wanted ? offered : wanted;
}

/* ---- Buffers ---- */

static void handle_buffer_release(void *data, struct wl_buffer *wl_buffer)
{
	struct buffer *buffer = data;
	buffer->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {
	.release = handle_buffer_release,
};

static void buffer_destroy(struct buffer *buffer)
{
	if (buffer->cairo_surface)
		cairo_surface_destroy(buffer->cairo_surface);
	if (buffer->wl_buffer)
		wl_buffer_destroy(buffer->wl_buffer);
	if (buffer->data)
		munmap(buffer->data, buffer->size);
	*buffer = (struct buffer){0};
}

static bool buffer_create(struct buffer *buffer, struct wl_shm *shm, int width, int height)
{
	int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
	size_t size = (size_t)stride * height;

	int fd = memfd_create("howdy-island", MFD_CLOEXEC);
	if (fd < 0)
		return false;
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		return false;
	}
	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		return false;
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
	buffer->wl_buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);

	wl_buffer_add_listener(buffer->wl_buffer, &buffer_listener, buffer);
	buffer->cairo_surface = cairo_image_surface_create_for_data(data, CAIRO_FORMAT_ARGB32, width, height, stride);
	buffer->data = data;
	buffer->size = size;
	buffer->width = width;
	buffer->height = height;
	buffer->busy = false;
	return true;
}

static struct buffer *acquire_buffer(struct island_surface *surface, int width, int height)
{
	for (int i = 0; i < BUFFER_COUNT; i++) {
		struct buffer *buffer = &surface->buffers[i];
		if (buffer->busy)
			continue;
		if (buffer->wl_buffer && buffer->width == width && buffer->height == height)
			return buffer;

		buffer_destroy(buffer);
		if (buffer_create(buffer, surface->display->shm, width, height))
			return buffer;
		return NULL;
	}
	return NULL;
}

/* ---- Island surfaces ---- */

static void surface_destroy(struct island_surface *surface)
{
	if (surface->frame_callback)
		wl_callback_destroy(surface->frame_callback);
	for (int i = 0; i < BUFFER_COUNT; i++)
		buffer_destroy(&surface->buffers[i]);
	if (surface->fractional_scale)
		wp_fractional_scale_v1_destroy(surface->fractional_scale);
	if (surface->viewport)
		wp_viewport_destroy(surface->viewport);
	zwlr_layer_surface_v1_destroy(surface->layer_surface);
	wl_surface_destroy(surface->wl_surface);
	wl_list_remove(&surface->link);
	free(surface);
}

static void forget_surface(struct island_surface *surface)
{
	if (surface->output)
		surface->output->surface = NULL;
	else
		surface->display->unassigned_surface = NULL;
	surface_destroy(surface);
}

static void handle_layer_configure(void *data, struct zwlr_layer_surface_v1 *layer_surface, uint32_t serial,
	uint32_t width, uint32_t height)
{
	struct island_surface *surface = data;
	zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
	surface->configured = true;
	surface->dirty = true;
}

static void handle_layer_closed(void *data, struct zwlr_layer_surface_v1 *layer_surface)
{
	forget_surface(data);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = handle_layer_configure,
	.closed = handle_layer_closed,
};

static void handle_preferred_scale(void *data, struct wp_fractional_scale_v1 *fractional_scale, uint32_t scale_120)
{
	struct island_surface *surface = data;
	surface->scale_120 = scale_120;
	surface->dirty = true;
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
	.preferred_scale = handle_preferred_scale,
};

static void handle_surface_enter(void *data, struct wl_surface *wl_surface, struct wl_output *output)
{
}

static void handle_surface_leave(void *data, struct wl_surface *wl_surface, struct wl_output *output)
{
}

static void handle_preferred_buffer_scale(void *data, struct wl_surface *wl_surface, int32_t factor)
{
	struct island_surface *surface = data;
	surface->integer_scale = factor;
	surface->dirty = true;
}

static void handle_preferred_buffer_transform(void *data, struct wl_surface *wl_surface, uint32_t transform)
{
}

static const struct wl_surface_listener surface_listener = {
	.enter = handle_surface_enter,
	.leave = handle_surface_leave,
	.preferred_buffer_scale = handle_preferred_buffer_scale,
	.preferred_buffer_transform = handle_preferred_buffer_transform,
};

static bool can_switch_layer(const struct display *display)
{
	return display->layer_shell_version >= LAYER_SWITCH_VERSION;
}

static struct island_surface *surface_create(struct display *display, struct output *output, bool raised)
{
	struct island_surface *surface = calloc(1, sizeof *surface);
	surface->display = display;
	surface->output = output;
	surface->integer_scale = 1;
	surface->raised = raised || !can_switch_layer(display);

	surface->wl_surface = wl_compositor_create_surface(display->compositor);
	if (display->compositor_version >= WANTED_COMPOSITOR_VERSION)
		wl_surface_add_listener(surface->wl_surface, &surface_listener, surface);

	if (display->viewporter && display->fractional_scale_manager) {
		surface->viewport = wp_viewporter_get_viewport(display->viewporter, surface->wl_surface);
		surface->fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
			display->fractional_scale_manager, surface->wl_surface);
		wp_fractional_scale_v1_add_listener(surface->fractional_scale, &fractional_scale_listener, surface);
	}

	struct wl_region *nothing = wl_compositor_create_region(display->compositor);
	wl_surface_set_input_region(surface->wl_surface, nothing);
	wl_region_destroy(nothing);

	uint32_t layer = surface->raised ? ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY : ZWLR_LAYER_SHELL_V1_LAYER_TOP;
	surface->layer_surface = zwlr_layer_shell_v1_get_layer_surface(display->layer_shell, surface->wl_surface,
		output ? output->wl_output : NULL, layer, NAMESPACE);
	zwlr_layer_surface_v1_add_listener(surface->layer_surface, &layer_surface_listener, surface);
	zwlr_layer_surface_v1_set_size(surface->layer_surface, RENDER_CANVAS_WIDTH, RENDER_CANVAS_HEIGHT);
	zwlr_layer_surface_v1_set_anchor(surface->layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP);
	zwlr_layer_surface_v1_set_exclusive_zone(surface->layer_surface, -1);
	zwlr_layer_surface_v1_set_keyboard_interactivity(surface->layer_surface,
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
	wl_surface_commit(surface->wl_surface);

	wl_list_insert(&display->surfaces, &surface->link);
	return surface;
}

static void handle_frame_done(void *data, struct wl_callback *callback, uint32_t time)
{
	struct island_surface *surface = data;
	wl_callback_destroy(callback);
	surface->frame_callback = NULL;
}

static const struct wl_callback_listener frame_listener = {
	.done = handle_frame_done,
};

static void buffer_size(const struct island_surface *surface, int *width, int *height)
{
	if (surface->viewport && surface->scale_120 > 0) {
		int half = FRACTIONAL_SCALE_DENOMINATOR / 2;
		*width = (RENDER_CANVAS_WIDTH * (int)surface->scale_120 + half) / FRACTIONAL_SCALE_DENOMINATOR;
		*height = (RENDER_CANVAS_HEIGHT * (int)surface->scale_120 + half) / FRACTIONAL_SCALE_DENOMINATOR;
		return;
	}
	*width = RENDER_CANVAS_WIDTH * surface->integer_scale;
	*height = RENDER_CANVAS_HEIGHT * surface->integer_scale;
}

static void lower(struct island_surface *surface)
{
	if (!surface->raised || !can_switch_layer(surface->display))
		return;
	zwlr_layer_surface_v1_set_layer(surface->layer_surface, ZWLR_LAYER_SHELL_V1_LAYER_TOP);
	surface->raised = false;
}

/*
 * Moving a surface to the overlay layer does not put it above the overlay surfaces already there,
 * like a greeter covering the whole screen. A new overlay surface goes on top of them, so the island
 * is created again each time it opens.
 */
static void raise_above_everything(struct island_surface *surface)
{
	struct display *display = surface->display;
	struct output *output = surface->output;
	surface_destroy(surface);

	struct island_surface *raised = surface_create(display, output, true);
	if (output)
		output->surface = raised;
	else
		display->unassigned_surface = raised;
}

static void surface_render(struct island_surface *surface, const struct island *island)
{
	if (!surface->configured || surface->frame_callback)
		return;
	if (!surface->dirty && !island_is_animating(island))
		return;

	int width;
	int height;
	buffer_size(surface, &width, &height);
	struct buffer *buffer = acquire_buffer(surface, width, height);
	if (!buffer)
		return;

	cairo_t *cr = cairo_create(buffer->cairo_surface);
	cairo_scale(cr, (double)width / RENDER_CANVAS_WIDTH, (double)height / RENDER_CANVAS_HEIGHT);
	render_island(cr, island);
	cairo_destroy(cr);
	cairo_surface_flush(buffer->cairo_surface);

	if (!island_is_raised(island))
		lower(surface);
	if (surface->viewport) {
		wp_viewport_set_destination(surface->viewport, RENDER_CANVAS_WIDTH, RENDER_CANVAS_HEIGHT);
		wl_surface_set_buffer_scale(surface->wl_surface, 1);
	} else {
		wl_surface_set_buffer_scale(surface->wl_surface, surface->integer_scale);
	}
	wl_surface_attach(surface->wl_surface, buffer->wl_buffer, 0, 0);
	wl_surface_damage_buffer(surface->wl_surface, 0, 0, width, height);
	surface->frame_callback = wl_surface_frame(surface->wl_surface);
	wl_callback_add_listener(surface->frame_callback, &frame_listener, surface);
	wl_surface_commit(surface->wl_surface);

	buffer->busy = true;
	surface->dirty = false;
}

/* ---- Outputs: the island goes on the internal panel, under the camera ---- */

static bool is_internal_panel(const struct output *output)
{
	if (!output->name)
		return false;
	for (size_t i = 0; i < sizeof INTERNAL_PANEL_PREFIXES / sizeof *INTERNAL_PANEL_PREFIXES; i++) {
		const char *prefix = INTERNAL_PANEL_PREFIXES[i];
		if (strncmp(output->name, prefix, strlen(prefix)) == 0)
			return true;
	}
	return false;
}

static void reconcile_surfaces(struct display *display)
{
	if (!display->ready)
		return;

	bool has_internal_panel = false;
	struct output *output;
	wl_list_for_each(output, &display->outputs, link) {
		if (is_internal_panel(output))
			has_internal_panel = true;
	}

	wl_list_for_each(output, &display->outputs, link) {
		bool wanted = is_internal_panel(output);
		if (wanted && !output->surface) {
			output->surface = surface_create(display, output, false);
		} else if (!wanted && output->surface) {
			surface_destroy(output->surface);
			output->surface = NULL;
		}
	}

	if (!has_internal_panel && !display->unassigned_surface) {
		display->unassigned_surface = surface_create(display, NULL, false);
	} else if (has_internal_panel && display->unassigned_surface) {
		surface_destroy(display->unassigned_surface);
		display->unassigned_surface = NULL;
	}
}

static void handle_output_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y, int32_t width_mm,
	int32_t height_mm, int32_t subpixel, const char *make, const char *model, int32_t transform)
{
}

static void handle_output_mode(void *data, struct wl_output *wl_output, uint32_t flags, int32_t width, int32_t height,
	int32_t refresh)
{
}

static void handle_output_done(void *data, struct wl_output *wl_output)
{
	struct output *output = data;
	reconcile_surfaces(output->display);
}

static void handle_output_scale(void *data, struct wl_output *wl_output, int32_t factor)
{
}

static void handle_output_name(void *data, struct wl_output *wl_output, const char *name)
{
	struct output *output = data;
	free(output->name);
	output->name = strdup(name);
}

static void handle_output_description(void *data, struct wl_output *wl_output, const char *description)
{
}

static const struct wl_output_listener output_listener = {
	.geometry = handle_output_geometry,
	.mode = handle_output_mode,
	.done = handle_output_done,
	.scale = handle_output_scale,
	.name = handle_output_name,
	.description = handle_output_description,
};

static void add_output(struct display *display, uint32_t global_name, uint32_t offered_version)
{
	struct output *output = calloc(1, sizeof *output);
	output->display = display;
	output->global_name = global_name;
	output->version = min_version(offered_version, WANTED_OUTPUT_VERSION);
	output->wl_output = wl_registry_bind(display->registry, global_name, &wl_output_interface, output->version);
	wl_output_add_listener(output->wl_output, &output_listener, output);
	wl_list_insert(&display->outputs, &output->link);
}

static void output_destroy(struct output *output)
{
	if (output->surface)
		surface_destroy(output->surface);
	if (output->version >= WL_OUTPUT_RELEASE_SINCE_VERSION)
		wl_output_release(output->wl_output);
	else
		wl_output_destroy(output->wl_output);
	wl_list_remove(&output->link);
	free(output->name);
	free(output);
}

/* ---- Registry ---- */

static void handle_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
	struct display *display = data;

	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		display->compositor_version = min_version(version, WANTED_COMPOSITOR_VERSION);
		display->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, display->compositor_version);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		display->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		display->layer_shell_version = min_version(version, WANTED_LAYER_SHELL_VERSION);
		display->layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, display->layer_shell_version);
	} else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
		display->viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
	} else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
		display->fractional_scale_manager = wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		add_output(display, name, version);
	}
}

static void handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	struct display *display = data;
	struct output *output;
	struct output *next;
	wl_list_for_each_safe(output, next, &display->outputs, link) {
		if (output->global_name == name) {
			output_destroy(output);
			reconcile_surfaces(display);
			return;
		}
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove,
};

/* ---- Display ---- */

enum connect_result display_connect(const char *socket_path, struct display **result)
{
	*result = NULL;
	struct wl_display *wl_display = wl_display_connect(socket_path);
	if (!wl_display)
		return CONNECT_UNREACHABLE;

	struct display *display = calloc(1, sizeof *display);
	display->wl_display = wl_display;
	wl_list_init(&display->outputs);
	wl_list_init(&display->surfaces);
	display->registry = wl_display_get_registry(wl_display);
	wl_registry_add_listener(display->registry, &registry_listener, display);

	bool output_names_received = wl_display_roundtrip(wl_display) >= 0 && wl_display_roundtrip(wl_display) >= 0;
	if (!output_names_received) {
		display_destroy(display);
		return CONNECT_UNREACHABLE;
	}

	bool supported = display->compositor && display->compositor_version >= MIN_COMPOSITOR_VERSION && display->shm &&
		display->layer_shell;
	if (!supported) {
		display_destroy(display);
		return CONNECT_UNSUPPORTED;
	}

	display->ready = true;
	reconcile_surfaces(display);
	*result = display;
	return CONNECT_OK;
}

void display_destroy(struct display *display)
{
	struct output *output;
	struct output *next_output;
	wl_list_for_each_safe(output, next_output, &display->outputs, link)
		output_destroy(output);
	if (display->unassigned_surface)
		surface_destroy(display->unassigned_surface);

	if (display->fractional_scale_manager)
		wp_fractional_scale_manager_v1_destroy(display->fractional_scale_manager);
	if (display->viewporter)
		wp_viewporter_destroy(display->viewporter);
	if (display->layer_shell && display->layer_shell_version >= ZWLR_LAYER_SHELL_V1_DESTROY_SINCE_VERSION)
		zwlr_layer_shell_v1_destroy(display->layer_shell);
	else if (display->layer_shell)
		wl_proxy_destroy((struct wl_proxy *)display->layer_shell);
	if (display->shm)
		wl_shm_destroy(display->shm);
	if (display->compositor)
		wl_compositor_destroy(display->compositor);
	wl_registry_destroy(display->registry);
	wl_display_disconnect(display->wl_display);
	free(display);
}

int display_prepare_poll(struct display *display)
{
	while (wl_display_prepare_read(display->wl_display) != 0) {
		if (wl_display_dispatch_pending(display->wl_display) < 0)
			return -1;
	}
	if (wl_display_flush(display->wl_display) < 0 && errno != EAGAIN) {
		wl_display_cancel_read(display->wl_display);
		return -1;
	}
	return wl_display_get_fd(display->wl_display);
}

bool display_after_poll(struct display *display, bool readable)
{
	if (!readable)
		wl_display_cancel_read(display->wl_display);
	else if (wl_display_read_events(display->wl_display) < 0)
		return false;
	return wl_display_dispatch_pending(display->wl_display) >= 0;
}

void display_mark_dirty(struct display *display)
{
	struct island_surface *surface;
	wl_list_for_each(surface, &display->surfaces, link)
		surface->dirty = true;
}

void display_render(struct display *display, const struct island *island)
{
	struct island_surface *surface;
	struct island_surface *next;
	wl_list_for_each_safe(surface, next, &display->surfaces, link) {
		if (island_is_raised(island) && !surface->raised)
			raise_above_everything(surface);
		else
			surface_render(surface, island);
	}
}
