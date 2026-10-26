import {Spring} from './spring.js';

export const REST_WIDTH = 130;
export const REST_HEIGHT = 7;
export const EXPANDED_WIDTH = 380;
export const EXPANDED_HEIGHT = 112;

const WIDTH_STIFFNESS = 260;
const WIDTH_DAMPING = 21;
const HEIGHT_STIFFNESS = 320;
const HEIGHT_DAMPING = 23;
const WIDTH_CLOSING_DAMPING = 32;
const HEIGHT_CLOSING_DAMPING = 36;

const SUCCESS_HOLD_SECONDS = 0.9;
const FAILURE_HOLD_SECONDS = 1.8;
const SILENCE_LIMIT_SECONDS = 5;
const SHAKE_SECONDS = 0.6;
const SHAKE_AMPLITUDE = 10;
const SCAN_LINE_SECONDS = 1.4;
const FULL_STRETCH_SPEED = 900;

export const Phase = {
    REST: 'rest',
    PREPARING: 'preparing',
    SCANNING: 'scanning',
    ENROLLING: 'enrolling',
    SUCCESS: 'success',
    FAILURE: 'failure',
};

export const SCAN_COLOR = [0.35, 0.78, 0.98];
const SUCCESS_COLOR = [0.19, 0.82, 0.35];
const FAILURE_COLOR = [1.0, 0.27, 0.23];

const FAILURE_TEXTS = {
    10: ['No face enrolled', 'Add one with sudo howdy add'],
    11: ['Not recognised', 'Use your password'],
    13: ['Too dark', 'Check the IR camera'],
};

const ENROLMENT_FAILURE_TEXTS = {
    'no-face': ['No face found', 'Look straight at the camera'],
    'multiple-faces': ['More than one face', 'Only you, please'],
    'dark': ['Too dark', 'Check the IR camera'],
    'black': ['Camera sees nothing', 'Is the IR emitter on?'],
    'camera-failed': ['Camera error', 'Check the IR camera'],
    'db-failed': ['Could not save the face', 'See journalctl -u gazed'],
};

function clamp(value, low, high) {
    return Math.max(low, Math.min(value, high));
}

/*
 * What the island shows and how far its open/close animation has gone. Times are monotonic seconds;
 * the message methods stamp them with the time of the last advance().
 */
export class Island {
    constructor(now) {
        this.now = now;
        this.phase = Phase.REST;
        this.width = new Spring(REST_WIDTH, WIDTH_STIFFNESS, WIDTH_DAMPING);
        this.height = new Spring(REST_HEIGHT, HEIGHT_STIFFNESS, HEIGHT_DAMPING);
        this.accent = SCAN_COLOR;
        this.title = '';
        this.subtitle = '';
        this.user = '';
        this.preview = null;
        this.faces = [];
        this.threshold = 0;
        this.scanTimeout = 0;
        this.scanStartedAt = 0;
        this.enrolmentProgress = 0;
        this.shakeStartedAt = 0;
        this.lastMessageAt = now;
        this.collapseAt = 0;
    }

    begin(user) {
        this.user = user;
        this.preview = null;
        this.faces = [];
        this.scanStartedAt = 0;
        this.shakeStartedAt = 0;
        this._setPhase(Phase.PREPARING, SCAN_COLOR, 'Opening camera', 'Look at the screen');
        this._expand();
    }

    scan(timeout, threshold) {
        this.scanTimeout = timeout;
        this.threshold = threshold;
        this.scanStartedAt = this.now;
        this._setPhase(Phase.SCANNING, SCAN_COLOR, 'Scanning', 'Looking for your face');
        this._expand();
    }

    enrol() {
        this.enrolmentProgress = 0;
        this._setPhase(Phase.ENROLLING, SCAN_COLOR, 'Adding your face', 'Look straight at the camera');
        this._expand();
    }

    showEnrolmentProgress(fraction) {
        this.enrolmentProgress = clamp(fraction, 0, 1);
        this._touch();
    }

    /* preview: {width, height, gray} with one gray byte per pixel. */
    showFrame(preview) {
        this.preview = preview;
        this._touch();
    }

    /* faces: [{x, y, width, height}] as fractions of the preview. */
    showFaces(faces) {
        this.faces = faces;
        if (this.phase === Phase.SCANNING && faces.length === 0)
            this.subtitle = 'Looking for your face';
        this._touch();
    }

    showCertainty(value) {
        if (this.phase === Phase.SCANNING)
            this.subtitle = `Distance ${value.toFixed(1)} (max ${this.threshold.toFixed(1)})`;
        this._touch();
    }

    /* A line under the title while working, such as "Come closer". */
    showHint(hint) {
        if (this._isWorking() && hint !== '')
            this.subtitle = hint;
        this._touch();
    }

    /* The scan or enrolment stopped without a result (the password was typed instead): close quietly. */
    cancel() {
        if (this._isWorking())
            this._collapse();
    }

    finish(exitCode) {
        if (this.phase === Phase.REST)
            return;

        if (exitCode === 0) {
            this._succeed('Recognised', `Hi, ${this.user}`);
            return;
        }
        const [title, subtitle] = FAILURE_TEXTS[exitCode] ?? ['Howdy error', `Code ${exitCode}`];
        this._fail(title, subtitle);
    }

    /* reason: "added", or why it failed ("no-face", "multiple-faces", "dark", "black", "camera-failed", "db-failed"). */
    finishEnrolment(reason) {
        if (this.phase === Phase.REST)
            return;

        if (reason === 'added') {
            this._succeed('Face added', `Saved for ${this.user}`);
            return;
        }
        const [title, subtitle] = ENROLMENT_FAILURE_TEXTS[reason] ?? ['Could not add the face', reason];
        this._fail(title, subtitle);
    }

    advance(now) {
        const elapsed = now - this.now;
        this.now = now;
        this.width.step(elapsed);
        this.height.step(elapsed);

        const holdOver = this.collapseAt > 0 && now >= this.collapseAt;
        const howdyWentSilent = this._isWorking() && now - this.lastMessageAt > SILENCE_LIMIT_SECONDS;
        if (holdOver || howdyWentSilent)
            this._collapse();
    }

    /* True while open or moving: then it must be on screen and redrawn every frame. */
    isRaised() {
        return this.phase !== Phase.REST || !this.width.isSettled() || !this.height.isSettled();
    }

    /* 0 at rest size, 1 fully open. */
    expansion() {
        return clamp((this.height.value - REST_HEIGHT) / (EXPANDED_HEIGHT - REST_HEIGHT), 0, 1);
    }

    /* How fast it opens (toward 1) or closes (toward -1); 0 when still. */
    stretch() {
        return clamp(this.height.velocity / FULL_STRETCH_SPEED, -1, 1);
    }

    shakeOffset() {
        if (!this._isShaking())
            return 0;
        const since = this.now - this.shakeStartedAt;
        return SHAKE_AMPLITUDE * Math.sin(since * 40) * Math.exp(-since * 6);
    }

    progress() {
        switch (this.phase) {
        case Phase.PREPARING:
            return 0;
        case Phase.ENROLLING:
            return this.enrolmentProgress;
        case Phase.SCANNING:
            if (this.scanTimeout <= 0)
                return 0;
            return clamp((this.now - this.scanStartedAt) / this.scanTimeout, 0, 1);
        default:
            return 1;
        }
    }

    /* Height of the sweeping line as a fraction of the preview; null when not scanning. */
    scanLine() {
        if (this.phase !== Phase.SCANNING && this.phase !== Phase.ENROLLING)
            return null;
        return (this.now % SCAN_LINE_SECONDS) / SCAN_LINE_SECONDS;
    }

    _setPhase(phase, accent, title, subtitle) {
        this.phase = phase;
        this.accent = accent;
        this.title = title;
        this.subtitle = subtitle;
        this._touch();
    }

    _touch() {
        this.lastMessageAt = this.now;
    }

    _expand() {
        this.collapseAt = 0;
        this.width.target = EXPANDED_WIDTH;
        this.width.damping = WIDTH_DAMPING;
        this.height.target = EXPANDED_HEIGHT;
        this.height.damping = HEIGHT_DAMPING;
    }

    _collapse() {
        this.phase = Phase.REST;
        this.collapseAt = 0;
        this.width.target = REST_WIDTH;
        this.width.damping = WIDTH_CLOSING_DAMPING;
        this.height.target = REST_HEIGHT;
        this.height.damping = HEIGHT_CLOSING_DAMPING;
    }

    _succeed(title, subtitle) {
        this._setPhase(Phase.SUCCESS, SUCCESS_COLOR, title, subtitle);
        this.collapseAt = this.now + SUCCESS_HOLD_SECONDS;
    }

    _fail(title, subtitle) {
        this._setPhase(Phase.FAILURE, FAILURE_COLOR, title, subtitle);
        this.shakeStartedAt = this.now;
        this.collapseAt = this.now + FAILURE_HOLD_SECONDS;
    }

    _isWorking() {
        return this.phase === Phase.PREPARING || this.phase === Phase.SCANNING || this.phase === Phase.ENROLLING;
    }

    _isShaking() {
        return this.shakeStartedAt > 0 && this.now - this.shakeStartedAt < SHAKE_SECONDS;
    }
}
