#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PREVIEW_WIDTH 192
#define PREVIEW_MAX_HEIGHT 240

/* A camera frame shrunk to PREVIEW_WIDTH, one gray byte per pixel. */
struct preview {
	int width;
	int height;
	bool from_gray_camera;
	uint8_t gray[PREVIEW_WIDTH * PREVIEW_MAX_HEIGHT];
};

/* Decodes one of Gaze's JPEG preview frames. Returns false when it is not a usable JPEG. */
bool preview_from_jpeg(struct preview *preview, const uint8_t *jpeg, size_t length);
