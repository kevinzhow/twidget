// twcard 0.0.0-test protocol 1
// A minimal runtime bundle for the engine's own tests (twcard/tests/engine_tests.cpp): it speaks the engine <-> bundle
// interface (SPEC S1.3a) with just enough of a card world to exercise the engine:
// timers, frames, events handed to a global `onTestEvent`, a title to read back with probe. It is not the card runtime.
(function () {
    'use strict';
    const host = globalThis.host;
    const hostBinary = globalThis.hostBinary;
    // S1.2.2: before anything else, CallSites hand out no functions or receivers.
    {
        const lock = (proto) => {
            if (!proto) return;
            for (const name of ['getFunction', 'getThis']) {
                Object.defineProperty(proto, name, { value() { return undefined; }, writable: false, configurable: false });
            }
        };
        const saved = Error.prepareStackTrace;
        let proto = null;
        Error.prepareStackTrace = (e, sites) => { if (sites.length) proto = Object.getPrototypeOf(sites[0]); return ''; };
        try { new Error().stack; } catch (e) {}
        Error.prepareStackTrace = saved;
        lock(proto);
    }

    let scripts = [];
    let scriptIndex = 0;
    const timers = new Map();
    let nextTimer = 1;
    let rafs = [];

    const report = (e) => host('error', String(e));
    const run = (fn, ...args) => { try { fn(...args); } catch (e) { report(e); } };

    globalThis.document = { title: '', hidden: false };
    globalThis.setTimeout = (fn, delay) => {
        const id = nextTimer++;
        timers.set(id, fn);
        host('timer', JSON.stringify({ id, delay: delay || 0 }));
        return id;
    };
    globalThis.clearTimeout = (id) => { timers.delete(id); host('clearTimer', id); };
    globalThis.requestAnimationFrame = (fn) => { rafs.push(fn); host('raf'); };
    globalThis.performance = { now: () => host('now') };
    globalThis.query = (q) => host('query', JSON.stringify(q));
    globalThis.randomBytes = (n) => host('random', n);
    globalThis.onTestEvent = null;

    globalThis.dispatch = function (kind, arg, arg2) {
        const json = () => JSON.parse(arg);
        switch (kind) {
            case 'boot':
                scripts = json().config.scripts || [];
                return;
            case 'script': {
                if (!scripts.length) return null;
                const s = scripts.shift();
                if (!s.text) return '{}';
                return JSON.stringify({name: 'card-inline-' + (++scriptIndex) + '.js', module: !!s.module, text: s.text});
            }
            case 'scriptDone':
                globalThis.lastScriptResult = arg;
                return;
            case 'scriptError':
                globalThis.lastScriptError = arg;
                report(arg);
                return;
            case 'loaded': document.title += '[loaded]'; return;
            case 'endTick': return 0;
            case 'timer': {
                if (typeof arg !== 'string' || !/^\d+$/.test(arg)) throw new Error('expected decimal timer id');
                const id = Number(arg);
                const fn = timers.get(id);
                timers.delete(id);
                if (typeof fn === 'function') run(fn);
                return;
            }
            case 'frame': {
                const list = rafs;
                rafs = [];
                for (const f of list) run(f, json().time);
                return;
            }
            case 'event': if (onTestEvent) run(onTestEvent, json()); return;
            case 'visibility': document.hidden = !json().visible; document.title += document.hidden ? '[hidden]' : '[shown]'; return;
            case 'rejection':
                globalThis.lastRejectedPromise = arg;
                globalThis.lastRejectionReason = arg2;
                report('unhandled rejection: ' + String(arg2));
                return;
        }
    };
    // A way for test cards to queue a script "inserted" during a task (S1.5).
    globalThis.insertScript = (text) => { scripts.push({ text }); };
})();
