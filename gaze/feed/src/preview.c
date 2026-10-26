#include "preview.h"

#include <math.h>
#include <stdlib.h>
#include <turbojpeg.h>

/* The smallest of libjpeg's decoding scales that still gives at least the preview's width. */
static tjscalingfactor scale_for(int width)
{
	int count = 0;
	tjscalingfactor *factors = tj3GetScalingFactors(&count);
	tjscalingfactor best = {1, 1};
	for (int i = 0; i < count; i++) {
		int scaled = TJSCALED(width, factors[i]);
		if (scaled >= PREVIEW_WIDTH && scaled < TJSCALED(width, best))
			best = factors[i];
	}
	return best;
}

#define GRAY_CHANNEL_TOLERANCE 6

/* Gaze encodes every frame in colour; an IR frame is one whose three channels are about equal. */
static bool looks_gray(const uint8_t *rgb, size_t pixels)
{
	long difference = 0;
	for (size_t i = 0; i < pixels; i++) {
		const uint8_t *pixel = rgb + 3 * i;
		difference += abs(pixel[0] - pixel[1]) + abs(pixel[1] - pixel[2]);
	}
	return difference <= (long)pixels * GRAY_CHANNEL_TOLERANCE;
}

static void shrink(struct preview *preview, const uint8_t *rgb, int width, int height)
{
	preview->width = PREVIEW_WIDTH;
	preview->height = (int)fmin(round((double)height * PREVIEW_WIDTH / width), PREVIEW_MAX_HEIGHT);
	for (int y = 0; y < preview->height; y++) {
		const uint8_t *source_row = rgb + (size_t)(y * height / preview->height) * width * 3;
		for (int x = 0; x < preview->width; x++) {
			const uint8_t *pixel = source_row + (size_t)(x * width / preview->width) * 3;
			preview->gray[y * preview->width + x] = (uint8_t)((pixel[0] * 77 + pixel[1] * 150 + pixel[2] * 29) >> 8);
		}
	}
}

bool preview_from_jpeg(struct preview *preview, const uint8_t *jpeg, size_t length)
{
	tjhandle decoder = tj3Init(TJINIT_DECOMPRESS);
	if (!decoder)
		return false;

	bool decoded = false;
	uint8_t *rgb = NULL;
	if (tj3DecompressHeader(decoder, jpeg, length) == 0) {
		int width = tj3Get(decoder, TJPARAM_JPEGWIDTH);
		int height = tj3Get(decoder, TJPARAM_JPEGHEIGHT);
		tjscalingfactor scale = scale_for(width);
		int scaled_width = TJSCALED(width, scale);
		int scaled_height = TJSCALED(height, scale);
		rgb = malloc((size_t)scaled_width * scaled_height * 3);
		decoded = rgb && scaled_width > 0 && scaled_height > 0 &&
			tj3SetScalingFactor(decoder, scale) == 0 &&
			tj3Decompress8(decoder, jpeg, length, rgb, 0, TJPF_RGB) == 0;
		if (decoded) {
			preview->from_gray_camera = looks_gray(rgb, (size_t)scaled_width * scaled_height);
			shrink(preview, rgb, scaled_width, scaled_height);
		}
	}
	free(rgb);
	tj3Destroy(decoder);
	return decoded;
}
