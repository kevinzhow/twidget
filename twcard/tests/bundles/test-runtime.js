// twcard 0.0.0-test protocol 1
// A minimal runtime bundle for the engine's own tests (twcard/tests/engine_tests.cpp): it speaks the engine <-> bundle
// interface (QUESTIONS Q3 / twcard/src/engine_protocol.h) with just enough of a "card world" to exercise the engine:
// timers, frames, events handed to a global `onTestEvent`, a title to read back with probe. It is not the card runtime.
(function () {
    'use strict';
    const host = globalThis.__twcard_host;
    const hostBinary = globalThis.__twcard_hostBinary;
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
    let pendingModule = '';
    const timers = new Map();
    let nextTimer = 1;
    let rafs = [];

    const report = (e) => host('diag', 'uncaught:' + String(e && e.message !== undefined ? e.message : e));
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

    globalThis.__twcard_dispatch = function (kind, arg) {
        const a = arg === undefined ? undefined : JSON.parse(arg);
        switch (kind) {
            case 'boot':
                scripts = (a.config && a.config.scripts) || [];
                return;
            case 'script': {
                if (!scripts.length) return undefined;
                const s = scripts.shift();
                if (s.module) { pendingModule = s.text; return 'module'; }
                try { (0, eval)(s.text); } catch (e) { report(e); }
                return 'run';
            }
            case 'moduleSource': return pendingModule;
            case 'moduleDone': if (a.error) report(a.error); return;
            case 'loaded': document.title += '[loaded]'; return;
            case 'endTick': return undefined;
            case 'timer': {
                const fn = timers.get(a.id);
                timers.delete(a.id);
                if (typeof fn === 'function') run(fn);
                return;
            }
            case 'frame': {
                const list = rafs;
                rafs = [];
                for (const f of list) run(f, a.time);
                return;
            }
            case 'event': if (onTestEvent) run(onTestEvent, a); return;
            case 'visibility': document.hidden = !a.visible; document.title += a.visible ? '[shown]' : '[hidden]'; return;
            case 'rejection': report('unhandled rejection: ' + a.message); return;
            case 'insertScript': return;
        }
    };
    // A way for test cards to queue a script "inserted" during a task (S1.5).
    globalThis.insertScript = (text) => { scripts.push({ text }); };
})();
