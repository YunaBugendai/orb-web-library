// Orb! Web runtime - the only JavaScript an Orb! browser application needs.
//
//   <script type="module">
//     import { start } from "./orb.js";
//     start("app.wasm");
//   </script>
//
// It loads the .wasm file, provides the host functions that std/web.orb declares (module "orb_web")
// and the process functions of std/os_wasm.orb (module "orb_rt"), and calls the program's `main`.
// All application logic stays in Orb!; this file contains no application logic.
//
// Conventions shared with std/web.orb:
//   * strings cross the boundary as (pointer, length) in UTF-8 and are decoded straight out of
//     linear memory; strings that JS returns are staged in `scratch` and copied once by take();
//   * DOM nodes, events, responses and JSON values are small integer handles
//     (index | generation << 20); a stale or unknown handle yields E_HANDLE, never a crash;
//   * functions return >= 0 on success (a handle, a length, 0/1) or a negative error code;
//   * Orb! callbacks are registered by integer id; JS calls the exported `export_dispatch(id, arg)`.

const E_HANDLE = -1, E_NOT_FOUND = -2, E_INVALID = -3, E_SECURITY = -4, E_TYPE = -5, E_NETWORK = -6, E_UNSUPPORTED = -7;

export class OrbExit extends Error {
  constructor(code) { super(`Orb! program exited with code ${code}`); this.code = code; }
}

export async function start(source, options = {}) {
  const orb = new OrbWeb(options);
  await orb.load(source);
  orb.run();
  return orb;
}

export class OrbWeb {
  constructor(options = {}) {
    this.options = options;
    this.doc = options.document || globalThis.document;
    this.win = options.window || globalThis.window || globalThis;
    this.decoder = new TextDecoder("utf-8");
    this.encoder = new TextEncoder();
    this.exports = null;
    this.memory = null;
    this.exited = null;             // exit code once the program has called exit()
    this.scratch = null;            // bytes staged for take()
    this._view = null;
    this._lineBuf = ["", "", ""];
    // handle table
    this.objs = [undefined]; this.gens = [0]; this.refs = [0]; this.freeList = []; this.byObj = new Map();
    // listeners and timers
    this.listeners = new Map(); this.nextListener = 1;
    this.timers = new Map(); this.nextTimer = 1;
    this.pendingFetches = 0;
    this.log = options.log || ((level, text) => (console[level] || console.log).call(console, text));
  }

  // ------------------------------------------------------------------ loading
  async load(source) {
    const imports = { orb_rt: this._rtImports(), orb_web: this._webImports() };
    Object.assign(imports, this.options.imports || {});
    let result;
    if (source instanceof WebAssembly.Module) result = { instance: await WebAssembly.instantiate(source, imports) };
    else if (source instanceof ArrayBuffer || ArrayBuffer.isView(source)) result = await WebAssembly.instantiate(source, imports);
    else {
      const response = await fetch(source);
      if (!response.ok) throw new Error(`cannot load ${source}: HTTP ${response.status}`);
      try { result = await WebAssembly.instantiateStreaming(response.clone(), imports); }
      catch (_) { result = await WebAssembly.instantiate(await response.arrayBuffer(), imports); }   // wrong MIME type, etc.
    }
    this.instance = result.instance;
    this.exports = this.instance.exports;
    this.memory = this.exports.memory;
  }

  // Runs `main`. A program that registers callbacks stays alive after main returns.
  run() {
    this._guard(() => this.exports._start());
  }

  // Calls an exported Orb! function; restores the shadow stack if it traps.
  _guard(fn) {
    if (this.exited !== null) return undefined;
    const sp = this.exports.__orb_sp.value;
    try { return fn(); }
    catch (e) {
      if (e instanceof OrbExit) { this.exited = e.code; this._flushLines(); if (this.options.onexit) this.options.onexit(e.code); return undefined; }
      this.exports.__orb_sp.value = sp;
      this._flushLines();
      this.log("error", `Orb! runtime error: ${e && e.message ? e.message : e}`);
      if (this.options.onerror) this.options.onerror(e);
      return undefined;
    }
  }

  _dispatch(cbid, arg) {
    this._guard(() => { this.exports.export_dispatch(cbid, arg); this.exports.export_flush(); });
  }

  stats() {
    let live = 0; for (let i = 1; i < this.objs.length; i++) if (this.objs[i] !== undefined) live++;
    return { handles: live, listeners: this.listeners.size, timers: this.timers.size, fetches: this.pendingFetches,
             memoryBytes: this.memory ? this.memory.buffer.byteLength : 0, exited: this.exited };
  }

  // ------------------------------------------------------------------ memory helpers
  mem() {
    if (!this._view || this._view.buffer !== this.memory.buffer) this._view = new Uint8Array(this.memory.buffer);   // memory.grow detaches old views
    return this._view;
  }
  str(p, n) { p >>>= 0; n >>>= 0; return this.decoder.decode(this.mem().subarray(p, p + n)); }
  stage(s) { this.scratch = this.encoder.encode(s); return this.scratch.length; }
  writeF64(ptr, v) { new DataView(this.memory.buffer).setFloat64(ptr >>> 0, v, true); }

  // ------------------------------------------------------------------ handles
  put(obj, intern) {
    if (obj === null || obj === undefined) return E_NOT_FOUND;
    if (intern) { const i = this.byObj.get(obj); if (i !== undefined) { this.refs[i]++; return i | (this.gens[i] << 20); } }
    let i;
    if (this.freeList.length) i = this.freeList.pop();
    else { i = this.objs.length; this.objs.push(undefined); this.gens.push(1); this.refs.push(0); if (i >= (1 << 20)) throw new Error("too many live handles"); }
    this.objs[i] = obj; this.refs[i] = 1;
    if (intern) this.byObj.set(obj, i);
    return i | (this.gens[i] << 20);
  }
  get(h) {
    const i = h & 0xFFFFF, g = h >>> 20;
    if (i <= 0 || i >= this.objs.length || this.gens[i] !== g || this.objs[i] === undefined) return undefined;
    return this.objs[i];
  }
  drop(h) {
    const i = h & 0xFFFFF, g = h >>> 20;
    if (i <= 0 || i >= this.objs.length || this.gens[i] !== g || this.objs[i] === undefined) return E_HANDLE;
    if (--this.refs[i] > 0) return 0;
    this.byObj.delete(this.objs[i]);
    this.objs[i] = undefined;
    this.gens[i] = (this.gens[i] % 2047) + 1;
    this.freeList.push(i);
    return 0;
  }
  _transient(obj, fn) {      // handle that is valid only while fn runs (events, responses)
    const h = this.put(obj, false);
    try { return fn(h); } finally { this.drop(h); }
  }

  // ------------------------------------------------------------------ errors
  _err(e) {
    const name = e && e.name;
    if (name === "SecurityError" || name === "QuotaExceededError" || name === "NotAllowedError") return E_SECURITY;
    if (name === "TypeError" && e.message && /fetch|network/i.test(e.message)) return E_NETWORK;
    return E_INVALID;
  }
  _try(fn) { try { return fn(); } catch (e) { return this._err(e); } }

  // ------------------------------------------------------------------ orb_rt (process)
  _lineOut(fd, text) {
    const level = fd === 2 ? "error" : "log";
    const custom = fd === 2 ? this.options.stderr : this.options.stdout;
    if (custom) { custom(text); return; }
    this._lineBuf[fd] += text;
    let nl;
    while ((nl = this._lineBuf[fd].indexOf("\n")) >= 0) {
      this.log(level, this._lineBuf[fd].slice(0, nl));
      this._lineBuf[fd] = this._lineBuf[fd].slice(nl + 1);
    }
  }
  _flushLines() {
    for (const fd of [1, 2]) if (this._lineBuf[fd]) { this.log(fd === 2 ? "error" : "log", this._lineBuf[fd]); this._lineBuf[fd] = ""; }
  }
  _rtImports() {
    return {
      write: (fd, p, n) => { this._lineOut(fd, this.str(p, n)); return n; },
      read: () => 0,
      exit: (code) => { throw new OrbExit(code); },
    };
  }

  // ------------------------------------------------------------------ orb_web (browser APIs)
  _webImports() {
    const self = this;
    const node = (h) => self.get(h);
    const withNode = (h, f) => { const o = self.get(h); if (o === undefined) return E_HANDLE; return self._try(() => f(o)); };
    const storageOf = (kind) => { try { return kind === 0 ? self.win.localStorage : self.win.sessionStorage; } catch (_) { return null; } };
    const jsonKind = (v) => v === null ? 0 : typeof v === "boolean" ? 1 : typeof v === "number" ? 2 : typeof v === "string" ? 3 : Array.isArray(v) ? 4 : 5;
    const sstr = (p, n) => self.str(p, n);

    return {
      // ---- handles
      release: (h) => self.drop(h),

      // ---- DOM lookup / creation
      get_element: (p, n) => self.put(self.doc.getElementById(sstr(p, n)), true),
      query: (p, n) => self._try(() => self.put(self.doc.querySelector(sstr(p, n)), true)),
      query_all: (p, n) => self._try(() => { self.nodeList = Array.from(self.doc.querySelectorAll(sstr(p, n))); return self.nodeList.length; }),
      take_nodes: (dst, count) => {
        const list = self.nodeList || []; self.nodeList = null;
        const out = new Int32Array(self.memory.buffer, dst >>> 0, count);
        for (let i = 0; i < count && i < list.length; i++) out[i] = self.put(list[i], true);
        return 0;
      },
      body: () => self.put(self.doc.body, true),
      document: () => self.put(self.doc, true),
      window: () => self.put(self.win, true),
      create: (p, n) => self._try(() => self.put(self.doc.createElement(sstr(p, n)), true)),
      create_text: (p, n) => self.put(self.doc.createTextNode(sstr(p, n)), true),
      append: (parent, child) => { const c = node(child); return c === undefined ? E_HANDLE : withNode(parent, (o) => { o.appendChild(c); return 0; }); },
      remove: (h) => withNode(h, (o) => { o.remove(); return 0; }),
      clear: (h) => withNode(h, (o) => { o.replaceChildren(); return 0; }),
      focus: (h) => withNode(h, (o) => { o.focus(); return 0; }),

      // ---- text / html / attributes / classes / properties / style
      get_text: (h) => withNode(h, (o) => self.stage(o.textContent ?? "")),
      set_text: (h, p, n) => withNode(h, (o) => { o.textContent = sstr(p, n); return 0; }),
      get_html: (h) => withNode(h, (o) => self.stage(o.innerHTML ?? "")),
      set_html: (h, p, n) => withNode(h, (o) => { o.innerHTML = sstr(p, n); return 0; }),
      get_attr: (h, p, n) => withNode(h, (o) => { const v = o.getAttribute(sstr(p, n)); return v === null ? E_NOT_FOUND : self.stage(v); }),
      set_attr: (h, p, n, vp, vn) => withNode(h, (o) => { o.setAttribute(sstr(p, n), sstr(vp, vn)); return 0; }),
      remove_attr: (h, p, n) => withNode(h, (o) => { o.removeAttribute(sstr(p, n)); return 0; }),
      has_attr: (h, p, n) => withNode(h, (o) => (o.hasAttribute(sstr(p, n)) ? 1 : 0)),
      class_op: (h, op, p, n) => withNode(h, (o) => {
        const c = sstr(p, n);
        if (/\s/.test(c) || c === "") return E_INVALID;
        if (op === 0) { o.classList.add(c); return 1; }
        if (op === 1) { o.classList.remove(c); return 0; }
        if (op === 2) return o.classList.toggle(c) ? 1 : 0;
        return o.classList.contains(c) ? 1 : 0;
      }),
      get_prop_str: (h, p, n) => withNode(h, (o) => { const v = o[sstr(p, n)]; return v === undefined ? E_NOT_FOUND : typeof v === "string" ? self.stage(v) : E_TYPE; }),
      set_prop_str: (h, p, n, vp, vn) => withNode(h, (o) => { o[sstr(p, n)] = sstr(vp, vn); return 0; }),
      get_prop_num: (h, p, n, out) => withNode(h, (o) => { const v = o[sstr(p, n)]; if (typeof v !== "number") return v === undefined ? E_NOT_FOUND : E_TYPE; self.writeF64(out, v); return 0; }),
      set_prop_num: (h, p, n, x) => withNode(h, (o) => { o[sstr(p, n)] = x; return 0; }),
      get_prop_bool: (h, p, n) => withNode(h, (o) => { const v = o[sstr(p, n)]; return typeof v === "boolean" ? (v ? 1 : 0) : v === undefined ? E_NOT_FOUND : E_TYPE; }),
      set_prop_bool: (h, p, n, v) => withNode(h, (o) => { o[sstr(p, n)] = v !== 0; return 0; }),
      set_style: (h, p, n, vp, vn) => withNode(h, (o) => { o.style.setProperty(sstr(p, n), sstr(vp, vn)); return 0; }),

      // ---- events
      listen: (h, p, n, cbid) => {
        const t = node(h);
        if (t === undefined || typeof t.addEventListener !== "function") return E_HANDLE;
        const type = sstr(p, n);
        const fn = (ev) => { if (self.exited === null) self._transient(ev, (eh) => self._dispatch(cbid, eh)); };
        t.addEventListener(type, fn);
        const id = self.nextListener++;
        self.listeners.set(id, { t, type, fn });
        return id;
      },
      unlisten: (id) => {
        const l = self.listeners.get(id);
        if (!l) return E_HANDLE;
        l.t.removeEventListener(l.type, l.fn); self.listeners.delete(id);
        return 0;
      },
      ev_str: (h, field) => { const e = node(h); if (e === undefined) return E_HANDLE;
        const v = field === 0 ? e.type : field === 1 ? e.key : field === 2 ? e.code : field === 3 ? (e.target && e.target.value) : undefined;
        return v === undefined || v === null ? E_NOT_FOUND : self.stage(String(v)); },
      ev_int: (h, field) => { const e = node(h); if (e === undefined) return E_HANDLE;
        switch (field) {
          case 0: return (e.ctrlKey ? 1 : 0) | (e.shiftKey ? 2 : 0) | (e.altKey ? 4 : 0) | (e.metaKey ? 8 : 0) | (e.repeat ? 16 : 0);
          case 1: return e.button ?? 0;
          case 2: return Math.trunc(e.clientX ?? 0);
          case 3: return Math.trunc(e.clientY ?? 0);
          case 4: return e.defaultPrevented ? 1 : 0;
          default: return E_INVALID;
        } },
      ev_target: (h) => { const e = node(h); return e === undefined ? E_HANDLE : self.put(e.target, true); },
      ev_prevent: (h) => { const e = node(h); if (e === undefined) return E_HANDLE; e.preventDefault(); return 0; },
      ev_stop: (h) => { const e = node(h); if (e === undefined) return E_HANDLE; e.stopPropagation(); return 0; },
      dispatch_event: (h, p, n) => withNode(h, (o) => { o.dispatchEvent(new self.win.Event(sstr(p, n), { bubbles: true, cancelable: true })); return 0; }),

      // ---- timers
      set_timer: (ms, repeat, cbid) => {
        const id = self.nextTimer++;
        const fire = () => { if (!repeat) self.timers.delete(id); self._dispatch(cbid, 0); };
        const handle = repeat ? self.win.setInterval(fire, ms) : self.win.setTimeout(fire, ms);
        self.timers.set(id, { handle, repeat: repeat !== 0 });
        return id;
      },
      clear_timer: (id) => {
        const t = self.timers.get(id);
        if (!t) return E_HANDLE;
        (t.repeat ? self.win.clearInterval : self.win.clearTimeout).call(self.win, t.handle);
        self.timers.delete(id);
        return 0;
      },
      now: () => (self.win.performance ? self.win.performance.now() : Date.now()),
      random: () => Math.random(),

      // ---- console
      console: (level, p, n) => { const text = sstr(p, n); self.log(["log", "info", "warn", "error", "debug"][level] || "log", text); return 0; },

      // ---- storage (kind 0 = localStorage, 1 = sessionStorage)
      store_get: (kind, p, n) => { const s = storageOf(kind); if (!s) return E_SECURITY;
        return self._try(() => { const v = s.getItem(sstr(p, n)); return v === null ? E_NOT_FOUND : self.stage(v); }); },
      store_set: (kind, p, n, vp, vn) => { const s = storageOf(kind); if (!s) return E_SECURITY;
        return self._try(() => { s.setItem(sstr(p, n), sstr(vp, vn)); return 0; }); },
      store_remove: (kind, p, n) => { const s = storageOf(kind); if (!s) return E_SECURITY; return self._try(() => { s.removeItem(sstr(p, n)); return 0; }); },
      store_clear: (kind) => { const s = storageOf(kind); if (!s) return E_SECURITY; return self._try(() => { s.clear(); return 0; }); },
      store_len: (kind) => { const s = storageOf(kind); if (!s) return E_SECURITY; return self._try(() => s.length); },
      store_key: (kind, i) => { const s = storageOf(kind); if (!s) return E_SECURITY; return self._try(() => { const k = s.key(i); return k === null ? E_NOT_FOUND : self.stage(k); }); },

      // ---- location
      loc_get: (which) => { const l = self.win.location; const v = [l.href, l.origin, l.protocol, l.host, l.hostname, l.pathname, l.search, l.hash][which];
        return v === undefined ? E_INVALID : self.stage(v); },
      loc_param: (p, n) => self._try(() => { const v = new URLSearchParams(self.win.location.search).get(sstr(p, n)); return v === null ? E_NOT_FOUND : self.stage(v); }),
      loc_set_hash: (p, n) => self._try(() => { self.win.location.hash = sstr(p, n); return 0; }),
      loc_navigate: (p, n) => self._try(() => { self.win.location.assign(sstr(p, n)); return 0; }),
      loc_reload: () => self._try(() => { self.win.location.reload(); return 0; }),

      // ---- fetch (asynchronous: the Orb! callback runs later with a Response handle)
      fetch: (up, un, mp, mn, bp, bn, cp, cn, cbid) => {
        let url, init;
        try {
          url = new URL(sstr(up, un), self.win.location.href).href;
          init = { method: mn ? sstr(mp, mn) : "GET" };
          if (bn) init.body = sstr(bp, bn);
          if (cn) init.headers = { "Content-Type": sstr(cp, cn) };
        } catch (_) { return E_INVALID; }
        self.pendingFetches++;
        const finish = (res) => { self.pendingFetches--; self._transient(res, (rh) => self._dispatch(cbid, rh)); };
        (self.options.fetch || self.win.fetch).call(self.win, url, init)
          .then(async (r) => { const buf = new Uint8Array(await r.arrayBuffer()); finish({ ok: r.ok, status: r.status, headers: r.headers, body: buf, failed: false }); })
          .catch(() => finish({ ok: false, status: 0, headers: null, body: new Uint8Array(0), failed: true }));
        return 0;
      },
      res_info: (h, field) => { const r = node(h); if (r === undefined) return E_HANDLE;
        return field === 0 ? r.status : field === 1 ? (r.ok ? 1 : 0) : field === 2 ? r.body.length : field === 3 ? (r.failed ? 1 : 0) : E_INVALID; },
      res_read: (h, dst, cap) => { const r = node(h); if (r === undefined) return E_HANDLE;
        const n = Math.min(cap >>> 0, r.body.length); self.mem().set(r.body.subarray(0, n), dst >>> 0); return n; },     // one copy: response -> linear memory
      res_header: (h, p, n) => { const r = node(h); if (r === undefined) return E_HANDLE; const v = r.headers && r.headers.get(sstr(p, n)); return v === null || v === undefined ? E_NOT_FOUND : self.stage(v); },

      // ---- JSON values (kept on the JS side; Orb! reads and builds them through handles)
      json_parse: (p, n) => { try { return self.put({ v: JSON.parse(sstr(p, n)) }, false); } catch (_) { return E_INVALID; } },
      json_kind: (h) => { const j = node(h); return j === undefined ? E_HANDLE : jsonKind(j.v); },
      json_len: (h) => { const j = node(h); if (j === undefined) return E_HANDLE; return Array.isArray(j.v) ? j.v.length : j.v && typeof j.v === "object" ? Object.keys(j.v).length : E_TYPE; },
      json_get: (h, p, n) => { const j = node(h); if (j === undefined) return E_HANDLE; if (jsonKind(j.v) !== 5) return E_TYPE;
        const k = sstr(p, n); return Object.prototype.hasOwnProperty.call(j.v, k) ? self.put({ v: j.v[k] }, false) : E_NOT_FOUND; },
      json_at: (h, i) => { const j = node(h); if (j === undefined) return E_HANDLE; if (!Array.isArray(j.v)) return E_TYPE;
        return i >= 0 && i < j.v.length ? self.put({ v: j.v[i] }, false) : E_NOT_FOUND; },
      json_key_at: (h, i) => { const j = node(h); if (j === undefined) return E_HANDLE; if (jsonKind(j.v) !== 5) return E_TYPE;
        const ks = Object.keys(j.v); return i >= 0 && i < ks.length ? self.stage(ks[i]) : E_NOT_FOUND; },
      json_str: (h) => { const j = node(h); if (j === undefined) return E_HANDLE; return typeof j.v === "string" ? self.stage(j.v) : E_TYPE; },
      json_num: (h, out) => { const j = node(h); if (j === undefined) return E_HANDLE; if (typeof j.v !== "number") return E_TYPE; self.writeF64(out, j.v); return 0; },
      json_bool: (h) => { const j = node(h); if (j === undefined) return E_HANDLE; return typeof j.v === "boolean" ? (j.v ? 1 : 0) : E_TYPE; },
      json_stringify: (h) => { const j = node(h); if (j === undefined) return E_HANDLE; return self._try(() => self.stage(JSON.stringify(j.v))); },
      json_new: (kind) => self.put({ v: kind === 4 ? [] : kind === 5 ? {} : null }, false),
      json_from_num: (x) => self.put({ v: x }, false),
      json_from_str: (p, n) => self.put({ v: sstr(p, n) }, false),
      json_from_bool: (b) => self.put({ v: b !== 0 }, false),
      json_set: (h, p, n, child) => { const j = node(h), c = node(child); if (j === undefined || c === undefined) return E_HANDLE;
        if (jsonKind(j.v) !== 5) return E_TYPE; j.v[sstr(p, n)] = c.v; return 0; },
      json_push: (h, child) => { const j = node(h), c = node(child); if (j === undefined || c === undefined) return E_HANDLE;
        if (!Array.isArray(j.v)) return E_TYPE; j.v.push(c.v); return 0; },

      // ---- staged results
      take: (dst, n) => { const s = self.scratch; self.scratch = null; if (!s || n > s.length) return E_INVALID; self.mem().set(s.subarray(0, n), dst >>> 0); return 0; },
    };
  }
}
