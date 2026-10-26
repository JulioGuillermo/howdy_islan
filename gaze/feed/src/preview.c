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

static void shrink(struct preview *preview, const uint8_t *gray, int width, int height)
{
	preview->width = PREVIEW_WIDTH;
	preview->height = (int)fmin(round((double)height * PREVIEW_WIDTH / width), PREVIEW_MAX_HEIGHT);
	for (int y = 0; y < preview->height; y++) {
		const uint8_t *source_row = gray + (size_t)(y * height / preview->height) * width;
		for (int x = 0; x < preview->width; x++)
			preview->gray[y * preview->width + x] = source_row[x * width / preview->width];
	}
}

bool preview_from_jpeg(struct preview *preview, const uint8_t *jpeg, size_t length)
{
	tjhandle decoder = tj3Init(TJINIT_DECOMPRESS);
	if (!decoder)
		return false;

	bool decoded = false;
	uint8_t *gray = NULL;
	if (tj3DecompressHeader(decoder, jpeg, length) == 0) {
		int width = tj3Get(decoder, TJPARAM_JPEGWIDTH);
		int height = tj3Get(decoder, TJPARAM_JPEGHEIGHT);
		tjscalingfactor scale = scale_for(width);
		int scaled_width = TJSCALED(width, scale);
		int scaled_height = TJSCALED(height, scale);
		gray = malloc((size_t)scaled_width * scaled_height);
		decoded = gray && scaled_width > 0 && scaled_height > 0 &&
			tj3SetScalingFactor(decoder, scale) == 0 &&
			tj3Decompress8(decoder, jpeg, length, gray, scaled_width, TJPF_GRAY) == 0;
		if (decoded) {
			preview->from_gray_camera = tj3Get(decoder, TJPARAM_COLORSPACE) == TJCS_GRAY;
			shrink(preview, gray, scaled_width, scaled_height);
		}
	}
	free(gray);
	tj3Destroy(decoder);
	return decoded;
}
