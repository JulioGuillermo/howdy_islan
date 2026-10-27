/*
 * Cairo drawing for the island, the same as the wlroots service draws it. The IR preview itself is a
 * separate image actor (GJS cairo cannot take raw pixels), so the content is drawn in two layers
 * around it: below it the background and texts, above it the rounded corners, the faces and the
 * sweeping line.
 */
import Cairo from 'cairo';
import Pango from 'gi://Pango';
import PangoCairo from 'gi://PangoCairo';

import {EXPANDED_HEIGHT, EXPANDED_WIDTH, Phase} from './island.js';

export const CANVAS_WIDTH = 480;
export const CANVAS_HEIGHT = 150;

const SHOULDER = 12;
const SHOULDER_ROUNDNESS = 0.55;
const SHOULDER_MAX_DEPTH_SHARE = 0.6;
const OPENING_REACH_SHRINK = 0.4;
const OPENING_DEPTH_GROWTH = 0.9;
const CLOSING_REACH_GROWTH = 1.4;
const CLOSING_DEPTH_SHRINK = 0.5;
const CORNER = 34;
const CORNER_HANDLE = 0.3;
const OUTLINE_WIDTH = 1.5;
const OUTLINE_ALPHA = 0.45;

const PADDING = 12;
const PREVIEW_ASPECT = 1.6;
const PREVIEW_RADIUS = 12;
const PREVIEW_FEATHER = 10;
const RESULT_TINT_STRENGTH = 0.45;
const FACE_RADIUS = 6;
const LANDMARK_RADIUS = 1.6;
const LANDMARK_ALPHA = 0.7;
const LANDMARK_LINE_WIDTH = 0.8;
const LANDMARK_LINE_ALPHA = 0.3;
const BADGE_RADIUS = 13;
const BAR_HEIGHT = 4;
const TITLE_FONT = 'Sans Bold';
const TITLE_SIZE = 17;
const SUBTITLE_FONT = 'Sans';
const SUBTITLE_SIZE = 12.5;

const WHITE = [1, 1, 1];
const SUBTITLE_COLOR = [0.75, 0.75, 0.78];
const BLACK = [0, 0, 0];

/* The landmark lines: across the eyes, from each eye to the nose, from the nose to each mouth corner, and across the mouth. */
const LANDMARK_LINES = [[0, 1], [0, 2], [1, 2], [2, 3], [2, 4], [3, 4]];

const PREVIEW_HEIGHT = EXPANDED_HEIGHT - 2 * PADDING;

/* Where the IR preview sits inside the open island's content. */
export const PREVIEW_BOX = {
    x: PADDING,
    y: PADDING,
    width: PREVIEW_HEIGHT * PREVIEW_ASPECT,
    height: PREVIEW_HEIGHT,
};

function setColor(cr, [red, green, blue], alpha) {
    cr.setSourceRGBA(red, green, blue, alpha);
}

/*
 * The curves joining the island to the top edge. They stay glued to it and react to the motion:
 * stretched (deeper, narrower) while it comes out, squashed (flatter, wider) while it goes back.
 */
function shouldersFor(island) {
    const stretch = island.stretch();
    let reach = SHOULDER;
    let depth = SHOULDER;
    if (stretch > 0) {
        reach *= 1 - OPENING_REACH_SHRINK * stretch;
        depth *= 1 + OPENING_DEPTH_GROWTH * stretch;
    } else {
        reach *= 1 - CLOSING_REACH_GROWTH * stretch;
        depth *= 1 + CLOSING_DEPTH_SHRINK * stretch;
    }
    return {reach, depth: Math.min(depth, island.height.value * SHOULDER_MAX_DEPTH_SHARE)};
}

/* Notch hanging from the top edge, open along it: shoulders into the edge at the top, smooth corners below. */
function islandOutline(cr, centerX, width, height, {reach, depth}) {
    const left = centerX - width / 2;
    const right = centerX + width / 2;
    const reachHandle = reach * (1 - SHOULDER_ROUNDNESS);
    const depthHandle = depth * (1 - SHOULDER_ROUNDNESS);
    const corner = Math.max(0, Math.min(CORNER, height - depth, width / 2));
    const cornerHandle = corner * CORNER_HANDLE;

    cr.newPath();
    cr.moveTo(left - reach, 0);
    cr.curveTo(left - reachHandle, 0, left, depthHandle, left, depth);
    cr.lineTo(left, height - corner);
    cr.curveTo(left, height - cornerHandle, left + cornerHandle, height, left + corner, height);
    cr.lineTo(right - corner, height);
    cr.curveTo(right - cornerHandle, height, right, height - cornerHandle, right, height - corner);
    cr.lineTo(right, depth);
    cr.curveTo(right, depthHandle, right + reachHandle, 0, right + reach, 0);
}

function roundedRectangle(cr, x, y, width, height, radius) {
    radius = Math.min(radius, width / 2, height / 2);
    cr.newSubPath();
    cr.arc(x + width - radius, y + radius, radius, -Math.PI / 2, 0);
    cr.arc(x + width - radius, y + height - radius, radius, 0, Math.PI / 2);
    cr.arc(x + radius, y + height - radius, radius, Math.PI / 2, Math.PI);
    cr.arc(x + radius, y + radius, radius, Math.PI, 3 * Math.PI / 2);
    cr.closePath();
}

function clear(cr) {
    cr.save();
    cr.setOperator(Cairo.Operator.CLEAR);
    cr.paint();
    cr.restore();
}

export function islandCenterX(island) {
    return CANVAS_WIDTH / 2 + island.shakeOffset();
}

/* The black body, on a CANVAS_WIDTH x CANVAS_HEIGHT canvas. */
export function drawBody(cr, island) {
    clear(cr);
    if (island.width.value < 1 || island.height.value < 0.5)
        return;
    islandOutline(cr, islandCenterX(island), island.width.value, island.height.value, shouldersFor(island));
    cr.setSourceRGB(0, 0, 0);
    cr.fill();
}

/* The colored outline over everything, on the same canvas as drawBody. */
export function drawOutline(cr, island) {
    clear(cr);
    const expansion = island.expansion();
    if (expansion <= 0)
        return;
    islandOutline(cr, islandCenterX(island), island.width.value, island.height.value, shouldersFor(island));
    setColor(cr, island.accent, OUTLINE_ALPHA * expansion);
    cr.setLineWidth(OUTLINE_WIDTH);
    cr.stroke();
}

function drawText(cr, text, family, size, color, x, top, maxWidth) {
    const font = Pango.FontDescription.from_string(family);
    font.set_absolute_size(size * Pango.SCALE);
    const layout = PangoCairo.create_layout(cr);
    layout.set_font_description(font);
    layout.set_width(Math.round(maxWidth * Pango.SCALE));
    layout.set_ellipsize(Pango.EllipsizeMode.END);
    layout.set_text(text, -1);
    setColor(cr, color, 1);
    cr.moveTo(x, top);
    PangoCairo.show_layout(cr, layout);
}

function drawProgressBar(cr, fraction, accent, x, y, width) {
    roundedRectangle(cr, x, y, width, BAR_HEIGHT, BAR_HEIGHT / 2);
    setColor(cr, WHITE, 0.12);
    cr.fill();
    if (fraction <= 0)
        return;
    roundedRectangle(cr, x, y, Math.max(BAR_HEIGHT, width * fraction), BAR_HEIGHT, BAR_HEIGHT / 2);
    setColor(cr, accent, 1);
    cr.fill();
}

/* Spinning arc while working, a check on success, a cross on failure. */
function drawStatusBadge(cr, island, centerX, centerY) {
    const radius = BADGE_RADIUS;
    cr.setLineWidth(2.5);
    cr.setLineCap(Cairo.LineCap.ROUND);
    setColor(cr, island.accent, 1);
    cr.newPath();

    if (island.phase === Phase.SUCCESS) {
        cr.moveTo(centerX - radius * 0.5, centerY);
        cr.lineTo(centerX - radius * 0.1, centerY + radius * 0.4);
        cr.lineTo(centerX + radius * 0.55, centerY - radius * 0.4);
    } else if (island.phase === Phase.FAILURE) {
        const arm = radius * 0.42;
        cr.moveTo(centerX - arm, centerY - arm);
        cr.lineTo(centerX + arm, centerY + arm);
        cr.moveTo(centerX + arm, centerY - arm);
        cr.lineTo(centerX - arm, centerY + arm);
    } else {
        const start = (island.now % 1) * 2 * Math.PI;
        cr.arc(centerX, centerY, radius * 0.7, start, start + Math.PI * 1.3);
    }
    cr.stroke();
}

/* Below the preview: its empty frame, the texts, the progress bar and the badge. EXPANDED_WIDTH x EXPANDED_HEIGHT. */
export function drawContentBelow(cr, island) {
    clear(cr);
    const box = PREVIEW_BOX;
    roundedRectangle(cr, box.x, box.y, box.width, box.height, PREVIEW_RADIUS);
    setColor(cr, BLACK, 1);
    cr.fill();

    const textLeft = 2 * PADDING + box.width;
    const badgeCenterX = EXPANDED_WIDTH - PADDING - BADGE_RADIUS;
    const textMaxWidth = badgeCenterX - BADGE_RADIUS - PADDING - textLeft;
    drawText(cr, island.title, TITLE_FONT, TITLE_SIZE, WHITE, textLeft, PADDING + 2, textMaxWidth);
    drawText(cr, island.subtitle, SUBTITLE_FONT, SUBTITLE_SIZE, SUBTITLE_COLOR, textLeft, PADDING + 26, textMaxWidth);
    drawProgressBar(cr, island.progress(), island.accent, textLeft,
        EXPANDED_HEIGHT - PADDING - 2 * BAR_HEIGHT, textMaxWidth);
    drawStatusBadge(cr, island, badgeCenterX, PADDING + 16);
}

/* Above the preview: its rounded corners (black, like the body), the sweeping line and the face boxes, mirrored. */
export function drawContentAbove(cr, island) {
    clear(cr);
    const box = PREVIEW_BOX;

    cr.rectangle(box.x, box.y, box.width, box.height);
    roundedRectangle(cr, box.x, box.y, box.width, box.height, PREVIEW_RADIUS);
    cr.setFillRule(Cairo.FillRule.EVEN_ODD);
    cr.setSourceRGB(0, 0, 0);
    cr.fill();
    cr.setFillRule(Cairo.FillRule.WINDING);

    if (island.preview)
        featherEdges(cr, box);

    const scanLine = island.scanLine();
    if (scanLine !== null) {
        cr.rectangle(box.x, box.y + scanLine * box.height - 1, box.width, 2);
        setColor(cr, island.accent, 0.55);
        cr.fill();
    }

    cr.setLineWidth(2);
    for (const face of island.faces) {
        const mirroredX = 1 - face.x - face.width;
        roundedRectangle(cr, box.x + mirroredX * box.width, box.y + face.y * box.height,
            face.width * box.width, face.height * box.height, FACE_RADIUS);
        setColor(cr, island.accent, 0.95);
        cr.stroke();
    }
    if (island.landmarks)
        drawLandmarks(cr, island, box);
}

/* Each colour channel's scale toward the accent: gray, a little green on success, a little red on failure. */
export function previewTint(island) {
    const strength = RESULT_TINT_STRENGTH * island.resultTint();
    return island.accent.map(channel => 1 - strength + strength * channel);
}

/* Black fading in from one edge of the preview over PREVIEW_FEATHER. */
function fadeEdge(cr, x0, y0, x1, y1) {
    const fade = new Cairo.LinearGradient(x0, y0, x1, y1);
    fade.addColorStopRGBA(0, 0, 0, 0, 1);
    fade.addColorStopRGBA(1, 0, 0, 0, 0);
    cr.setSource(fade);
    cr.paint();
}

/* The preview's edges melt into the island's black instead of ending in a hard line. */
function featherEdges(cr, box) {
    cr.save();
    cr.rectangle(box.x, box.y, box.width, box.height);
    cr.clip();
    fadeEdge(cr, box.x, box.y, box.x + PREVIEW_FEATHER, box.y);
    fadeEdge(cr, box.x + box.width, box.y, box.x + box.width - PREVIEW_FEATHER, box.y);
    fadeEdge(cr, box.x, box.y, box.x, box.y + PREVIEW_FEATHER);
    fadeEdge(cr, box.x, box.y + box.height, box.x, box.y + box.height - PREVIEW_FEATHER);
    cr.restore();
}

/* The eyes, nose tip and mouth corners as small dots, joined by faint lines. */
function drawLandmarks(cr, island, box) {
    const points = island.landmarks.map(({x, y}) => ({
        x: box.x + (1 - x) * box.width,
        y: box.y + y * box.height,
    }));

    cr.setLineWidth(LANDMARK_LINE_WIDTH);
    for (const [from, to] of LANDMARK_LINES) {
        cr.moveTo(points[from].x, points[from].y);
        cr.lineTo(points[to].x, points[to].y);
    }
    setColor(cr, island.accent, LANDMARK_LINE_ALPHA);
    cr.stroke();

    for (const point of points) {
        cr.newSubPath();
        cr.arc(point.x, point.y, LANDMARK_RADIUS, 0, 2 * Math.PI);
    }
    setColor(cr, island.accent, LANDMARK_ALPHA);
    cr.fill();
}
