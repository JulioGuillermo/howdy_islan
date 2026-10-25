#pragma once

#include <cairo.h>

#include "island.h"

#define RENDER_CANVAS_WIDTH 480
#define RENDER_CANVAS_HEIGHT 150

/* Draws the island centered at the top of a RENDER_CANVAS_WIDTH x RENDER_CANVAS_HEIGHT canvas. */
void render_island(cairo_t *cr, const struct island *island);
