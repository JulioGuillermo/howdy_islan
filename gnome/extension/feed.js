/*
 * Receives Howdy's scan from the howdy-island service, which passes on the messages of scans of this
 * user to a socket in the user's runtime dir. Same messages as the service's own feed: a letter,
 * then its data.
 */
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

export const SOCKET_NAME = 'howdy-island.sock';
const MAX_MESSAGE_BYTES = 65536;
const FRAME_HEADER_BYTES = 4;
const VALUES_PER_FACE = 4;

function parseFaces(text) {
    const values = text.trim().split(/\s+/).filter(value => value !== '').map(Number);
    const faces = [];
    for (let i = 0; i + VALUES_PER_FACE <= values.length; i += VALUES_PER_FACE) {
        const [x, y, width, height] = values.slice(i, i + VALUES_PER_FACE);
        faces.push({x, y, width, height});
    }
    return faces;
}

/* Frame message: width and height as little-endian 16-bit numbers, then one gray byte per pixel. */
function parseFrame(payload) {
    if (payload.length < FRAME_HEADER_BYTES)
        return null;
    const width = payload[0] | payload[1] << 8;
    const height = payload[2] | payload[3] << 8;
    if (width * height !== payload.length - FRAME_HEADER_BYTES)
        return null;
    return {width, height, gray: payload.subarray(FRAME_HEADER_BYTES)};
}

function apply(island, kind, payload) {
    const text = new TextDecoder().decode(payload);
    switch (kind) {
    case 'S':
        island.begin(text.slice(text.indexOf(' ') + 1));
        break;
    case 'R': {
        const [timeout, threshold] = text.split(' ').map(Number);
        island.scan(timeout, threshold);
        break;
    }
    case 'G': {
        const frame = parseFrame(payload);
        if (frame)
            island.showFrame(frame);
        break;
    }
    case 'D':
        island.showFaces(parseFaces(text));
        break;
    case 'C':
        island.showCertainty(Number(text));
        break;
    case 'E':
        island.finish(Number(text));
        break;
    case 'N':
        island.enrol();
        break;
    case 'P':
        island.showEnrolmentProgress(Number(text));
        break;
    case 'A':
        island.finishEnrolment(text);
        break;
    case 'H':
        island.showHint(text);
        break;
    case 'X':
        island.cancel();
        break;
    }
}

export class Feed {
    /* onMessages runs after each batch of messages has been applied to the island. */
    constructor(island, onMessages) {
        this._island = island;
        this._onMessages = onMessages;
        this._path = GLib.build_filenamev([GLib.get_user_runtime_dir(), SOCKET_NAME]);

        this._removeSocketFile();
        this._socket = Gio.Socket.new(Gio.SocketFamily.UNIX, Gio.SocketType.DATAGRAM, Gio.SocketProtocol.DEFAULT);
        this._socket.bind(Gio.UnixSocketAddress.new(this._path), true);
        this._socket.set_blocking(false);

        this._source = this._socket.create_source(GLib.IOCondition.IN, null);
        this._source.set_callback(() => {
            this._receiveAll();
            return GLib.SOURCE_CONTINUE;
        });
        this._source.attach(null);
    }

    destroy() {
        this._source.destroy();
        this._socket.close();
        this._removeSocketFile();
    }

    _receiveAll() {
        let received = 0;
        for (;;) {
            let bytes;
            try {
                bytes = this._socket.receive_bytes(MAX_MESSAGE_BYTES, 0, null);
            } catch (error) {
                if (!error.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.WOULD_BLOCK))
                    logError(error, 'howdy-island: cannot read the feed');
                break;
            }

            const message = bytes.toArray();
            if (message.length === 0)
                continue;
            apply(this._island, String.fromCharCode(message[0]), message.subarray(1));
            received++;
        }

        if (received > 0)
            this._onMessages();
    }

    _removeSocketFile() {
        try {
            Gio.File.new_for_path(this._path).delete(null);
        } catch {
        }
    }
}
