/* Пролог JavaScript TSafari: DOM и window, какими их видят скрипты страницы.
 *
 * Выполняется первым на каждой странице (js.c). Получает N — функции на C (js.c) над деревом
 * dom.c: узел для них — номер. Здесь каждому узлу, которого коснулся скрипт, заводится объект
 * (один на узел, в массиве W по номеру), его класс — по виду узла и тегу (HTMLElement,
 * HTMLInputElement, Text…). Возвращает хуки для js.c: начало и конец скрипта (document.currentScript,
 * document.write), DOMContentLoaded, load и шаг таймеров.
 *
 * Время у таймеров — своё («виртуальное»): setTimeout(f, 1000) выполнится, когда до него дойдёт
 * очередь, а не через секунду — страницу ведь показывают один раз, ждать незачем. Размеры
 * элементов (offsetWidth, getBoundingClientRect) — приблизительные: раскладка у браузера потом,
 * в html.c. Сеть (fetch, XMLHttpRequest) — пока ошибка. */
(function (N) {
'use strict';
const G = globalThis;
const W = [];                                   /* объекты узлов по номеру */
let vnow = 0;                                   /* виртуальное время таймеров, мс */
let current = 0;                                /* выполняемый <script> */
let writeAt = null;                             /* куда пишет document.write: [родитель, перед кем] */
let readyState = 'loading';
const DOCW = 1008, DOCH = 768;                  /* место страницы на экране */

function def(o, name, get, set) {
    Object.defineProperty(o, name, {get: get, set: set, configurable: true, enumerable: false});
}
function hide(o, name, value) {
    Object.defineProperty(o, name, {value: value, writable: true, configurable: true, enumerable: false});
}
function camel2kebab(s) { return s.replace(/[A-Z]/g, c => '-' + c.toLowerCase()).replace(/^(webkit|moz|ms)-/, '-$1-'); }
function kebab2camel(s) { return s.replace(/-([a-z])/g, (m, c) => c.toUpperCase()); }

/* --- события ------------------------------------------------------------------------------ */
class Event {
    constructor(type, init) {
        init = init || {};
        this.type = String(type);
        this.bubbles = !!init.bubbles;
        this.cancelable = !!init.cancelable;
        this.composed = !!init.composed;
        this.defaultPrevented = false;
        this.isTrusted = false;
        this.timeStamp = vnow;
        this.target = null;
        this.currentTarget = null;
        this.eventPhase = 0;
        hide(this, '_stop', false);
        hide(this, '_now', false);
    }
    preventDefault() { if (this.cancelable) this.defaultPrevented = true; }
    stopPropagation() { this._stop = true; }
    stopImmediatePropagation() { this._stop = this._now = true; }
    composedPath() { return this._path ? this._path.slice() : []; }
    get returnValue() { return !this.defaultPrevented; }
    set returnValue(v) { if (!v) this.preventDefault(); }
    get srcElement() { return this.target; }
    initEvent(type, bubbles, cancelable) { this.type = String(type); this.bubbles = !!bubbles; this.cancelable = !!cancelable; }
}
Event.NONE = 0; Event.CAPTURING_PHASE = 1; Event.AT_TARGET = 2; Event.BUBBLING_PHASE = 3;
class CustomEvent extends Event {
    constructor(type, init) { super(type, init); this.detail = init && init.detail !== undefined ? init.detail : null; }
    initCustomEvent(type, b, c, detail) { this.initEvent(type, b, c); this.detail = detail; }
}
class UIEvent extends Event { constructor(t, i) { super(t, i); this.view = (i && i.view) || null; this.detail = (i && i.detail) || 0; } }
class MouseEvent extends UIEvent {
    constructor(t, i) {
        super(t, i); i = i || {};
        for (const k of ['screenX', 'screenY', 'clientX', 'clientY', 'pageX', 'pageY', 'offsetX', 'offsetY', 'button', 'buttons'])
            this[k] = i[k] || 0;
        for (const k of ['ctrlKey', 'shiftKey', 'altKey', 'metaKey']) this[k] = !!i[k];
        this.relatedTarget = i.relatedTarget || null;
    }
}
class KeyboardEvent extends UIEvent {
    constructor(t, i) {
        super(t, i); i = i || {};
        this.key = i.key || ''; this.code = i.code || ''; this.keyCode = i.keyCode || 0; this.which = this.keyCode;
        for (const k of ['ctrlKey', 'shiftKey', 'altKey', 'metaKey', 'repeat']) this[k] = !!i[k];
    }
}
class FocusEvent extends UIEvent {}
class InputEvent extends UIEvent { constructor(t, i) { super(t, i); this.data = (i && i.data) || null; } }
class PointerEvent extends MouseEvent {}
class WheelEvent extends MouseEvent {}
class ErrorEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.message = i.message || ''; this.error = i.error || null; } }
class ProgressEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.loaded = i.loaded || 0; this.total = i.total || 0; this.lengthComputable = !!i.lengthComputable; } }
class MessageEvent extends Event { constructor(t, i) { super(t, i); this.data = i ? i.data : undefined; this.origin = ''; } }
class PopStateEvent extends Event { constructor(t, i) { super(t, i); this.state = i ? i.state : null; } }
class HashChangeEvent extends Event {}
class AnimationEvent extends Event {}
class TransitionEvent extends Event {}
class StorageEvent extends Event {}

/* Обработчики: у объекта — скрытая таблица «тип → [{f, capture, once}]» */
function listeners(t, create) {
    if (!Object.prototype.hasOwnProperty.call(t, '_l')) { if (!create) return null; hide(t, '_l', Object.create(null)); }
    return t._l;
}
const ET = {
    addEventListener(type, f, opt) {
        if (!f) return;
        const capture = typeof opt === 'boolean' ? opt : !!(opt && opt.capture);
        const l = listeners(this, true), a = l[type] || (l[type] = []);
        for (const x of a) if (x.f === f && x.capture === capture) return;
        a.push({f: f, capture: capture, once: !!(opt && opt.once)});
    },
    removeEventListener(type, f, opt) {
        const l = listeners(this, false);
        if (!l || !l[type]) return;
        const capture = typeof opt === 'boolean' ? opt : !!(opt && opt.capture);
        l[type] = l[type].filter(x => !(x.f === f && x.capture === capture));
    },
    dispatchEvent(ev) {
        ev.target = this;
        const path = [];
        for (let t = this; t; t = parentTarget(t)) path.push(t);
        hide(ev, '_path', path);
        const call = (t, phase) => {
            const l = listeners(t, false);
            ev.currentTarget = t;
            ev.eventPhase = phase;
            if (l && l[ev.type]) {
                for (const x of l[ev.type].slice()) {
                    if (phase === 1 && !x.capture) continue;
                    if (phase === 3 && x.capture) continue;
                    if (x.once) this.removeEventListener.call(t, ev.type, x.f, x.capture);
                    try {
                        if (typeof x.f === 'function') x.f.call(t, ev);
                        else if (x.f && typeof x.f.handleEvent === 'function') x.f.handleEvent(ev);
                    } catch (e) { reportError(e); }
                    if (ev._now) return;
                }
            }
            if (phase !== 1) {
                let h = t['on' + ev.type];
                if (typeof h !== 'function' && t._i && N.type(t._i) === 1) h = attrHandler(t, ev.type);
                if (typeof h === 'function') {
                    try { if (h.call(t, ev) === false) ev.preventDefault(); } catch (e) { reportError(e); }
                }
            }
        };
        for (let i = path.length - 1; i > 0 && !ev._stop; i--) call(path[i], 1);
        if (!ev._stop) call(this, 2);
        if (ev.bubbles) for (let i = 1; i < path.length && !ev._stop; i++) call(path[i], 3);
        ev.currentTarget = null;
        ev.eventPhase = 0;
        return !ev.defaultPrevented;
    }
};
/* onclick="…" в разметке — функция (один раз на элемент и тип) */
function attrHandler(t, type) {
    const code = N.getAttr(t._i, 'on' + type);
    if (code === null) return null;
    const cache = Object.prototype.hasOwnProperty.call(t, '_h') ? t._h : (hide(t, '_h', {}), t._h);
    if (!(type in cache)) {
        try { cache[type] = new Function('event', code); } catch (e) { reportError(e); cache[type] = null; }
    }
    return cache[type];
}
function parentTarget(t) {
    if (t === G) return null;
    if (t === document) return G;
    if (!t._i) return null;
    const p = N.parent(t._i);
    return p ? wrap(p) : null;
}
class EventTarget {}
Object.assign(EventTarget.prototype, ET);

function reportError(e) {
    N.log(2, (e && e.stack) ? String(e) + ' ' + String(e.stack).split('\n')[0] : String(e));
    try { G.dispatchEvent(new ErrorEvent('error', {message: String(e), error: e})); } catch (x) {}
}

/* --- узлы ----------------------------------------------------------------------------------- */
function wrap(id) {
    if (!id) return null;
    let o = W[id];
    if (o) return o;
    const t = N.type(id);
    let p;
    if (t === 1) p = (TAGS[N.tag(id)] || HTMLElement).prototype;
    else if (t === 3) p = Text.prototype;
    else if (t === 8) p = Comment.prototype;
    else if (t === 9) p = Document.prototype;
    else if (t === 11) p = DocumentFragment.prototype;
    else if (t === 10) p = DocumentType.prototype;
    else p = Node.prototype;
    o = Object.create(p);
    hide(o, '_i', id);
    W[id] = o;
    return o;
}
function idOf(n) {
    if (n && typeof n === 'object' && n._i) return n._i;
    throw new TypeError('parameter is not of type Node');
}
function list(ids) {
    const a = ids.map(wrap);
    hide(a, 'item', i => a[i] || null);
    return a;
}
function nodesOf(args) {                         /* append(…) и подобные: строки — текстовые узлы */
    const f = N.frag();
    for (const a of args) N.insert(f, typeof a === 'object' && a !== null ? idOf(a) : N.text(String(a), false), 0);
    return f;
}
/* Что окажется в дереве после вставки: узел или дети фрагмента */
function inserted(id) { return N.type(id) === 11 ? N.kids(id) : [id]; }
/* Вставленные в документ <script> выполняются сразу (как у браузеров) */
function runInserted(id) {
    if (!N.contains(1, id)) return;
    const todo = N.type(id) === 1 && N.tag(id) === 'script' ? [id] : N.find(id, 'script', false);
    for (const s of todo) {
        const o = wrap(s);
        if (o._started) continue;
        hide(o, '_started', true);
        const type = (N.getAttr(s, 'type') || '').toLowerCase();
        if (type && !/^(text|application)\/(x-)?(java|ecma)script$|^module$/.test(type)) continue;
        const src = N.getAttr(s, 'src');
        if (src) {                               /* внешний — загрузить пока не умеем: ошибка */
            N.log(1, 'script not loaded (dynamic): ' + N.resolve(src));
            setTimeout(() => o.dispatchEvent(new Event('error')), 0);
            continue;
        }
        const was = current;
        current = s;
        try { (0, eval)(N.textContent(s)); } catch (e) { reportError(e); }
        current = was;
    }
}

class Node extends EventTarget {
    constructor() { super(); throw new TypeError('Illegal constructor'); }
    get nodeType() { return N.type(this._i); }
    get nodeName() {
        const t = N.type(this._i);
        return t === 1 ? this.tagName : t === 3 ? '#text' : t === 8 ? '#comment' : t === 9 ? '#document'
             : t === 11 ? '#document-fragment' : t === 10 ? 'html' : '';
    }
    get nodeValue() { const t = N.type(this._i); return t === 3 || t === 8 ? N.data(this._i) : null; }
    set nodeValue(v) { const t = N.type(this._i); if (t === 3 || t === 8) N.setData(this._i, String(v)); }
    get parentNode() { return wrap(N.parent(this._i)); }
    get parentElement() { const p = N.parent(this._i); return p && N.type(p) === 1 ? wrap(p) : null; }
    get firstChild() { return wrap(N.first(this._i)); }
    get lastChild() { return wrap(N.last(this._i)); }
    get nextSibling() { return wrap(N.next(this._i)); }
    get previousSibling() { return wrap(N.prev(this._i)); }
    get childNodes() { return list(N.kids(this._i)); }
    get ownerDocument() { return this._i === 1 ? null : document; }
    get isConnected() { return N.contains(1, this._i); }
    get textContent() { const t = N.type(this._i); return t === 9 ? null : t === 3 || t === 8 ? N.data(this._i) : N.textContent(this._i); }
    set textContent(v) {
        const t = N.type(this._i);
        if (t === 3 || t === 8) { N.setData(this._i, String(v)); return; }
        for (let k = N.first(this._i); k; k = N.first(this._i)) N.detach(k);
        if (v !== null && v !== undefined && String(v) !== '') N.insert(this._i, N.text(String(v), false), 0);
    }
    hasChildNodes() { return !!N.first(this._i); }
    getRootNode() { let n = this._i; while (N.parent(n)) n = N.parent(n); return wrap(n); }
    appendChild(c) {
        const ids = inserted(idOf(c));
        if (!N.insert(this._i, c._i, 0)) throw new DOMException('The new child element contains the parent.', 'HierarchyRequestError');
        ids.forEach(runInserted);
        return c;
    }
    insertBefore(c, ref) {
        const ids = inserted(idOf(c));
        if (!N.insert(this._i, c._i, ref ? idOf(ref) : 0)) throw new DOMException('Failed to insert', 'HierarchyRequestError');
        ids.forEach(runInserted);
        return c;
    }
    removeChild(c) {
        if (N.parent(idOf(c)) !== this._i) throw new DOMException('The node to be removed is not a child of this node.', 'NotFoundError');
        N.detach(c._i);
        return c;
    }
    replaceChild(nw, old) { this.insertBefore(nw, old); if (nw !== old) this.removeChild(old); return old; }
    cloneNode(deep) { return wrap(N.clone(this._i, !!deep)); }
    contains(o) { return !!o && !!o._i && N.contains(this._i, o._i); }
    compareDocumentPosition(o) {
        if (o === this) return 0;
        const r = N.order(this._i, idOf(o));
        if (!r) return 1 | 32;                   /* разные деревья */
        if (N.contains(this._i, o._i)) return 4 | 16;
        if (N.contains(o._i, this._i)) return 2 | 8;
        return r < 0 ? 4 : 2;
    }
    isSameNode(o) { return o === this; }
    isEqualNode(o) { return !!o && o._i && N.serialize(this._i, true) === N.serialize(o._i, true); }
    normalize() {}
    lookupNamespaceURI() { return null; }
}
Object.assign(Node, {ELEMENT_NODE: 1, ATTRIBUTE_NODE: 2, TEXT_NODE: 3, CDATA_SECTION_NODE: 4, COMMENT_NODE: 8,
    DOCUMENT_NODE: 9, DOCUMENT_TYPE_NODE: 10, DOCUMENT_FRAGMENT_NODE: 11, DOCUMENT_POSITION_DISCONNECTED: 1,
    DOCUMENT_POSITION_PRECEDING: 2, DOCUMENT_POSITION_FOLLOWING: 4, DOCUMENT_POSITION_CONTAINS: 8,
    DOCUMENT_POSITION_CONTAINED_BY: 16, DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC: 32});
Object.assign(Node.prototype, {ELEMENT_NODE: 1, TEXT_NODE: 3, COMMENT_NODE: 8, DOCUMENT_NODE: 9, DOCUMENT_FRAGMENT_NODE: 11});

class DOMException extends Error {
    constructor(message, name) { super(message || ''); this.name = name || 'Error'; }
}

/* Поиск и соседи-элементы — у элементов, документа и фрагмента */
const ParentNode = {
    querySelector(s) { return wrap(N.find(this._i, String(s), true)); },
    querySelectorAll(s) { return list(N.find(this._i, String(s), false)); },
    getElementsByTagName(t) { t = String(t); return list(N.find(this._i, t === '*' ? '*' : cssIdent(t), false)); },
    getElementsByClassName(c) {
        const cs = String(c).trim().split(/\s+/).filter(Boolean);
        return cs.length ? list(N.find(this._i, cs.map(x => '.' + cssIdent(x)).join(''), false)) : list([]);
    },
    append(...a) { const f = nodesOf(a), ids = N.kids(f); N.insert(this._i, f, 0); ids.forEach(runInserted); },
    prepend(...a) { const f = nodesOf(a), ids = N.kids(f); N.insert(this._i, f, N.first(this._i)); ids.forEach(runInserted); },
    replaceChildren(...a) { for (let k = N.first(this._i); k; k = N.first(this._i)) N.detach(k); this.append(...a); },
};
function elKids(id) { const a = []; for (let k = N.first(id); k; k = N.next(k)) if (N.type(k) === 1) a.push(k); return a; }
function defParent(proto) {
    Object.assign(proto, ParentNode);
    def(proto, 'children', function () { return list(elKids(this._i)); });
    def(proto, 'childElementCount', function () { return elKids(this._i).length; });
    def(proto, 'firstElementChild', function () { let k = N.first(this._i); while (k && N.type(k) !== 1) k = N.next(k); return wrap(k); });
    def(proto, 'lastElementChild', function () { let k = N.last(this._i); while (k && N.type(k) !== 1) k = N.prev(k); return wrap(k); });
}
const ChildNode = {
    remove() { N.detach(this._i); },
    before(...a) { const p = N.parent(this._i); if (p) N.insert(p, nodesOf(a), this._i); },
    after(...a) { const p = N.parent(this._i); if (p) N.insert(p, nodesOf(a), N.next(this._i)); },
    replaceWith(...a) { const p = N.parent(this._i); if (p) { N.insert(p, nodesOf(a), this._i); N.detach(this._i); } },
};
function cssIdent(s) {                           /* CSS.escape */
    return String(s).replace(/[\0-\x1f\x7f]|^-?\d|^-$|[^\w\-\u0080-￿]/g, (c, i) =>
        /\d/.test(c) && (i === 0 || (i === 1 && s[0] === '-')) ? '\\' + c.charCodeAt(0).toString(16) + ' ' : '\\' + c);
}

class CharacterData extends Node {
    get data() { return N.data(this._i); }
    set data(v) { N.setData(this._i, String(v)); }
    get length() { return N.data(this._i).length; }
    get nextElementSibling() { let k = N.next(this._i); while (k && N.type(k) !== 1) k = N.next(k); return wrap(k); }
    get previousElementSibling() { let k = N.prev(this._i); while (k && N.type(k) !== 1) k = N.prev(k); return wrap(k); }
    appendData(s) { this.data += s; }
    substringData(o, n) { return this.data.substr(o, n); }
}
Object.assign(CharacterData.prototype, ChildNode);
class Text extends CharacterData {
    get wholeText() { return this.data; }
    splitText(o) {
        const d = this.data, t = N.text(d.slice(o), false);
        this.data = d.slice(0, o);
        const p = N.parent(this._i);
        if (p) N.insert(p, t, N.next(this._i));
        return wrap(t);
    }
}
class Comment extends CharacterData {}
class DocumentType extends Node { get name() { return 'html'; } get publicId() { return ''; } get systemId() { return ''; } }
class DocumentFragment extends Node {
    getElementById(id) { return this.querySelector('#' + cssIdent(id)); }
}
defParent(DocumentFragment.prototype);

/* --- атрибуты, классы, стиль ------------------------------------------------------------------ */
class Attr {
    constructor(el, name) { hide(this, '_e', el); this.name = name; this.localName = name; this.specified = true; this.namespaceURI = null; this.prefix = null; }
    get value() { return this._e.getAttribute(this.name); }
    set value(v) { this._e.setAttribute(this.name, v); }
    get nodeValue() { return this.value; }
    get ownerElement() { return this._e; }
}
class DOMTokenList {
    constructor(el, attr) { hide(this, '_e', el); hide(this, '_a', attr); }
    _get() { const v = N.getAttr(this._e._i, this._a); return v ? v.trim().split(/\s+/).filter(Boolean) : []; }
    _set(a) { N.setAttr(this._e._i, this._a, a.join(' ')); }
    get length() { return this._get().length; }
    get value() { return N.getAttr(this._e._i, this._a) || ''; }
    set value(v) { N.setAttr(this._e._i, this._a, String(v)); }
    item(i) { return this._get()[i] || null; }
    contains(t) { return this._get().indexOf(String(t)) >= 0; }
    add(...t) { const a = this._get(); let ch = false; for (const x of t) if (a.indexOf(String(x)) < 0) { a.push(String(x)); ch = true; } if (ch || N.getAttr(this._e._i, this._a) === null) this._set(a); }
    remove(...t) { const a = this._get(), b = a.filter(x => t.map(String).indexOf(x) < 0); if (b.length !== a.length) this._set(b); }
    toggle(t, force) {
        t = String(t);
        const has = this.contains(t);
        if (force === true || (force === undefined && !has)) { if (!has) this.add(t); return true; }
        if (has) this.remove(t);
        return false;
    }
    replace(a, b) { const l = this._get(), i = l.indexOf(String(a)); if (i < 0) return false; l[i] = String(b); this._set(l); return true; }
    supports() { return true; }
    forEach(f, self) { this._get().forEach((x, i) => f.call(self, x, i, this)); }
    entries() { return this._get().entries(); }
    keys() { return this._get().keys(); }
    values() { return this._get().values(); }
    toString() { return this.value; }
    [Symbol.iterator]() { return this._get()[Symbol.iterator](); }
}
/* style="…" ⇄ объект стиля: el.style.display = 'none' правит атрибут */
function parseStyle(s) {
    const m = new Map();
    if (s) for (const part of s.split(';')) {
        const i = part.indexOf(':');
        if (i > 0) m.set(part.slice(0, i).trim().toLowerCase(), part.slice(i + 1).trim());
    }
    return m;
}
function styleOf(el) {
    if (Object.prototype.hasOwnProperty.call(el, '_st')) return el._st;
    const id = el._i;
    const decl = {
        getPropertyValue(p) { return parseStyle(N.getAttr(id, 'style')).get(String(p).toLowerCase()) || ''; },
        getPropertyPriority() { return ''; },
        setProperty(p, v) {
            const m = parseStyle(N.getAttr(id, 'style'));
            p = String(p).toLowerCase();
            if (v === null || v === undefined || v === '') m.delete(p); else m.set(p, String(v));
            N.setAttr(id, 'style', Array.from(m, ([k, x]) => k + ': ' + x).join('; '));
        },
        removeProperty(p) { const was = this.getPropertyValue(p); this.setProperty(p, ''); return was; },
        item(i) { return Array.from(parseStyle(N.getAttr(id, 'style')).keys())[i] || ''; },
    };
    const px = new Proxy(decl, {
        get(t, k) {
            if (typeof k !== 'string') return undefined;
            if (k in t) return t[k];
            if (k === 'cssText') return N.getAttr(id, 'style') || '';
            if (k === 'length') return parseStyle(N.getAttr(id, 'style')).size;
            if (k === 'cssFloat') k = 'float';
            return t.getPropertyValue(k.startsWith('--') ? k : camel2kebab(k));
        },
        set(t, k, v) {
            if (typeof k !== 'string') return true;
            if (k === 'cssText') { N.setAttr(id, 'style', String(v)); return true; }
            if (k === 'cssFloat') k = 'float';
            t.setProperty(k.startsWith('--') ? k : camel2kebab(k), v);
            return true;
        },
        has(t, k) { return typeof k === 'string'; },
    });
    hide(el, '_st', px);
    return px;
}
function datasetOf(el) {
    if (Object.prototype.hasOwnProperty.call(el, '_ds')) return el._ds;
    const id = el._i;
    const px = new Proxy({}, {
        get(t, k) { if (typeof k !== 'string') return undefined; const v = N.getAttr(id, 'data-' + camel2kebab(k)); return v === null ? undefined : v; },
        set(t, k, v) { if (typeof k === 'string') N.setAttr(id, 'data-' + camel2kebab(k), String(v)); return true; },
        deleteProperty(t, k) { if (typeof k === 'string') N.rmAttr(id, 'data-' + camel2kebab(k)); return true; },
        has(t, k) { return typeof k === 'string' && N.getAttr(id, 'data-' + camel2kebab(k)) !== null; },
        ownKeys() { const a = N.attrs(id), r = []; for (let i = 0; i < a.length; i += 2) if (a[i].startsWith('data-')) r.push(kebab2camel(a[i].slice(5))); return r; },
        getOwnPropertyDescriptor(t, k) { const v = this.get(t, k); return v === undefined ? undefined : {value: v, enumerable: true, configurable: true, writable: true}; },
    });
    hide(el, '_ds', px);
    return px;
}

/* --- элементы ------------------------------------------------------------------------------- */
const BLOCKS = new Set(['html', 'body', 'div', 'p', 'section', 'article', 'aside', 'header', 'footer', 'nav', 'main',
    'ul', 'ol', 'li', 'dl', 'dd', 'dt', 'table', 'form', 'fieldset', 'figure', 'figcaption', 'blockquote', 'pre',
    'h1', 'h2', 'h3', 'h4', 'h5', 'h6', 'hr', 'address', 'details', 'summary', 'menu', 'center']);
const HIDDEN = new Set(['head', 'script', 'style', 'meta', 'link', 'title', 'template', 'noscript', 'base']);
function displayOf(id) {
    if (N.getAttr(id, 'hidden') !== null) return 'none';
    const d = N.cssProp(id, 'display');
    if (d) return d;
    const t = N.tag(id);
    return HIDDEN.has(t) ? 'none' : BLOCKS.has(t) ? 'block' : t === 'tr' ? 'table-row' : t === 'td' || t === 'th' ? 'table-cell' : 'inline';
}
function shown(id) {
    for (let n = id; n && N.type(n) === 1; n = N.parent(n)) if (displayOf(n) === 'none') return false;
    return N.contains(1, id);
}
function rect(el) {
    const w = shown(el._i) ? (BLOCKS.has(N.tag(el._i)) ? DOCW : 100) : 0, h = w ? 20 : 0;
    return {x: 0, y: 0, top: 0, left: 0, width: w, height: h, right: w, bottom: h, toJSON() { return this; }};
}

class Element extends Node {
    get tagName() { const t = N.tag(this._i); return this instanceof SVGElement || this._svg ? t : t.toUpperCase(); }
    get localName() { return N.tag(this._i); }
    get namespaceURI() { return 'http://www.w3.org/1999/xhtml'; }
    get prefix() { return null; }
    get id() { return N.getAttr(this._i, 'id') || ''; }
    set id(v) { N.setAttr(this._i, 'id', String(v)); }
    get className() { return N.getAttr(this._i, 'class') || ''; }
    set className(v) { N.setAttr(this._i, 'class', String(v)); }
    get classList() { return Object.prototype.hasOwnProperty.call(this, '_cl') ? this._cl : (hide(this, '_cl', new DOMTokenList(this, 'class')), this._cl); }
    set classList(v) { N.setAttr(this._i, 'class', String(v)); }
    get attributes() {
        /* NamedNodeMap: по номеру и по имени — el.attributes['onfocusin'] (jQuery 1.12 проверяет так
         * поддержку событий: без этого «cannot read property 'expando' of undefined», bbb.retroscene.org) */
        const a = N.attrs(this._i), r = [];
        for (let i = 0; i < a.length; i += 2) {
            const at = new Attr(this, a[i]);
            r.push(at);
            if (!(a[i] in r)) hide(r, a[i], at);
        }
        hide(r, 'getNamedItem', n => this.hasAttribute(n) ? new Attr(this, String(n).toLowerCase()) : null);
        hide(r, 'item', i => r[i] || null);
        return r;
    }
    getAttribute(n) { return N.getAttr(this._i, String(n).toLowerCase()); }
    getAttributeNS(ns, n) { return this.getAttribute(n); }
    setAttribute(n, v) { N.setAttr(this._i, String(n).toLowerCase(), String(v)); }
    setAttributeNS(ns, n, v) { this.setAttribute(n, v); }
    removeAttribute(n) { N.rmAttr(this._i, String(n).toLowerCase()); }
    removeAttributeNS(ns, n) { this.removeAttribute(n); }
    hasAttribute(n) { return N.getAttr(this._i, String(n).toLowerCase()) !== null; }
    hasAttributeNS(ns, n) { return this.hasAttribute(n); }
    hasAttributes() { return N.attrs(this._i).length > 0; }
    getAttributeNames() { const a = N.attrs(this._i), r = []; for (let i = 0; i < a.length; i += 2) r.push(a[i]); return r; }
    getAttributeNode(n) { return this.hasAttribute(n) ? new Attr(this, String(n).toLowerCase()) : null; }
    toggleAttribute(n, force) {
        const has = this.hasAttribute(n);
        if (force === true || (force === undefined && !has)) { if (!has) this.setAttribute(n, ''); return true; }
        this.removeAttribute(n);
        return false;
    }
    get innerHTML() { return N.serialize(this._i, false); }
    set innerHTML(v) {
        for (let k = N.first(this._i); k; k = N.first(this._i)) N.detach(k);
        N.insertHTML(this._i, 0, v === null ? '' : String(v));
    }
    get outerHTML() { return N.serialize(this._i, true); }
    set outerHTML(v) {
        const p = N.parent(this._i);
        if (!p) return;
        N.insertHTML(p, this._i, String(v));
        N.detach(this._i);
    }
    get innerText() { return this.textContent; }
    set innerText(v) { this.textContent = v; }
    get outerText() { return this.textContent; }
    insertAdjacentHTML(pos, html) {
        pos = String(pos).toLowerCase();
        const p = N.parent(this._i);
        if (pos === 'beforebegin' && p) N.insertHTML(p, this._i, String(html));
        else if (pos === 'afterbegin') N.insertHTML(this._i, N.first(this._i), String(html));
        else if (pos === 'beforeend') N.insertHTML(this._i, 0, String(html));
        else if (pos === 'afterend' && p) N.insertHTML(p, N.next(this._i), String(html));
    }
    insertAdjacentElement(pos, el) {
        pos = String(pos).toLowerCase();
        const p = N.parent(this._i);
        if (pos === 'beforebegin' && p) N.insert(p, idOf(el), this._i);
        else if (pos === 'afterbegin') N.insert(this._i, idOf(el), N.first(this._i));
        else if (pos === 'beforeend') N.insert(this._i, idOf(el), 0);
        else if (pos === 'afterend' && p) N.insert(p, idOf(el), N.next(this._i));
        else return null;
        runInserted(el._i);
        return el;
    }
    insertAdjacentText(pos, text) { this.insertAdjacentElement(pos, wrap(N.text(String(text), false))); }
    get nextElementSibling() { let k = N.next(this._i); while (k && N.type(k) !== 1) k = N.next(k); return wrap(k); }
    get previousElementSibling() { let k = N.prev(this._i); while (k && N.type(k) !== 1) k = N.prev(k); return wrap(k); }
    matches(s) { return N.matches(this._i, String(s), this._i); }
    webkitMatchesSelector(s) { return this.matches(s); }
    msMatchesSelector(s) { return this.matches(s); }
    closest(s) { for (let n = this._i; n && N.type(n) === 1; n = N.parent(n)) if (N.matches(n, String(s), this._i)) return wrap(n); return null; }
    getBoundingClientRect() { return rect(this); }
    getClientRects() { const r = rect(this); return r.width ? [r] : []; }
    get offsetWidth() { return rect(this).width; }
    get offsetHeight() { return rect(this).height; }
    get clientWidth() { return rect(this).width; }
    get clientHeight() { return rect(this).height; }
    get scrollWidth() { return rect(this).width; }
    get scrollHeight() { return rect(this).height; }
    get offsetTop() { return 0; }
    get offsetLeft() { return 0; }
    get offsetParent() { return shown(this._i) ? document.body : null; }
    get scrollTop() { return 0; } set scrollTop(v) {}
    get scrollLeft() { return 0; } set scrollLeft(v) {}
    get clientTop() { return 0; } get clientLeft() { return 0; }
    scrollIntoView() {} scroll() {} scrollTo() {} scrollBy() {}
    focus() {} blur() {}
    click() { this.dispatchEvent(new MouseEvent('click', {bubbles: true, cancelable: true})); }
    animate() { return {finished: Promise.resolve(), cancel() {}, play() {}, pause() {}, onfinish: null, addEventListener() {}}; }
    getAnimations() { return []; }
    attachShadow() { const f = wrap(N.frag()); hide(this, 'shadowRoot', f); return f; }
    requestFullscreen() { return Promise.reject(new TypeError('fullscreen')); }
}
Object.assign(Element.prototype, ChildNode);
defParent(Element.prototype);

/* Свойства, отражающие атрибуты: строки, адреса (абсолютные), флаги, числа */
function reflect(proto, prop, attr, kind) {
    attr = attr || prop.toLowerCase();
    if (kind === 'bool') def(proto, prop, function () { return N.getAttr(this._i, attr) !== null; },
        function (v) { if (v) N.setAttr(this._i, attr, ''); else N.rmAttr(this._i, attr); });
    else if (kind === 'url') def(proto, prop, function () { const v = N.getAttr(this._i, attr); return v === null ? '' : (N.resolve(v) || v); },
        function (v) { N.setAttr(this._i, attr, String(v)); });
    else if (kind === 'num') def(proto, prop, function () { const v = parseInt(N.getAttr(this._i, attr), 10); return isNaN(v) ? 0 : v; },
        function (v) { N.setAttr(this._i, attr, String(v)); });
    else def(proto, prop, function () { return N.getAttr(this._i, attr) || ''; }, function (v) { N.setAttr(this._i, attr, String(v)); });
}

class HTMLElement extends Element {
    get style() { return styleOf(this); }
    set style(v) { N.setAttr(this._i, 'style', String(v)); }
    get dataset() { return datasetOf(this); }
    get tabIndex() { const v = parseInt(N.getAttr(this._i, 'tabindex'), 10); return isNaN(v) ? -1 : v; }
    set tabIndex(v) { N.setAttr(this._i, 'tabindex', String(v)); }
}
for (const p of ['title', 'lang', 'dir', 'accessKey', 'slot', 'role']) reflect(HTMLElement.prototype, p);
reflect(HTMLElement.prototype, 'hidden', 'hidden', 'bool');
reflect(HTMLElement.prototype, 'draggable', 'draggable', 'bool');
def(HTMLElement.prototype, 'isContentEditable', () => false);
def(HTMLElement.prototype, 'contentEditable', () => 'inherit', () => {});
/* on…-свойства есть у элементов («'onclick' in el»), по умолчанию null */
for (const e of ['click', 'load', 'error', 'change', 'input', 'submit', 'focus', 'blur', 'keydown', 'keyup', 'keypress',
                 'mousedown', 'mouseup', 'mouseover', 'mouseout', 'mousemove', 'mouseenter', 'mouseleave', 'scroll',
                 'resize', 'abort', 'reset', 'select', 'dblclick', 'contextmenu', 'wheel', 'animationend', 'transitionend',
                 'pointerdown', 'pointerup', 'pointermove'])
    HTMLElement.prototype['on' + e] = null;

class HTMLAnchorElement extends HTMLElement {
    get pathname() { try { return new URL(this.href).pathname; } catch (e) { return ''; } }
    get hostname() { try { return new URL(this.href).hostname; } catch (e) { return ''; } }
    get host() { try { return new URL(this.href).host; } catch (e) { return ''; } }
    get protocol() { try { return new URL(this.href).protocol; } catch (e) { return ':'; } }
    get search() { try { return new URL(this.href).search; } catch (e) { return ''; } }
    get hash() { try { return new URL(this.href).hash; } catch (e) { return ''; } }
    get origin() { try { return new URL(this.href).origin; } catch (e) { return ''; } }
    get text() { return this.textContent; }
}
reflect(HTMLAnchorElement.prototype, 'href', 'href', 'url');
for (const p of ['target', 'rel', 'download', 'type', 'hreflang', 'name']) reflect(HTMLAnchorElement.prototype, p);
class HTMLAreaElement extends HTMLAnchorElement {}
class HTMLLinkElement extends HTMLElement { get sheet() { return null; } get relList() { return new DOMTokenList(this, 'rel'); } }
reflect(HTMLLinkElement.prototype, 'href', 'href', 'url');
for (const p of ['rel', 'media', 'type', 'as', 'crossOrigin', 'integrity']) reflect(HTMLLinkElement.prototype, p);
class HTMLImageElement extends HTMLElement {
    get complete() { return true; }
    get naturalWidth() { return parseInt(N.getAttr(this._i, 'width'), 10) || 0; }
    get naturalHeight() { return parseInt(N.getAttr(this._i, 'height'), 10) || 0; }
    get currentSrc() { return this.src; }
    decode() { return Promise.resolve(); }
}
reflect(HTMLImageElement.prototype, 'src', 'src', 'url');
for (const p of ['alt', 'srcset', 'sizes', 'loading', 'decoding', 'crossOrigin', 'useMap', 'referrerPolicy']) reflect(HTMLImageElement.prototype, p);
reflect(HTMLImageElement.prototype, 'width', 'width', 'num');
reflect(HTMLImageElement.prototype, 'height', 'height', 'num');
class HTMLSourceElement extends HTMLElement {}
reflect(HTMLSourceElement.prototype, 'src', 'src', 'url');
for (const p of ['srcset', 'sizes', 'type', 'media']) reflect(HTMLSourceElement.prototype, p);
class HTMLScriptElement extends HTMLElement {
    get text() { return this.textContent; }
    set text(v) { this.textContent = v; }
}
reflect(HTMLScriptElement.prototype, 'src', 'src', 'url');
for (const p of ['type', 'charset', 'crossOrigin', 'integrity', 'nonce']) reflect(HTMLScriptElement.prototype, p);
for (const p of ['async', 'defer', 'noModule']) reflect(HTMLScriptElement.prototype, p, p.toLowerCase(), 'bool');
class HTMLStyleElement extends HTMLElement { get sheet() { return {cssRules: [], insertRule() { return 0; }, deleteRule() {}}; } }
reflect(HTMLStyleElement.prototype, 'media');
class HTMLFormElement extends HTMLElement {
    get elements() { return this.querySelectorAll('input,select,textarea,button,fieldset,output,object'); }
    get length() { return this.elements.length; }
    submit() {} reset() {} requestSubmit() {} checkValidity() { return true; } reportValidity() { return true; }
}
reflect(HTMLFormElement.prototype, 'action', 'action', 'url');
for (const p of ['method', 'name', 'target', 'enctype', 'autocomplete']) reflect(HTMLFormElement.prototype, p);
class HTMLInputElement extends HTMLElement {
    get value() { const v = N.getAttr(this._i, 'value'); return v === null ? ((this.type === 'checkbox' || this.type === 'radio') ? 'on' : '') : v; }
    set value(v) { N.setAttr(this._i, 'value', String(v)); }
    get defaultValue() { return N.getAttr(this._i, 'value') || ''; }
    set defaultValue(v) { N.setAttr(this._i, 'value', String(v)); }
    get type() { return (N.getAttr(this._i, 'type') || 'text').toLowerCase(); }
    set type(v) { N.setAttr(this._i, 'type', String(v)); }
    get defaultChecked() { return N.getAttr(this._i, 'checked') !== null; }
    get form() { return this.closest('form'); }
    get validity() { return {valid: true}; }
    get labels() { return list([]); }
    select() {} setSelectionRange() {} checkValidity() { return true; } reportValidity() { return true; } setCustomValidity() {}
}
for (const p of ['checked', 'disabled', 'readOnly', 'required', 'multiple', 'autofocus']) reflect(HTMLInputElement.prototype, p, p.toLowerCase(), 'bool');
for (const p of ['name', 'placeholder', 'accept', 'autocomplete', 'pattern', 'min', 'max', 'step', 'inputMode']) reflect(HTMLInputElement.prototype, p);
reflect(HTMLInputElement.prototype, 'src', 'src', 'url');
reflect(HTMLInputElement.prototype, 'maxLength', 'maxlength', 'num');
reflect(HTMLInputElement.prototype, 'size', 'size', 'num');
class HTMLButtonElement extends HTMLElement {
    get type() { return (N.getAttr(this._i, 'type') || 'submit').toLowerCase(); }
    set type(v) { N.setAttr(this._i, 'type', String(v)); }
    get form() { return this.closest('form'); }
}
reflect(HTMLButtonElement.prototype, 'disabled', 'disabled', 'bool');
for (const p of ['name', 'value']) reflect(HTMLButtonElement.prototype, p);
class HTMLTextAreaElement extends HTMLElement {
    get value() { return this.textContent; }
    set value(v) { this.textContent = v; }
    get defaultValue() { return this.textContent; }
    set defaultValue(v) { this.textContent = v; }
    get type() { return 'textarea'; }
    get form() { return this.closest('form'); }
    select() {} setSelectionRange() {}
}
for (const p of ['name', 'placeholder']) reflect(HTMLTextAreaElement.prototype, p);
for (const p of ['disabled', 'readOnly', 'required']) reflect(HTMLTextAreaElement.prototype, p, p.toLowerCase(), 'bool');
reflect(HTMLTextAreaElement.prototype, 'rows', 'rows', 'num');
reflect(HTMLTextAreaElement.prototype, 'cols', 'cols', 'num');
class HTMLOptionElement extends HTMLElement {
    get value() { const v = N.getAttr(this._i, 'value'); return v === null ? this.textContent.trim() : v; }
    set value(v) { N.setAttr(this._i, 'value', String(v)); }
    get text() { return this.textContent; }
    set text(v) { this.textContent = v; }
    get index() { const s = this.closest('select'); return s ? s.options.indexOf(this) : 0; }
    get defaultSelected() { return N.getAttr(this._i, 'selected') !== null; }
}
reflect(HTMLOptionElement.prototype, 'selected', 'selected', 'bool');
reflect(HTMLOptionElement.prototype, 'disabled', 'disabled', 'bool');
reflect(HTMLOptionElement.prototype, 'label');
class HTMLSelectElement extends HTMLElement {
    get options() { return this.querySelectorAll('option'); }
    get length() { return this.options.length; }
    get selectedIndex() { const o = this.options; for (let i = 0; i < o.length; i++) if (o[i].selected) return i; return o.length && !this.multiple ? 0 : -1; }
    set selectedIndex(n) { const o = this.options; for (let i = 0; i < o.length; i++) o[i].selected = i === n; }
    get selectedOptions() { return list(this.options.filter(o => o.selected).map(o => o._i)); }
    get value() { const i = this.selectedIndex; return i >= 0 ? this.options[i].value : ''; }
    set value(v) { const o = this.options; for (let i = 0; i < o.length; i++) o[i].selected = o[i].value === String(v); }
    get type() { return this.multiple ? 'select-multiple' : 'select-one'; }
    get form() { return this.closest('form'); }
    add(o, before) { this.insertBefore(o, typeof before === 'number' ? this.options[before] || null : before || null); }
    item(i) { return this.options[i] || null; }
}
reflect(HTMLSelectElement.prototype, 'name');
for (const p of ['disabled', 'multiple', 'required']) reflect(HTMLSelectElement.prototype, p, p.toLowerCase(), 'bool');
class HTMLTemplateElement extends HTMLElement {
    get content() {
        if (!Object.prototype.hasOwnProperty.call(this, '_c')) {
            const f = N.frag();
            N.insertHTML(f, 0, N.serialize(this._i, false));
            hide(this, '_c', wrap(f));
        }
        return this._c;
    }
}
class HTMLIFrameElement extends HTMLElement { get contentWindow() { return null; } get contentDocument() { return null; } }
reflect(HTMLIFrameElement.prototype, 'src', 'src', 'url');
for (const p of ['name', 'allow', 'loading']) reflect(HTMLIFrameElement.prototype, p);
class HTMLCanvasElement extends HTMLElement { getContext() { return null; } toDataURL() { return 'data:,'; } }
reflect(HTMLCanvasElement.prototype, 'width', 'width', 'num');
reflect(HTMLCanvasElement.prototype, 'height', 'height', 'num');
class HTMLMediaElement extends HTMLElement {
    play() { return Promise.reject(new DOMException('no media', 'NotSupportedError')); }
    pause() {} load() {} canPlayType() { return ''; }
    get paused() { return true; } get currentTime() { return 0; } set currentTime(v) {}
    get duration() { return NaN; } get readyState() { return 0; }
}
reflect(HTMLMediaElement.prototype, 'src', 'src', 'url');
for (const p of ['autoplay', 'loop', 'muted', 'controls']) reflect(HTMLMediaElement.prototype, p, p, 'bool');
class HTMLVideoElement extends HTMLMediaElement {}
class HTMLAudioElement extends HTMLMediaElement {}
class HTMLLabelElement extends HTMLElement { get control() { const f = this.htmlFor; return f ? document.getElementById(f) : this.querySelector('input,select,textarea'); } }
reflect(HTMLLabelElement.prototype, 'htmlFor', 'for');
class HTMLMetaElement extends HTMLElement {}
for (const p of ['name', 'content', 'httpEquiv', 'charset']) reflect(HTMLMetaElement.prototype, p, p === 'httpEquiv' ? 'http-equiv' : p.toLowerCase());
class HTMLTableElement extends HTMLElement {
    get rows() { return this.querySelectorAll('tr'); }
    get tBodies() { return this.querySelectorAll('tbody'); }
}
class HTMLTableRowElement extends HTMLElement { get cells() { return this.children; } }
class HTMLTableCellElement extends HTMLElement {}
reflect(HTMLTableCellElement.prototype, 'colSpan', 'colspan', 'num');
reflect(HTMLTableCellElement.prototype, 'rowSpan', 'rowspan', 'num');
class HTMLDivElement extends HTMLElement {}
class HTMLSpanElement extends HTMLElement {}
class HTMLParagraphElement extends HTMLElement {}
class HTMLHeadingElement extends HTMLElement {}
class HTMLUListElement extends HTMLElement {}
class HTMLOListElement extends HTMLElement {}
class HTMLLIElement extends HTMLElement {}
class HTMLBodyElement extends HTMLElement {}
class HTMLHeadElement extends HTMLElement {}
class HTMLHtmlElement extends HTMLElement {}
class HTMLBRElement extends HTMLElement {}
class HTMLHRElement extends HTMLElement {}
class HTMLPreElement extends HTMLElement {}
class HTMLTitleElement extends HTMLElement { get text() { return this.textContent; } set text(v) { this.textContent = v; } }
class HTMLUnknownElement extends HTMLElement {}
class SVGElement extends Element {
    get style() { return styleOf(this); }
    get dataset() { return datasetOf(this); }
    get ownerSVGElement() { return this.closest('svg'); }
}
class SVGSVGElement extends SVGElement {}
const TAGS = {
    a: HTMLAnchorElement, area: HTMLAreaElement, link: HTMLLinkElement, img: HTMLImageElement, source: HTMLSourceElement,
    script: HTMLScriptElement, style: HTMLStyleElement, form: HTMLFormElement, input: HTMLInputElement,
    button: HTMLButtonElement, textarea: HTMLTextAreaElement, option: HTMLOptionElement, select: HTMLSelectElement,
    template: HTMLTemplateElement, iframe: HTMLIFrameElement, canvas: HTMLCanvasElement, video: HTMLVideoElement,
    audio: HTMLAudioElement, label: HTMLLabelElement, meta: HTMLMetaElement, table: HTMLTableElement,
    tr: HTMLTableRowElement, td: HTMLTableCellElement, th: HTMLTableCellElement, div: HTMLDivElement,
    span: HTMLSpanElement, p: HTMLParagraphElement, h1: HTMLHeadingElement, h2: HTMLHeadingElement,
    h3: HTMLHeadingElement, h4: HTMLHeadingElement, h5: HTMLHeadingElement, h6: HTMLHeadingElement,
    ul: HTMLUListElement, ol: HTMLOListElement, li: HTMLLIElement, body: HTMLBodyElement, head: HTMLHeadElement,
    html: HTMLHtmlElement, br: HTMLBRElement, hr: HTMLHRElement, pre: HTMLPreElement, title: HTMLTitleElement,
    svg: SVGSVGElement,
};
for (const t of ['g', 'path', 'circle', 'rect', 'line', 'polyline', 'polygon', 'use', 'defs', 'symbol', 'text',
                 'tspan', 'lineargradient', 'radialgradient', 'stop', 'clippath', 'mask', 'image', 'ellipse'])
    TAGS[t] = SVGElement;

/* --- документ ------------------------------------------------------------------------------- */
let cookies = '';
class Document extends Node {
    get documentElement() { return wrap(N.html()); }
    get head() { return wrap(N.head()); }
    get body() { return wrap(N.body()); }
    get title() { const t = this.querySelector('title'); return t ? t.textContent.replace(/\s+/g, ' ').trim() : ''; }
    set title(v) { let t = this.querySelector('title'); if (!t) { t = this.createElement('title'); this.head.appendChild(t); } t.textContent = v; }
    get readyState() { return readyState; }
    get currentScript() { return wrap(current); }
    get defaultView() { return G; }
    get location() { return location; }
    set location(v) { location.href = v; }
    get URL() { return location.href; }
    get documentURI() { return location.href; }
    get baseURI() { return location.href; }
    get domain() { return location.hostname; }
    get referrer() { return ''; }
    get characterSet() { return 'UTF-8'; }
    get charset() { return 'UTF-8'; }
    get inputEncoding() { return 'UTF-8'; }
    get contentType() { return 'text/html'; }
    get compatMode() { return 'CSS1Compat'; }
    get doctype() { const k = N.first(1); return k && N.type(k) === 10 ? wrap(k) : null; }
    get visibilityState() { return 'visible'; }
    get hidden() { return false; }
    get activeElement() { return this.body; }
    get scrollingElement() { return this.documentElement; }
    get forms() { return this.querySelectorAll('form'); }
    get images() { return this.querySelectorAll('img'); }
    get links() { return this.querySelectorAll('a[href],area[href]'); }
    get scripts() { return this.querySelectorAll('script'); }
    get styleSheets() { return []; }
    get fonts() { return {ready: Promise.resolve(), status: 'loaded', check() { return true; }, load() { return Promise.resolve([]); }, addEventListener() {}, forEach() {}}; }
    get cookie() { return cookies; }
    set cookie(v) {
        const kv = String(v).split(';')[0], i = kv.indexOf('='), name = (i < 0 ? '' : kv.slice(0, i)).trim();
        const parts = cookies ? cookies.split('; ').filter(c => !c.startsWith(name + '=')) : [];
        if (!/;\s*(max-age=0|expires=Thu, 01 Jan 1970)/i.test(String(v))) parts.push(kv.trim());
        cookies = parts.join('; ');
    }
    get implementation() {
        return {
            hasFeature() { return true; },
            createHTMLDocument(title) {
                /* отдельного документа нет: «документ» над отцеплённым <html> (jQuery разбирает так HTML) */
                const h = N.create('html'), hd = N.create('head'), b = N.create('body');
                N.insert(h, hd, 0); N.insert(h, b, 0);
                const d = Object.create(document);
                def(d, 'documentElement', () => wrap(h));
                def(d, 'head', () => wrap(hd));
                def(d, 'body', () => wrap(b));
                hide(d, 'querySelector', s => wrap(N.find(h, String(s), true)));
                hide(d, 'querySelectorAll', s => list(N.find(h, String(s), false)));
                return d;
            },
        };
    }
    getElementById(id) { return wrap(N.byId(String(id))); }
    getElementsByName(n) { return this.querySelectorAll('[name="' + String(n).replace(/"/g, '\\"') + '"]'); }
    createElement(t) { return wrap(N.create(String(t))); }
    createElementNS(ns, t) { const e = wrap(N.create(String(t))); if (/svg/.test(String(ns))) hide(e, '_svg', true); return e; }
    createTextNode(s) { return wrap(N.text(String(s), false)); }
    createComment(s) { return wrap(N.text(String(s), true)); }
    createDocumentFragment() { return wrap(N.frag()); }
    createAttribute(n) { return {name: String(n), value: ''}; }
    createEvent(t) { return /mouse/i.test(t) ? new MouseEvent('') : /key/i.test(t) ? new KeyboardEvent('') : /custom/i.test(t) ? new CustomEvent('') : new Event(''); }
    createRange() {
        return {
            setStart() {}, setEnd() {}, selectNode() {}, selectNodeContents() {}, collapse() {}, detach() {},
            getBoundingClientRect: () => rect({_i: N.body()}), getClientRects: () => [],
            createContextualFragment: h => { const f = N.frag(); N.insertHTML(f, 0, String(h)); return wrap(f); },
        };
    }
    createTreeWalker(root) { return {currentNode: root, nextNode() { return null; }}; }
    importNode(n, deep) { return n.cloneNode(deep); }
    adoptNode(n) { return n; }
    hasFocus() { return true; }
    getSelection() { return {rangeCount: 0, removeAllRanges() {}, addRange() {}, toString() { return ''; }}; }
    execCommand() { return false; }
    write(...a) {
        const html = a.join('');
        if (!writeAt) { N.log(1, 'document.write after load ignored'); return; }
        const p = writeAt[0], before = writeAt[1], prev = before ? N.prev(before) : N.last(p);
        N.insertHTML(p, before, html);
        for (let k = prev ? N.next(prev) : N.first(p); k && k !== before;) { const nx = N.next(k); runInserted(k); k = nx; }
    }
    writeln(...a) { this.write(...a, '\n'); }
    open() { return this; }
    close() {}
    elementFromPoint() { return this.body; }
    querySelector(s) { return wrap(N.find(1, String(s), true)); }
    querySelectorAll(s) { return list(N.find(1, String(s), false)); }
}
defParent(Document.prototype);

const document = wrap(1);

/* --- адрес страницы ------------------------------------------------------------------------- */
let href = N.url();
function parseUrl(u) {
    const m = /^([a-z][a-z0-9+.-]*:)(?:\/\/(?:([^@\/?#]*)@)?([^:\/?#]*)(?::(\d+))?)?([^?#]*)(\?[^#]*)?(#.*)?$/i.exec(u) || [];
    return {protocol: (m[1] || '').toLowerCase(), hostname: (m[3] || '').toLowerCase(), port: m[4] || '',
            pathname: m[5] || '/', search: m[6] && m[6] !== '?' ? m[6] : '', hash: m[7] && m[7] !== '#' ? m[7] : ''};
}
class URLSearchParams {
    constructor(init) {
        hide(this, '_p', []);
        if (typeof init === 'string') {
            for (const part of init.replace(/^\?/, '').split('&')) {
                if (!part) continue;
                const i = part.indexOf('='), dec = s => { try { return decodeURIComponent(s.replace(/\+/g, ' ')); } catch (e) { return s; } };
                this._p.push(i < 0 ? [dec(part), ''] : [dec(part.slice(0, i)), dec(part.slice(i + 1))]);
            }
        } else if (init && typeof init === 'object') {
            for (const k of Object.keys(init)) this._p.push([k, String(init[k])]);
        }
    }
    get(k) { const x = this._p.find(p => p[0] === k); return x ? x[1] : null; }
    getAll(k) { return this._p.filter(p => p[0] === k).map(p => p[1]); }
    has(k) { return this._p.some(p => p[0] === k); }
    set(k, v) { this.delete(k); this._p.push([k, String(v)]); }
    append(k, v) { this._p.push([k, String(v)]); }
    delete(k) { const a = this._p.filter(p => p[0] !== k); this._p.length = 0; this._p.push(...a); }
    forEach(f, s) { this._p.forEach(p => f.call(s, p[1], p[0], this)); }
    keys() { return this._p.map(p => p[0])[Symbol.iterator](); }
    values() { return this._p.map(p => p[1])[Symbol.iterator](); }
    entries() { return this._p.map(p => [p[0], p[1]])[Symbol.iterator](); }
    [Symbol.iterator]() { return this.entries(); }
    toString() { return this._p.map(p => encodeURIComponent(p[0]) + '=' + encodeURIComponent(p[1])).join('&'); }
}
class URL {
    constructor(u, base) {
        u = String(u);
        let abs = u;
        if (base !== undefined || !/^[a-z][a-z0-9+.-]*:/i.test(u)) {
            const b = base !== undefined ? String(base) : null;
            if (b === null) throw new TypeError('Invalid URL: ' + u);
            const saved = href;
            href = b;
            abs = resolveAgainst(b, u);
            href = saved;
        }
        if (!abs || !/^[a-z][a-z0-9+.-]*:/i.test(abs)) throw new TypeError('Invalid URL: ' + u);
        hide(this, '_u', parseUrl(encodeUrl(abs)));
        this._u.pathname = noDots(this._u.pathname);
        hide(this, '_sp', null);
    }
    get protocol() { return this._u.protocol; } set protocol(v) { this._u.protocol = String(v).replace(/:?$/, ':'); }
    get hostname() { return this._u.hostname; } set hostname(v) { this._u.hostname = String(v); }
    get port() { return this._u.port; } set port(v) { this._u.port = String(v); }
    get host() { return this._u.hostname + (this._u.port ? ':' + this._u.port : ''); }
    get origin() { return this._u.protocol + '//' + this.host; }
    get pathname() { return this._u.pathname; } set pathname(v) { this._u.pathname = String(v); }
    get search() { return this._sp ? (String(this._sp) ? '?' + this._sp : '') : this._u.search; }
    set search(v) { this._u.search = v ? '?' + String(v).replace(/^\?/, '') : ''; this._sp = null; }
    get searchParams() { if (!this._sp) hide(this, '_sp', new URLSearchParams(this._u.search)); return this._sp; }
    get hash() { return this._u.hash; } set hash(v) { this._u.hash = v ? '#' + String(v).replace(/^#/, '') : ''; }
    get username() { return ''; } get password() { return ''; }
    get href() { return this._u.protocol + (this._u.hostname || this._u.protocol === 'http:' || this._u.protocol === 'https:' ? '//' + this.host : '') + this._u.pathname + this.search + this._u.hash; }
    set href(v) { hide(this, '_u', parseUrl(String(v))); this._sp = null; }
    toString() { return this.href; }
    toJSON() { return this.href; }
    static createObjectURL() { return 'blob:tsafari'; }
    static revokeObjectURL() {}
}
/* Путь без «.» и «..» (RFC 3986, remove_dot_segments) */
function noDots(path) {
    const out = [];
    for (const seg of path.split('/')) {
        if (seg === '..') { if (out.length > 1) out.pop(); }
        else if (seg !== '.') out.push(seg);
    }
    if (/\/\.\.?$/.test(path)) out.push('');
    return out.join('/') || '/';
}
/* Не-ASCII и пробелы в адресе — %XX (как у браузеров), прочее как есть */
function encodeUrl(u) { return u.replace(/[^!-~]+/g, s => encodeURIComponent(s)); }
function resolveAgainst(base, ref) {
    if (base === N.url()) return N.resolve(ref) || '';
    /* другой базовый адрес: собрать вручную */
    if (/^[a-z][a-z0-9+.-]*:/i.test(ref)) return ref;
    const b = parseUrl(base);
    if (ref.startsWith('//')) return b.protocol + ref;
    const origin = b.protocol + '//' + b.hostname + (b.port ? ':' + b.port : '');
    if (ref.startsWith('/')) return origin + ref;
    if (ref.startsWith('?')) return origin + b.pathname + ref;
    if (ref.startsWith('#')) return origin + b.pathname + b.search + ref;
    const m = /^([^?#]*)(.*)$/.exec(ref);
    return origin + noDots(b.pathname.replace(/[^/]*$/, '') + m[1]) + m[2];
}
const location = {
    get href() { return href; },
    set href(v) { navigate(String(v), false); },
    get protocol() { return parseUrl(href).protocol; },
    get host() { const u = parseUrl(href); return u.hostname + (u.port ? ':' + u.port : ''); },
    get hostname() { return parseUrl(href).hostname; },
    get port() { return parseUrl(href).port; },
    get pathname() { return parseUrl(href).pathname; },
    get search() { return parseUrl(href).search; },
    get hash() { return parseUrl(href).hash; },
    set hash(v) { href = href.replace(/#.*$/, '') + (v ? '#' + String(v).replace(/^#/, '') : ''); },
    get origin() { const u = parseUrl(href); return u.protocol + '//' + u.hostname + (u.port ? ':' + u.port : ''); },
    get ancestorOrigins() { return []; },
    assign(u) { navigate(String(u), false); },
    replace(u) { navigate(String(u), true); },
    reload() {},
    toString() { return href; },
};
/* Скрипт просит перейти на другой адрес: браузер перейдёт, когда скрипты кончатся (js.c, js_nav).
 * Только «#…» на той же странице — не переход. Скрипты после этого больше не выполняются: у
 * браузера страница уже другая. */
function navigate(u, replace) {
    const abs = N.resolve(u);
    if (!abs) return;
    if (abs.replace(/#.*$/, '') === href.replace(/#.*$/, '')) { href = abs; return; }
    N.log(0, 'navigation: ' + abs);
    N.nav(abs, replace ? 1 : 0);
}
const history = {
    length: 1, state: null, scrollRestoration: 'auto',
    pushState(s, t, u) { this.state = s; if (u) href = N.resolve(String(u)) || href; },
    replaceState(s, t, u) { this.state = s; if (u) href = N.resolve(String(u)) || href; },
    back() {}, forward() {}, go() {},
};

/* --- таймеры и очередь ---------------------------------------------------------------------- */
let tseq = 0;
const timers = new Map();
function addTimer(f, ms, args, repeat) {
    const id = ++tseq;
    ms = Math.max(0, Number(ms) || 0);
    timers.set(id, {f: f, at: vnow + ms, ms: repeat ? Math.max(ms, 10) : 0, args: args, seq: id, runs: 0});
    return id;
}
function tick(limit) {
    /* один шаг: ближайший таймер не позже limit (вирт. мс); 1 — сделано, 0 — ждать нечего */
    let best = null;
    for (const t of timers.values()) if (t.at <= limit && (!best || t.at < best.at || (t.at === best.at && t.seq < best.seq))) best = t;
    if (!best) return 0;
    if (best.at > vnow) vnow = best.at;
    const id = best.seq;
    if (best.ms && ++best.runs < 50) best.at = vnow + best.ms; else timers.delete(id);
    try {
        if (typeof best.f === 'function') best.f.apply(G, best.args);
        else (0, eval)(String(best.f));
    } catch (e) { reportError(e); }
    return 1;
}

/* --- окно (window = globalThis) ------------------------------------------------------------- */
class Storage {
    constructor() { hide(this, '_m', new Map()); }
    get length() { return this._m.size; }
    key(i) { return Array.from(this._m.keys())[i] || null; }
    getItem(k) { k = String(k); return this._m.has(k) ? this._m.get(k) : null; }
    setItem(k, v) { this._m.set(String(k), String(v)); }
    removeItem(k) { this._m.delete(String(k)); }
    clear() { this._m.clear(); }
}
class MediaQueryList {
    constructor(q) { this.media = String(q); this.onchange = null; }
    get matches() { return N.media(this.media); }
    addListener() {} removeListener() {} addEventListener() {} removeEventListener() {}
}
class IntersectionObserver {
    /* всё «видно»: страницу показывают целиком, ленивые картинки и блоки должны загрузиться */
    constructor(cb, opt) { hide(this, '_cb', cb); hide(this, '_t', []); this.root = (opt && opt.root) || null; this.rootMargin = (opt && opt.rootMargin) || '0px'; this.thresholds = [0]; }
    observe(el) {
        if (this._t.indexOf(el) >= 0) return;
        this._t.push(el);
        const r = rect(el);
        setTimeout(() => { if (this._t.indexOf(el) >= 0) this._cb([{target: el, isIntersecting: true, intersectionRatio: 1, boundingClientRect: r, intersectionRect: r, rootBounds: null, time: vnow}], this); }, 0);
    }
    unobserve(el) { const i = this._t.indexOf(el); if (i >= 0) this._t.splice(i, 1); }
    disconnect() { this._t.length = 0; }
    takeRecords() { return []; }
}
class MutationObserver { constructor(cb) {} observe() {} disconnect() {} takeRecords() { return []; } }
class ResizeObserver { constructor(cb) {} observe() {} unobserve() {} disconnect() {} }
class PerformanceObserver { constructor() {} observe() {} disconnect() {} static get supportedEntryTypes() { return []; } }
class XMLHttpRequest extends EventTarget {
    constructor() { super(); this.readyState = 0; this.status = 0; this.statusText = ''; this.responseText = ''; this.response = ''; this.responseType = ''; this.timeout = 0; this.withCredentials = false; this.upload = new EventTarget(); }
    open(m, u) { this.readyState = 1; hide(this, '_u', N.resolve(String(u))); }
    setRequestHeader() {}
    getResponseHeader() { return null; }
    getAllResponseHeaders() { return ''; }
    overrideMimeType() {}
    abort() {}
    send() {
        N.log(1, 'XMLHttpRequest not supported yet: ' + this._u);
        setTimeout(() => {
            this.readyState = 4;
            for (const t of ['readystatechange', 'error', 'loadend']) {
                const ev = new ProgressEvent(t);
                this.dispatchEvent(ev);
                if (typeof this['on' + t] === 'function') this['on' + t](ev);
            }
        }, 0);
    }
}
Object.assign(XMLHttpRequest, {UNSENT: 0, OPENED: 1, HEADERS_RECEIVED: 2, LOADING: 3, DONE: 4});
function fetch(u) {
    N.log(1, 'fetch not supported yet: ' + (typeof u === 'string' ? N.resolve(u) : '?'));
    return Promise.reject(new TypeError('Failed to fetch'));
}
class Image {
    constructor(w, h) {
        const e = document.createElement('img');
        if (w !== undefined) e.width = w;
        if (h !== undefined) e.height = h;
        return e;
    }
}
const console = {};
for (const [k, lvl] of [['log', 0], ['info', 0], ['debug', 0], ['trace', 0], ['warn', 1], ['error', 2]])
    console[k] = (...a) => N.log(lvl, a.map(x => { try { return typeof x === 'object' && x !== null ? (x instanceof Error ? String(x) : JSON.stringify(x)) : String(x); } catch (e) { return String(x); } }).join(' '));
for (const k of ['group', 'groupCollapsed', 'groupEnd', 'time', 'timeEnd', 'timeLog', 'count', 'assert', 'table', 'dir'])
    console[k] = () => {};

const win = {
    window: G, self: G, top: G, parent: G, frames: G, globalThis: G, opener: null, closed: false, frameElement: null,
    document: document, location: location, history: history, name: '', length: 0,
    navigator: {
        userAgent: 'Mozilla/5.0 (ZX Evolution; VDAC4) TSafari/0.1', appName: 'Netscape', appVersion: '5.0',
        appCodeName: 'Mozilla', product: 'Gecko', vendor: '', platform: 'ZX Evolution', language: 'ru-RU',
        languages: ['ru-RU', 'ru', 'en'], onLine: true, cookieEnabled: true, doNotTrack: '1', maxTouchPoints: 0,
        hardwareConcurrency: 2, deviceMemory: 0.5, webdriver: false, pdfViewerEnabled: false, plugins: [], mimeTypes: [],
        javaEnabled() { return false; }, sendBeacon() { return true; }, vibrate() { return false; },
        serviceWorker: undefined, clipboard: undefined,
        userAgentData: {brands: [], mobile: false, platform: 'ZX Evolution', getHighEntropyValues() { return Promise.resolve({}); }},
    },
    screen: {width: 1024, height: 768, availWidth: 1024, availHeight: 768, colorDepth: 16, pixelDepth: 16,
             orientation: {type: 'landscape-primary', angle: 0, addEventListener() {}}},
    innerWidth: 1024, innerHeight: 768, outerWidth: 1024, outerHeight: 768, devicePixelRatio: 1,
    scrollX: 0, scrollY: 0, pageXOffset: 0, pageYOffset: 0, screenX: 0, screenY: 0, screenLeft: 0, screenTop: 0,
    isSecureContext: /^https:/.test(href), origin: location.origin, crossOriginIsolated: false,
    localStorage: new Storage(), sessionStorage: new Storage(),
    console: console,
    setTimeout: (f, ms, ...a) => addTimer(f, ms, a, false),
    setInterval: (f, ms, ...a) => addTimer(f, ms, a, true),
    clearTimeout: id => { timers.delete(id); },
    clearInterval: id => { timers.delete(id); },
    requestAnimationFrame: f => addTimer(() => f(vnow), 16, [], false),
    cancelAnimationFrame: id => { timers.delete(id); },
    requestIdleCallback: f => addTimer(() => f({didTimeout: false, timeRemaining: () => 10}), 1, [], false),
    cancelIdleCallback: id => { timers.delete(id); },
    queueMicrotask: f => { Promise.resolve().then(f); },
    setImmediate: (f, ...a) => addTimer(f, 0, a, false),
    getComputedStyle(el, pseudo) {
        if (pseudo) return new Proxy({}, {get: (t, k) => k === 'getPropertyValue' ? () => '' : k === 'content' ? 'none' : ''});
        const id = el && el._i;
        return new Proxy({}, {
            get(t, k) {
                if (typeof k !== 'string' || !id) return undefined;
                if (k === 'getPropertyValue') return p => this.get(t, kebab2camel(String(p)));
                if (k === 'getPropertyPriority') return () => '';
                if (k === 'length') return 0;
                const p = k.startsWith('--') ? k : camel2kebab(k);
                if (p === 'display') return displayOf(id);
                const v = N.cssProp(id, p);
                if (v) return v;
                if (p === 'visibility') return 'visible';
                if (p === 'position') return 'static';
                if (p === 'opacity') return '1';
                if (p === 'width' || p === 'height') return rect(el)[p] + 'px';
                if (/^(margin|padding)/.test(p) || /width$/.test(p)) return '0px';
                return '';
            },
        });
    },
    matchMedia: q => new MediaQueryList(q),
    scroll() {}, scrollTo() {}, scrollBy() {}, focus() {}, blur() {}, print() {}, stop() {},
    alert(m) { N.log(1, 'alert: ' + m); }, confirm() { return false; }, prompt() { return null; },
    open(u) { N.log(1, 'window.open: ' + u); return null; }, close() {},
    postMessage(data) { setTimeout(() => G.dispatchEvent(new MessageEvent('message', {data: data})), 0); },
    getSelection() { return document.getSelection(); },
    fetch: fetch, XMLHttpRequest: XMLHttpRequest, Image: Image, Option: function (t, v) { const o = document.createElement('option'); if (t !== undefined) o.text = t; if (v !== undefined) o.value = v; return o; },
    URL: URL, URLSearchParams: URLSearchParams, webkitURL: URL,
    CSS: {supports(p, v) { return v === undefined ? !/^selector\(/.test(String(p)) : true; }, escape: cssIdent},
    Event, CustomEvent, UIEvent, MouseEvent, KeyboardEvent, FocusEvent, InputEvent, PointerEvent, WheelEvent,
    ErrorEvent, ProgressEvent, MessageEvent, PopStateEvent, HashChangeEvent, AnimationEvent, TransitionEvent, StorageEvent,
    EventTarget, Node, Element, HTMLElement, CharacterData, Text, Comment, Document, HTMLDocument: Document,
    DocumentFragment, DocumentType, Attr, DOMTokenList, DOMException, SVGElement, SVGSVGElement,
    HTMLAnchorElement, HTMLAreaElement, HTMLLinkElement, HTMLImageElement, HTMLSourceElement, HTMLScriptElement,
    HTMLStyleElement, HTMLFormElement, HTMLInputElement, HTMLButtonElement, HTMLTextAreaElement, HTMLOptionElement,
    HTMLSelectElement, HTMLTemplateElement, HTMLIFrameElement, HTMLCanvasElement, HTMLMediaElement, HTMLVideoElement,
    HTMLAudioElement, HTMLLabelElement, HTMLMetaElement, HTMLTableElement, HTMLTableRowElement, HTMLTableCellElement,
    HTMLDivElement, HTMLSpanElement, HTMLParagraphElement, HTMLHeadingElement, HTMLUListElement, HTMLOListElement,
    HTMLLIElement, HTMLBodyElement, HTMLHeadElement, HTMLHtmlElement, HTMLBRElement, HTMLHRElement, HTMLPreElement,
    HTMLTitleElement, HTMLUnknownElement,
    NodeList: Array, HTMLCollection: Array, NamedNodeMap: Array,
    Storage, MediaQueryList, IntersectionObserver, MutationObserver, ResizeObserver, PerformanceObserver,
    Window: function Window() {},
    CSSStyleDeclaration: function CSSStyleDeclaration() {},
    customElements: {define() {}, get() { return undefined; }, whenDefined() { return new Promise(() => {}); }},
    TextEncoder: class TextEncoder {
        get encoding() { return 'utf-8'; }
        encode(s) {
            s = unescape(encodeURIComponent(String(s === undefined ? '' : s)));
            const a = new Uint8Array(s.length);
            for (let i = 0; i < s.length; i++) a[i] = s.charCodeAt(i);
            return a;
        }
    },
    TextDecoder: class TextDecoder {
        constructor(enc) { this.encoding = (enc || 'utf-8').toLowerCase(); }
        decode(b) {
            if (!b) return '';
            const a = b instanceof Uint8Array ? b : new Uint8Array(b.buffer || b);
            let s = '';
            for (let i = 0; i < a.length; i++) s += String.fromCharCode(a[i]);
            try { return decodeURIComponent(escape(s)); } catch (e) { return s; }
        }
    },
    AbortController: class AbortController {
        constructor() { this.signal = new EventTarget(); this.signal.aborted = false; this.signal.reason = undefined; this.signal.throwIfAborted = () => {}; }
        abort(r) { if (this.signal.aborted) return; this.signal.aborted = true; this.signal.reason = r; this.signal.dispatchEvent(new Event('abort')); }
    },
    crypto: {
        getRandomValues(a) { for (let i = 0; i < a.length; i++) a[i] = Math.floor(Math.random() * 4294967296); return a; },
        randomUUID() { return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, c => { const r = Math.random() * 16 | 0; return (c === 'x' ? r : (r & 3 | 8)).toString(16); }); },
        subtle: undefined,
    },
    FormData: class FormData {
        constructor() { hide(this, '_p', []); }
        append(k, v) { this._p.push([String(k), String(v)]); }
        set(k, v) { this.delete(k); this.append(k, v); }
        get(k) { const x = this._p.find(p => p[0] === k); return x ? x[1] : null; }
        getAll(k) { return this._p.filter(p => p[0] === k).map(p => p[1]); }
        has(k) { return this._p.some(p => p[0] === k); }
        delete(k) { const a = this._p.filter(p => p[0] !== k); this._p.length = 0; this._p.push(...a); }
        entries() { return this._p[Symbol.iterator](); }
        [Symbol.iterator]() { return this.entries(); }
    },
    Blob: class Blob {
        constructor(parts, opt) { this.type = (opt && opt.type) || ''; hide(this, '_s', (parts || []).map(String).join('')); this.size = this._s.length; }
        text() { return Promise.resolve(this._s); }
        slice() { return new G.Blob([this._s]); }
    },
    structuredClone: v => v === undefined ? undefined : JSON.parse(JSON.stringify(v)),
    reportError: e => reportError(e),
    matchesSelector: undefined,
    onload: null, onerror: null, onresize: null, onscroll: null, onmessage: null, onpopstate: null, onhashchange: null,
    onbeforeunload: null, onunload: null, onpageshow: null, onfocus: null, onblur: null,
    /* как у браузеров: «'onsubmit' in window» — да (jQuery по ним решает, всплывают ли события) */
    onsubmit: null, onchange: null, onclick: null, oninput: null, onkeydown: null, onkeyup: null, onmousedown: null,
    onmouseup: null, onmousemove: null, onreset: null, onselect: null,
};
for (const k of Object.keys(win)) {
    const d = Object.getOwnPropertyDescriptor(win, k);
    Object.defineProperty(G, k, {value: d.value, writable: true, configurable: true, enumerable: false});
}
Object.defineProperty(G, 'location', {get: () => location, set: v => navigate(String(v), false), configurable: true});
const WP = Object.create(EventTarget.prototype);         /* window instanceof Window, EventTarget */
G.Window.prototype = WP;
try { Object.setPrototypeOf(G, WP); } catch (e) { Object.assign(G, ET); }
for (const k of Object.keys(ET)) if (typeof G[k] !== 'function') hide(G, k, ET[k]);
Object.defineProperty(G, 'name', {get: () => '', set: () => {}, configurable: true});
if (G.performance && !G.performance.getEntriesByType) {
    G.performance.getEntriesByType = () => [];
    G.performance.getEntriesByName = () => [];
    G.performance.mark = () => {};
    G.performance.measure = () => {};
    G.performance.timing = {navigationStart: 0};
}

/* --- хуки для js.c ---------------------------------------------------------------------------- */
return {
    scriptStart(id) {
        current = id;
        hide(wrap(id), '_started', true);
        const p = N.parent(id);
        writeAt = p ? [p, N.next(id)] : null;
    },
    scriptEnd() { current = 0; writeAt = null; },
    ready() {
        readyState = 'interactive';
        document.dispatchEvent(new Event('readystatechange'));
        document.dispatchEvent(new Event('DOMContentLoaded', {bubbles: true}));
    },
    load() {
        readyState = 'complete';
        document.dispatchEvent(new Event('readystatechange'));
        const ev = new Event('load');
        if (typeof G.onload !== 'function' && N.body()) {    /* <body onload="…"> — это window.onload */
            const h = attrHandler(wrap(N.body()), 'load');
            if (h) G.onload = h;
        }
        ET.dispatchEvent.call(G, ev);
        G.dispatchEvent(new Event('pageshow'));
    },
    tick(limit) { let n = 0; while (n < 200 && tick(limit)) n++; return n; },
};
})
