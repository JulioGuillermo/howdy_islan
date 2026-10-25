/*
 * GNOME has no wlr-layer-shell, so the howdy-island service cannot draw here. This extension draws
 * the same island inside the shell, on top of everything (lock screen included, through the
 * unlock-dialog session mode), from the scan the service passes on to it.
 */
import Clutter from 'gi://Clutter';
import Cogl from 'gi://Cogl';
import GLib from 'gi://GLib';
import St from 'gi://St';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {Feed} from './feed.js';
import {EXPANDED_HEIGHT, EXPANDED_WIDTH, Island, Phase} from './island.js';
import {
    CANVAS_HEIGHT, CANVAS_WIDTH, PREVIEW_BOX,
    drawBody, drawContentAbove, drawContentBelow, drawOutline, islandCenterX,
} from './render.js';

const RGB_BYTES_PER_PIXEL = 3;

function monotonicSeconds() {
    return GLib.get_monotonic_time() / 1e6;
}

function grayToRgb(gray) {
    const rgb = new Uint8Array(gray.length * RGB_BYTES_PER_PIXEL);
    for (let i = 0; i < gray.length; i++) {
        rgb[i * 3] = gray[i];
        rgb[i * 3 + 1] = gray[i];
        rgb[i * 3 + 2] = gray[i];
    }
    return rgb;
}

function drawingArea(width, height, draw) {
    const area = new St.DrawingArea({width, height});
    area.connect('repaint', () => {
        const cr = area.get_context();
        draw(cr);
        cr.$dispose();
    });
    return area;
}

/* The island's actors: body, then the content (clipped to the body), then the outline. */
class IslandOverlay {
    constructor(island) {
        this._island = island;
        this._shownPreview = null;

        this.actor = new Clutter.Actor({reactive: false, width: CANVAS_WIDTH, height: CANVAS_HEIGHT});
        this._body = drawingArea(CANVAS_WIDTH, CANVAS_HEIGHT, cr => drawBody(cr, island));
        this._contentClip = new St.Widget({clip_to_allocation: true});
        this._content = new Clutter.Actor({width: EXPANDED_WIDTH, height: EXPANDED_HEIGHT});
        this._belowPreview = drawingArea(EXPANDED_WIDTH, EXPANDED_HEIGHT, cr => drawContentBelow(cr, island));
        this._preview = new St.Widget({
            x: PREVIEW_BOX.x,
            y: PREVIEW_BOX.y,
            width: PREVIEW_BOX.width,
            height: PREVIEW_BOX.height,
            content_gravity: Clutter.ContentGravity.RESIZE_FILL,
            visible: false,
        });
        this._preview.set_pivot_point(0.5, 0.5);
        this._preview.scale_x = -1;
        this._abovePreview = drawingArea(EXPANDED_WIDTH, EXPANDED_HEIGHT, cr => drawContentAbove(cr, island));
        this._outline = drawingArea(CANVAS_WIDTH, CANVAS_HEIGHT, cr => drawOutline(cr, island));

        this._content.add_child(this._belowPreview);
        this._content.add_child(this._preview);
        this._content.add_child(this._abovePreview);
        this._contentClip.add_child(this._content);
        this.actor.add_child(this._body);
        this.actor.add_child(this._contentClip);
        this.actor.add_child(this._outline);
    }

    destroy() {
        this.actor.destroy();
    }

    update() {
        const island = this._island;
        const width = island.width.value;
        const height = island.height.value;
        const centerX = islandCenterX(island);
        const left = centerX - width / 2;
        const contentAlpha = Math.max(0, Math.min(island.expansion() * 1.6 - 0.6, 1));

        this._contentClip.set_position(left, 0);
        this._contentClip.set_size(Math.max(width, 0), Math.max(height, 0));
        this._content.set_position(centerX - EXPANDED_WIDTH / 2 - left, height - EXPANDED_HEIGHT);
        this._content.opacity = Math.round(contentAlpha * 255);
        this._content.visible = contentAlpha > 0;

        this._uploadPreview();
        this._body.queue_repaint();
        this._outline.queue_repaint();
        if (this._content.visible) {
            this._belowPreview.queue_repaint();
            this._abovePreview.queue_repaint();
        }
    }

    _uploadPreview() {
        const preview = this._island.preview;
        if (preview === this._shownPreview)
            return;
        this._shownPreview = preview;
        this._preview.visible = preview !== null;
        if (!preview)
            return;

        const coglContext = global.stage.context.get_backend().get_cogl_context();
        const image = St.ImageContent.new_with_preferred_size(preview.width, preview.height);
        image.set_bytes(coglContext, new GLib.Bytes(grayToRgb(preview.gray)), Cogl.PixelFormat.RGB_888,
            preview.width, preview.height, preview.width * RGB_BYTES_PER_PIXEL);
        this._preview.content = image;
    }
}

export default class HowdyIslandExtension extends Extension {
    enable() {
        this._island = new Island(monotonicSeconds());
        this._overlay = new IslandOverlay(this._island);
        Main.layoutManager.uiGroup.add_child(this._overlay.actor);

        this._timeline = new Clutter.Timeline({actor: this._overlay.actor, duration: 1000, repeat_count: -1});
        this._timeline.connect('new-frame', () => this._onFrame());
        this._overlay.actor.connect('destroy', () => this._timeline?.stop());

        this._monitorsChangedId = Main.layoutManager.connect('monitors-changed', () => this._place());
        this._fullscreenChangedId = global.display.connect('in-fullscreen-changed', () => this._updateVisibility());

        this._wasRaised = false;
        this._feed = new Feed(this._island, () => this._onMessages());
        this._place();
        this._overlay.update();
    }

    disable() {
        this._feed.destroy();
        Main.layoutManager.disconnect(this._monitorsChangedId);
        global.display.disconnect(this._fullscreenChangedId);
        this._timeline.stop();
        this._overlay.destroy();
        this._feed = null;
        this._timeline = null;
        this._overlay = null;
        this._island = null;
    }

    /* On the primary monitor, centered at its top edge. */
    _place() {
        const monitor = Main.layoutManager.primaryMonitor;
        if (!monitor)
            return;
        this._overlay.actor.set_position(monitor.x + Math.round((monitor.width - CANVAS_WIDTH) / 2), monitor.y);
        this._updateVisibility();
    }

    /* At rest it steps aside for fullscreen windows; open, it shows over everything. */
    _updateVisibility() {
        const monitor = Main.layoutManager.primaryMonitor;
        const inFullscreen = monitor?.inFullscreen ?? false;
        this._overlay.actor.visible = this._island.isRaised() || !inFullscreen;
    }

    _onMessages() {
        const raised = this._island.phase !== Phase.REST;
        if (raised && !this._wasRaised)
            Main.layoutManager.uiGroup.set_child_above_sibling(this._overlay.actor, null);
        this._wasRaised = raised;

        this._updateVisibility();
        this._overlay.update();
        if (!this._timeline.is_playing())
            this._timeline.start();
    }

    _onFrame() {
        this._island.advance(monotonicSeconds());
        this._overlay.update();
        if (!this._island.isRaised()) {
            this._timeline.stop();
            this._wasRaised = false;
            this._updateVisibility();
        }
    }
}
