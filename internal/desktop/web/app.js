/* =============================================================================
   Beatport 下载器 · 桌面版前端
   -----------------------------------------------------------------------------
   与宿主 (Go + WebView2) 的全部通信都走 window.chrome.webview 消息桥：
     - rpc:  请求宿主调用 Go 后端 JSON-RPC（127.0.0.1 回环，由宿主进程发起）
     - host: 原生能力（选择文件夹、打开目录/文件、诊断信息等）
     - ui:   生命周期（ready / log / quit）
   页面本身没有 URL 端口、没有 fetch 网络请求，也不依赖任何第三方库。
   ========================================================================== */
(() => {
  'use strict';

  /* ---------------------------------------------------------------- utils */

  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));
  const clamp = (n, lo, hi) => Math.max(lo, Math.min(hi, n));

  const escHtml = (s) => String(s == null ? '' : s)
    .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;').replace(/'/g, '&#39;');

  const icon = (name, cls = 'ic') => `<svg class="${cls}"><use href="#i-${name}"/></svg>`;

  const pad2 = (n) => String(n).padStart(2, '0');
  const fmtClock = (d = new Date()) => `${pad2(d.getHours())}:${pad2(d.getMinutes())}:${pad2(d.getSeconds())}`;

  function fmtBytes(n) {
    if (!n || n < 0) return '';
    const units = ['B', 'KB', 'MB', 'GB'];
    let i = 0, v = n;
    while (v >= 1024 && i < units.length - 1) { v /= 1024; i++; }
    return `${v >= 100 || i === 0 ? Math.round(v) : v.toFixed(1)} ${units[i]}`;
  }

  function fmtSpeed(n) {
    const value = Number(n) || 0;
    return value > 0 ? `${fmtBytes(value)}/s` : '0 B/s';
  }

  function hashString(s) {
    let h = 2166136261;
    for (let i = 0; i < s.length; i++) { h ^= s.charCodeAt(i); h = Math.imul(h, 16777619); }
    return h >>> 0;
  }

  // Deterministic gradient cover so every row has a distinct, stable artwork
  // placeholder without downloading anything.
  function coverStyle(seed) {
    const h = hashString(seed || 'x');
    const hue = h % 360;
    const hue2 = (hue + 38 + (h >> 9) % 24) % 360;
    return `--c1:hsl(${hue} 46% 30%);--c2:hsl(${hue2} 62% 12%)`;
  }

  function coverLetter(title) {
    const t = String(title || '').trim();
    return t ? t[0].toUpperCase() : '♪';
  }

  function isEditable(target) {
    if (!target) return false;
    const tag = target.tagName;
    return tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT' || target.isContentEditable;
  }

  function dateIso(d) {
    return `${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())}`;
  }

  function relTime(ts) {
    if (!ts) return '';
    const diff = Date.now() - ts;
    if (diff < 60000) return '刚刚';
    if (diff < 3600000) return `${Math.floor(diff / 60000)} 分钟前`;
    if (diff < 86400000) return `${Math.floor(diff / 3600000)} 小时前`;
    return `${Math.floor(diff / 86400000)} 天前`;
  }

  const store = {
    get(key, fallback) {
      try {
        const raw = localStorage.getItem(key);
        return raw == null ? fallback : JSON.parse(raw);
      } catch { return fallback; }
    },
    set(key, value) {
      try { localStorage.setItem(key, JSON.stringify(value)); } catch { /* quota */ }
    },
  };

  /* --------------------------------------------------------------- bridge */

  const bridge = {
    available: !!(window.chrome && window.chrome.webview),
    seq: 0,
    pending: new Map(),
  };

  function rawPost(msg) {
    if (bridge.available) {
      window.chrome.webview.postMessage(JSON.stringify(msg));
    } else {
      Mock.receive(msg);
    }
  }

  function rpc(method, params) {
    return new Promise((resolve, reject) => {
      const id = ++bridge.seq;
      bridge.pending.set(id, { resolve, reject });
      rawPost({ kind: 'rpc', id, method, params: params || {} });
      setTimeout(() => {
        if (bridge.pending.has(id)) {
          bridge.pending.delete(id);
          reject(new Error('后端响应超时，请稍后重试'));
        }
      }, method === 'getLatest' || method === 'search' ? 185000 : 65000);
    });
  }

  function host(action, extra, timeoutMs) {
    return new Promise((resolve, reject) => {
      const id = ++bridge.seq;
      bridge.pending.set(id, { resolve, reject });
      rawPost(Object.assign({ kind: 'host', id, action }, extra || {}));
      setTimeout(() => {
        if (bridge.pending.has(id)) {
          bridge.pending.delete(id);
          reject(new Error('宿主操作超时'));
        }
      }, timeoutMs || 120000);
    });
  }

  function uiLog(level, message) {
    rawPost({ kind: 'ui', action: 'log', level, message: String(message).slice(0, 2000) });
  }

  function onHostMessage(msg) {
    if (!msg || typeof msg !== 'object') return;
    switch (msg.kind) {
      case 'hello':
        S.hello = msg.data || {};
        if (S.hello.username) {
          S.username = S.hello.username;
          store.set('bpdl.username', S.username);
        }
        S.update.current = S.hello.appVersion || S.update.current;
        renderChrome();
        renderSettings();
        break;
      case 'backend':
        setBackendState(msg.state, msg.message);
        break;
      case 'update':
        handleUpdateEvent(msg);
        break;
      case 'events.status':
        setEventsConnected(!!msg.connected, msg.message);
        break;
      case 'events.batch':
        handleEvents(msg.events || []);
        break;
      case 'rpc.result': {
        const p = bridge.pending.get(msg.id);
        if (p) { bridge.pending.delete(msg.id); p.resolve(msg.result); }
        break;
      }
      case 'rpc.error': {
        const p = bridge.pending.get(msg.id);
        if (p) { bridge.pending.delete(msg.id); p.reject(new Error((msg.error && msg.error.message) || '未知错误')); }
        break;
      }
      case 'host.result': {
        const p = bridge.pending.get(msg.id);
        if (p) { bridge.pending.delete(msg.id); p.resolve(msg.result); }
        break;
      }
      case 'host.error': {
        const p = bridge.pending.get(msg.id);
        if (p) { bridge.pending.delete(msg.id); p.reject(new Error((msg.error && msg.error.message) || '操作失败')); }
        break;
      }
      case 'toast':
        toast(msg.text || '', msg.level || 'info');
        break;
      default:
        break;
    }
  }

  /* ------------------------------------------------------------------ state */

  const S = {
    hello: null,
    backend: 'starting',
    backendMessage: '',
    eventsConnected: false,
    view: store.get('bpdl.view', 'discover'),
    genres: [],
    genresLoaded: false,
    genresLoading: false,
    genresWantRefresh: false,
    genreRetries: 0,
    authReady: null,
    username: store.get('bpdl.username', ''),
    bootstrapped: false,
    bootstrapLoading: false,
    discover: {
      kind: 'tracks',
      perPage: (() => {
        const saved = Number(store.get('bpdl.discover.perPage', 50));
        return [50, 100, 200].includes(saved) ? saved : 50;
      })(),
      genres: [],
      subgenres: [],
      genreTab: 'genre',
      preset: 'latest',
      from: '',
      to: '',
      page: 0,
      hasMore: false,
      loading: false,
      error: '',
      items: [],
      seen: new Set(),
      selected: new Set(),
      anchor: -1,
    },
    search: {
      query: '',
      items: [],
      selected: new Set(),
      anchor: -1,
      loading: false,
      error: '',
    },
    settings: {
      quality: '',
      trackExists: '',
      template: '',
      dir: '',
      writable: true,
      dirError: '',
      naming: { number: false, artists: false, mix: false },
      network: {
        proxy: '',
        sortByGenre: false,
        genreSortMode: 'genre',
        configPath: '',
        ownsBackend: true,
      },
    },
    update: {
      checking: false,
      applying: false,
      hasUpdate: false,
      current: '',
      latest: '',
      name: '',
      notes: '',
      publishedAt: '',
      releaseUrl: '',
      repo: '',
      asset: '',
      fullAsset: '',
      checkedAt: '',
      error: '',
      state: 'idle',
      progress: 0,
      stage: '',
      received: 0,
      total: 0,
    },
    catalog: {
      loaded: false,
      loading: false,
      error: '',
      items: [],
    },
    scheduler: {
      running: false,
      seconds: 86400,
      genres: [],
      subgenres: [],
      genreTab: 'genre',
      logs: [],
    },
    downloads: {
      map: new Map(),
      filter: 'all',
      restored: 0,
      nextSeq: 0,
      selected: new Set(),
      anchor: -1,
      pausedAll: false,
      maxActive: 10,
      pumping: false,
      expansions: [],
      stoppingAll: false,
      maintenance: false,
      // URLs whose late SSE events must be ignored (deleted or stopped rows).
      muted: new Set(),
    },
    navRail: store.get('bpdl.rail', false),
    theme: (() => {
      const forced = new URLSearchParams(location.search).get('theme');
      if (forced) return normalizeTheme(forced);
      return normalizeTheme(store.get('bpdl.theme', 'mecha'));
    })(),
  };

  function normalizeTheme(theme) {
    if (theme === 'classic') return 'classic';
    if (theme === 'mecha-light' || theme === 'light') return 'mecha-light';
    return 'mecha';
  }

  function applyTheme(theme, persist = true) {
    const next = normalizeTheme(theme);
    const light = next === 'mecha-light';
    document.documentElement.dataset.theme = next === 'classic' ? 'classic' : 'mecha';
    document.documentElement.dataset.mechaVariant = light ? 'light' : 'dark';
    S.theme = next;
    if (persist) store.set('bpdl.theme', next);
    $$('#theme-grid .theme-choice').forEach((btn) => {
      const on = btn.dataset.themeChoice === next;
      btn.classList.toggle('active', on);
      btn.setAttribute('aria-pressed', on ? 'true' : 'false');
    });
    const current = $('#theme-current');
    if (current) {
      current.textContent = next === 'mecha-light' ? 'MECHANICAL LIGHT' : next === 'classic' ? 'CLASSIC' : 'MECHANICAL';
    }
    host('setWindowAppearance', { params: { light: light } }).catch(() => {});
  }


  let dirty = new Set();
  let rafPending = false;

  function markDirty(...views) {
    views.forEach((v) => dirty.add(v));
    if (!rafPending) {
      rafPending = true;
      requestAnimationFrame(() => {
        rafPending = false;
        const viewsToRender = new Set(dirty);
        dirty = new Set();
        if (viewsToRender.has('chrome')) renderChrome();
        if (viewsToRender.has('discover')) renderDiscover();
        if (viewsToRender.has('search')) renderSearch();
        if (viewsToRender.has('downloads')) renderDownloads();
        if (viewsToRender.has('scheduler')) renderScheduler();
        if (viewsToRender.has('settings')) renderSettings();
      });
    }
  }

  /* ---------------------------------------------------------------- toasts */

  function toast(text, type = 'info', timeout = 4200) {
    const stack = $('#toast-stack');
    if (!stack || !text) return;
    const icons = { info: 'info', success: 'check', error: 'alert', warn: 'alert' };
    const node = document.createElement('div');
    node.className = `toast ${type}`;
    node.innerHTML = `${icon(icons[type] || 'info')}<div class="toast-text">${escHtml(text)}</div>
      <button class="icon-btn" type="button" aria-label="关闭">${icon('x')}</button>`;
    const close = () => { node.style.opacity = '0'; node.style.transform = 'translateY(6px)'; setTimeout(() => node.remove(), 160); };
    node.querySelector('.icon-btn').addEventListener('click', close);
    stack.appendChild(node);
    while (stack.children.length > 4) stack.firstElementChild.remove();
    setTimeout(close, timeout);
  }

  /* ------------------------------------------------------------------ modal */

  const modalLayer = () => $('#modal-layer');

  function closeModal() {
    const layer = modalLayer();
    if (!layer) return;
    layer.hidden = true;
    layer.innerHTML = '';
  }

  function openModal(html, { wide = false, closable = true, onClose = null } = {}) {
    const layer = modalLayer();
    if (!layer) return null;
    layer.hidden = false;
    layer.innerHTML = `<div class="modal${wide ? ' wide' : ''}" role="dialog" aria-modal="true">${html}</div>`;
    const card = layer.firstElementChild;
    if (closable) {
      layer.addEventListener('click', (e) => {
        if (e.target === layer) { closeModal(); if (onClose) onClose(); }
      }, { once: true });
      $$('[data-close]', card).forEach((b) => b.addEventListener('click', () => {
        closeModal();
        if (onClose) onClose();
      }));
    }
    const focusable = card.querySelector('input, select, button');
    if (focusable) setTimeout(() => focusable.focus(), 30);
    return card;
  }

  function confirmDialog({ title, message, confirmText = '确定', cancelText = '取消', danger = false }) {
    return new Promise((resolve) => {
      const card = openModal(`
        <div class="modal-head"><div><h2>${escHtml(title)}</h2></div></div>
        <div class="confirm-text">${escHtml(message)}</div>
        <div class="modal-actions">
          <button class="btn ghost" data-close type="button">${escHtml(cancelText)}</button>
          <button class="btn ${danger ? 'danger-ghost' : 'primary'}" data-ok type="button">${escHtml(confirmText)}</button>
        </div>`, {
        onClose: () => resolve(false),
      });
      if (!card) return resolve(false);
      $('[data-ok]', card).addEventListener('click', () => { closeModal(); resolve(true); });
    });
  }

  /* --------------------------------------------------------------- login UI */

  function showLoginModal(auto = false) {
    if (!modalLayer().hidden) return;
    const remembered = S.username || '';
    const card = openModal(`
      <div class="modal-head">
        <span class="brand-mark big">${icon('logo')}</span>
        <div>
          <h2>登录 Beatport</h2>
          <p>使用 Beatport / Beatsource 流媒体账号凭据。密码只写入本机配置文件。</p>
        </div>
      </div>
      <form class="login-form" autocomplete="off">
        <div class="input-wrap">
          <label for="login-user">用户名 / 邮箱</label>
          <input class="input" id="login-user" type="text" value="${escHtml(remembered)}" placeholder="Beatport 用户名" spellcheck="false">
        </div>
        <div class="input-wrap">
          <label for="login-pass">密码</label>
          <input class="input" id="login-pass" type="password" placeholder="Beatport 密码">
        </div>
        <div class="login-row">
          <label class="cb-label"><input type="checkbox" id="login-remember" ${remembered ? 'checked' : ''}><span>记住账号</span></label>
          <span class="tiny muted">密码不会单独保存</span>
        </div>
        <div class="login-error" role="alert"></div>
        <div class="login-actions">
          ${auto ? '' : '<button class="btn ghost" data-close type="button">取消</button>'}
          <button class="btn primary" type="submit">登录</button>
        </div>
      </form>
      <div class="login-hint">登录成功后凭据写入 <code>beatportdl-config.yml</code>，并生成本地认证缓存；下次启动自动登录。</div>`, {
      onClose: () => {},
    });
    if (!card) return;
    const form = $('form', card);
    const user = $('#login-user', card);
    const pass = $('#login-pass', card);
    const errEl = $('.login-error', card);
    const submit = $('button[type="submit"]', card);
    setTimeout(() => { (remembered ? pass : user).focus(); }, 40);

    form.addEventListener('submit', async (e) => {
      e.preventDefault();
      const u = user.value.trim();
      const p = pass.value;
      if (!u || !p) { errEl.textContent = '用户名和密码不能为空'; return; }
      submit.disabled = true;
      submit.textContent = '登录中…';
      errEl.textContent = '';
      try {
        await rpc('login', { username: u, password: p });
        S.authReady = true;
        S.username = u;
        store.set('bpdl.username', u);
        localStorage.setItem('bpdl.username.raw', u);
        closeModal();
        toast('登录成功，欢迎回来', 'success');
        markDirty('chrome', 'settings');
        await afterLogin();
      } catch (err) {
        errEl.textContent = err.message || '登录失败';
      } finally {
        submit.disabled = false;
        submit.textContent = '登录';
      }
    });
  }

  async function afterLogin() {
    try {
      S.genreRetries = 0;
      await Promise.all([loadGenres(true, false), loadSettings(), loadNetworkSettings()]);
      await refreshScheduler();
      markDirty('chrome', 'discover', 'settings', 'scheduler');
      if (!S.discover.items.length && !S.discover.loading) loadDiscover(true);
    } catch (err) {
      toast(err.message, 'error');
    }
  }

  /* ------------------------------------------------------------ virtual list */

  function createVirtualList({ scroll, spacer, rows, rowHeight, renderRow }) {
    let items = [];
    let rendered = new Map();
    let ticking = false;

    function layout() {
      ticking = false;
      spacer.style.height = `${items.length * rowHeight}px`;
      if (!items.length) {
        rows.innerHTML = '';
        rendered.clear();
        return;
      }
      const viewport = scroll.clientHeight || 480;
      const top = scroll.scrollTop;
      const start = clamp(Math.floor(top / rowHeight) - 5, 0, Math.max(0, items.length - 1));
      const end = clamp(Math.ceil((top + viewport) / rowHeight) + 5, 0, items.length - 1);

      for (const [idx, node] of Array.from(rendered)) {
        if (idx < start || idx > end) { node.remove(); rendered.delete(idx); }
      }
      const frag = document.createDocumentFragment();
      for (let i = start; i <= end; i++) {
        let node = rendered.get(i);
        if (!node) {
          node = document.createElement('div');
          node.dataset.index = String(i);
          rendered.set(i, node);
          frag.appendChild(node);
        }
        try {
          renderRow(node, items[i], i);
        } catch (err) {
          window.__lastRenderError = String(err && (err.stack || err.message || err));
          node.innerHTML = `<span class="muted">渲染失败：${escHtml(err && err.message ? err.message : err)}</span>`;
        }
        node.style.transform = `translateY(${i * rowHeight}px)`;
      }
      if (frag.childNodes.length) rows.appendChild(frag);
    }

    function requestLayout() {
      if (ticking) return;
      ticking = true;
      requestAnimationFrame(layout);
    }

    scroll.addEventListener('scroll', requestLayout, { passive: true });
    window.addEventListener('resize', requestLayout);

    return {
      // Data changes render synchronously so the list is never briefly empty
      // (and Ctrl+A/row clicks act on real rows immediately).
      setItems(next) { items = next; layout(); },
      refresh() { layout(); },
      getItems() { return items; },
      scrollToTop() { scroll.scrollTop = 0; layout(); },
    };
  }

  /* --------------------------------------------------------- row templates */

  function checkboxHtml(checked) {
    return `<label class="cb"><input type="checkbox" tabindex="-1" ${checked ? 'checked' : ''} aria-label="选择"><span></span></label>`;
  }

  function typeTag(item) {
    const t = item.type || 'track';
    if (t === 'release') return '<span class="type-tag release">专辑</span>';
    if (t === 'label') return '<span class="type-tag label">厂牌</span>';
    return '';
  }

  function rowSignature(item, selected, index) {
    return `${index}|${selected ? 1 : 0}|${item.url}|${item.title || item.name}|${item.genre || ''}|${item.subgenre || ''}|${item.artists || ''}|${item.cover || ''}`;
  }

  function renderResultRow(node, item, index, listId) {
    const selected = currentSelection(listId).has(item.url);
    const sig = rowSignature(item, selected, index);
    if (node.dataset.sig === sig) { node.className = `row${selected ? ' selected' : ''}`; return; }
    node.dataset.sig = sig;
    node.className = `row${selected ? ' selected' : ''}`;
    const name = item.name || item.title || '(未命名)';
    const mix = item.mix_name && item.mix_name !== name ? item.mix_name : '';
    const subParts = [];
    if (mix) subParts.push(mix);
    if (item.id) subParts.push(`#${item.id}`);
    const genre = item.genre || '';
    const subgenre = item.subgenre || '';
    const genreText = subgenre ? `${subgenre} · ${genre}` : genre;
    const titleTag = listId === 'search' ? '' : typeTag(item);
    const genreCell = listId === 'search'
      ? `${typeTag(item) || '<span class="muted">曲目</span>'}`
      : (genreText ? `<span class="truncate" title="${escHtml(genreText)}">${escHtml(genreText)}</span>` : '<span class="muted">—</span>');
    node.innerHTML = `
      <span class="cell-cb">${checkboxHtml(selected)}</span>
      <span class="cell-idx">${String(index + 1).padStart(2, '0')}</span>
      <span class="track-cell">
        <span class="cover-wrap">
          <span class="cover" style="${coverStyle(item.url || name)}">${item.cover ? `<img alt="" src="${escHtml(item.cover)}" loading="lazy" decoding="async" referrerpolicy="no-referrer" onerror="this.remove()">` : `<span class="cover-letter">${escHtml(coverLetter(name))}</span>`}</span>
        </span>
        <span class="track-text">
          <strong>${escHtml(name)}${titleTag}</strong>
          <em>${escHtml(subParts.join(' · '))}</em>
        </span>
      </span>
      <span class="cell-artists">${escHtml(item.artists || '—')}</span>
      <span class="cell-genre">${genreCell}</span>
      <span class="cell-actions">
        <button class="icon-btn" type="button" data-act="enqueue" title="加入下载队列">${icon('download')}</button>
        <button class="icon-btn" type="button" data-act="open" title="在浏览器打开">${icon('external')}</button>
      </span>`;
  }

  /* ------------------------------------------------------------- selection */

  function listState(listId) {
    return listId === 'discover' ? S.discover : S.search;
  }

  function currentSelection(listId) {
    return listState(listId).selected;
  }

  function selectedCount(listId) {
    // Selection only ever contains URLs that exist in the current item list
    // (lists clear it when they are replaced), so a size lookup is exact and
    // cheap enough to call from the frequent chrome re-renders.
    return listState(listId).selected.size;
  }

  function toggleRow(listId, index, shift) {
    const st = listState(listId);
    const item = st.items[index];
    if (!item) return;
    if (shift && st.anchor >= 0 && st.anchor !== index) {
      const target = !st.selected.has(item.url);
      const lo = Math.min(st.anchor, index), hi = Math.max(st.anchor, index);
      for (let i = lo; i <= hi; i++) {
        const it = st.items[i];
        if (!it) continue;
        if (target) st.selected.add(it.url); else st.selected.delete(it.url);
      }
    } else {
      if (st.selected.has(item.url)) st.selected.delete(item.url);
      else st.selected.add(item.url);
      st.anchor = index;
    }
    markDirty(listId, 'chrome');
    if (S.view === listId) window.__vl?.[listId]?.refresh();
    updateSelectionDock();
  }

  function setAllSelected(listId, value) {
    const st = listState(listId);
    st.selected.clear();
    if (value) st.items.forEach((it) => { if (it.url) st.selected.add(it.url); });
    st.anchor = -1;
    markDirty(listId);
    window.__vl?.[listId]?.refresh();
    updateSelectionDock();
  }

  function updateSelectionDock() {
    const listId = S.view === 'search' ? 'search' : 'discover';
    const dock = $('#selection-dock');
    if (!dock) return;
    const n = (S.view === 'discover' || S.view === 'search') ? selectedCount(listId) : 0;
    dock.hidden = n === 0;
    const app = $('#app');
    if (app) app.classList.toggle('has-selection', n > 0);
    if (n > 0) {
      $('#dock-count').textContent = String(n);
      dock.dataset.list = listId;
    }
  }

  function currentListId() {
    if (S.view === 'search') return 'search';
    return 'discover';
  }

  function isTrackUrl(url) {
    return /\/track\//.test(url) || /\/catalog\/tracks\//.test(url);
  }

  function isNonTerminal(row) {
    return row.status === 'waiting' || row.status === 'paused'
      || row.status === 'queued' || row.status === 'downloading';
  }

  async function enqueueUrls(urls, titles) {
    if (!urls.length) { toast('请先勾选要下载的曲目', 'warn'); return; }
    const now = Date.now();
    const tracks = [];
    const expansions = [];
    urls.forEach((url) => { (isTrackUrl(url) ? tracks : expansions).push(url); });

    let added = 0;
    tracks.forEach((url) => {
      const existing = S.downloads.map.get(url);
      if (existing && isNonTerminal(existing)) return;
      S.downloads.muted.delete(url);
      setDownloadRow(url, {
        url,
        title: (titles && titles[url]) || (existing && existing.title) || url,
        status: 'waiting',
        percent: 0,
        received: 0,
        total: 0,
        speed: 0,
        lastSample: null,
        error: '',
        reason: '',
        path: '',
        detail: '',
        seq: nextDownloadSeq(),
        addedAt: now,
        updatedAt: now,
      });
      added++;
    });

    // Albums / labels / playlists expand into child tracks inside the backend
    // and cannot be paused item-by-item; dispatch them as a separate job.
    expansions.forEach((url) => {
      if (!S.downloads.expansions.includes(url)) S.downloads.expansions.push(url);
    });

    markDirty('downloads', 'chrome');
    window.__vl?.downloads?.setItems(filteredDownloads());
    if (added > 0) {
      toast(`已加入下载队列 ${added} 首${expansions.length ? `，另有 ${expansions.length} 个展开任务` : ''}`, 'success');
    }
    pumpDownloads();
  }

  // pumpDownloads is the local scheduler in front of the backend. It only
  // submits a limited number of track URLs at a time, which is what makes
  // pause/delete truthful without touching the (unmodifiable) backend.
  async function pumpDownloads() {
    if (S.downloads.pumping) return;
    if (S.downloads.pausedAll || S.downloads.stoppingAll || S.downloads.maintenance) return;
    if (S.update.applying) return;
    if (S.backend !== 'ready' || S.authReady === false) return;
    S.downloads.pumping = true;
    try {
      // Expansion jobs first; they are not individually trackable.
      if (S.downloads.expansions.length) {
        const batch = S.downloads.expansions.splice(0, S.downloads.expansions.length);
        try {
          await rpc('download', { urls: batch });
        } catch (err) {
          S.downloads.expansions.unshift(...batch);
          toast(`提交专辑/歌单任务失败：${err.message}`, 'error');
          return;
        }
      }

      while (!S.downloads.pausedAll && S.backend === 'ready' && S.authReady !== false) {
        const rows = Array.from(S.downloads.map.values());
        const active = rows.filter((r) => r.status === 'queued' || r.status === 'downloading').length;
        const slots = S.downloads.maxActive - active;
        if (slots <= 0) break;
        const batch = rows
          .filter((r) => r.status === 'waiting')
          .sort((a, b) => (a.seq || 0) - (b.seq || 0))
          .slice(0, slots);
        if (!batch.length) break;
        batch.forEach((r) => { r.status = 'queued'; r.updatedAt = Date.now(); });
        markDirty('downloads', 'chrome');
        window.__vl?.downloads?.refresh();
        try {
          const res = await rpc('download', { urls: batch.map((r) => r.url) });
          const skipped = (res && res.skipped) || 0;
          if (skipped > 0) {
            // Duplicate URLs are announced via download.skipped events; the
            // rows will be finalized there.
          }
        } catch (err) {
          batch.forEach((r) => { r.status = 'waiting'; r.error = ''; });
          markDirty('downloads');
          window.__vl?.downloads?.refresh();
          toast(`提交下载失败：${err.message}`, 'error', 6000);
          break;
        }
      }
    } finally {
      S.downloads.pumping = false;
    }
  }

  function interruptActiveDownloads(reason) {
    let changed = false;
    S.downloads.map.forEach((row) => {
      if (row.status === 'queued' || row.status === 'downloading') {
        row.status = 'error';
        row.error = reason;
        row.percent = 0;
        row.speed = 0;
        row.updatedAt = Date.now();
        changed = true;
      }
    });
    if (changed) {
      persistDownloads();
      markDirty('downloads', 'chrome');
      window.__vl?.downloads?.refresh();
    }
  }

  /* ------------------------------------------------------------------ nav */

  const PAGE_META = {
    discover: ['发现最新', '从 Beatport 曲库按曲风与日期发现新歌'],
    search: ['搜索', '搜索曲目、专辑与厂牌，或直接粘贴 Beatport 链接'],
    scheduler: ['定时任务', '按固定间隔自动扫描最新发布并加入下载'],
    downloads: ['下载队列', ''],
    settings: ['设置', '音频格式、命名方式、目录、账号与更新'],
  };

  function setView(view) {
    if (!PAGE_META[view]) return;
    S.view = view;
    document.body.dataset.view = view;
    store.set('bpdl.view', view);
    $$('.nav-item').forEach((b) => b.classList.toggle('active', b.dataset.view === view));
    $$('.view').forEach((v) => { v.hidden = v.dataset.view !== view; });
    updatePageHead();
    updateSelectionDock();
    // Render non-list pages immediately so a click never waits for a frame;
    // the virtual lists are refreshed in place.
    if (view === 'downloads') renderDownloads();
    if (view === 'settings') renderSettings();
    if (view === 'scheduler') renderScheduler();
    if (view === 'discover' || view === 'search') window.__vl?.[view]?.refresh();
  }

  // A short entrance animation only on explicit navigation, so screenshots and
  // first paint are always fully opaque.
  function pulseView(view) {
    const el = document.querySelector(`.view[data-view="${view}"]`);
    if (!el) return;
    el.classList.remove('entering');
    void el.offsetWidth;
    el.classList.add('entering');
    setTimeout(() => el.classList.remove('entering'), 260);
  }

  let topbarSignature = '';

  function updatePageHead() {
    const [title, subtitle] = PAGE_META[S.view] || PAGE_META.discover;
    $('#page-title').textContent = title;
    const subtitleEl = $('#page-subtitle');
    subtitleEl.textContent = subtitle;
    subtitleEl.hidden = !subtitle;
    const actions = $('#topbar-actions');
    if (!actions) return;
    // Rebuilding the toolbar on every 70 ms progress batch would thrash the
    // DOM (and drop focus); only touch it when it actually changes.
    let sig = S.view;
    let html = '';
    if (S.view === 'downloads') {
      const speed = totalDownloadSpeed();
      sig += `|${Math.round(speed)}`;
      html = `<span class="count-pill speed-pill" title="当前所有任务的总下载速度"><span class="speed-label">DL</span><strong>${escHtml(fmtSpeed(speed))}</strong></span>`;
    } else if (S.view === 'settings') {
      sig += `|${(S.hello && S.hello.appVersion) || ''}`;
      html = `<span class="count-pill">v${escHtml((S.hello && S.hello.appVersion) || '1.0.0')}</span>`;
    } else if (S.view === 'search') {
      html = '<kbd>Ctrl</kbd><span class="muted tiny">+</span><kbd>K</kbd>';
    }
    if (sig === topbarSignature) return;
    topbarSignature = sig;
    actions.innerHTML = html;
  }

  /* ---------------------------------------------------------------- update */

  function currentAppVersion() {
    return (S.hello && S.hello.appVersion) || S.update.current || '1.0.0';
  }

  function formatUpdateTime(value) {
    if (!value) return '';
    const d = new Date(value);
    if (Number.isNaN(d.getTime())) return '';
    return d.toLocaleString('zh-CN', { hour12: false });
  }

  function renderUpdateSlot() {
    const slot = $('#update-slot');
    if (!slot) return;
    const u = S.update;
    if (u.applying) {
      slot.innerHTML = '<span class="update-checking"><span class="pulse"></span>正在更新…</span>';
    } else if (u.hasUpdate) {
      slot.innerHTML = `<button class="update-pill" type="button" data-update-open title="发现新版本，点击查看更新说明">
        <span class="new-dot"></span><span>新版本 v${escHtml(u.latest)}</span>
      </button>`;
    } else if (u.checking) {
      slot.innerHTML = '<span class="update-checking"><span class="pulse"></span>检查更新…</span>';
    } else {
      slot.innerHTML = '';
    }
  }

  function applyUpdateInfo(info) {
    if (!info) return;
    const u = S.update;
    u.checking = false;
    u.error = '';
    u.current = info.current || currentAppVersion();
    u.latest = info.latest || '';
    u.hasUpdate = !!info.hasUpdate;
    u.name = info.name || '';
    u.notes = info.notes || '';
    u.publishedAt = info.publishedAt || '';
    u.releaseUrl = info.releaseUrl || '';
    u.repo = info.repo || u.repo;
    u.asset = info.asset || '';
    u.fullAsset = info.fullAsset || '';
    u.checkedAt = info.checkedAt || new Date().toISOString();
    u.state = u.hasUpdate ? 'available' : 'none';
    renderUpdateSlot();
    renderSettingsUpdate();
  }

  async function checkUpdatesOnStartup() {
    if (S.update.checking || S.update.applying) return;
    S.update.checking = true;
    S.update.error = '';
    renderUpdateSlot();
    renderSettingsUpdate();
    try {
      const info = await host('checkUpdate');
      applyUpdateInfo(info);
    } catch (err) {
      S.update.checking = false;
      S.update.error = err.message;
      uiLog('warn', `检查更新失败：${err.message}`);
      renderUpdateSlot();
      renderSettingsUpdate();
    }
  }

  async function checkUpdatesManually(showToast = true) {
    if (S.update.checking || S.update.applying) return;
    S.update.checking = true;
    S.update.error = '';
    renderUpdateSlot();
    renderSettingsUpdate();
    try {
      const info = await host('checkUpdate');
      applyUpdateInfo(info);
      if (showToast) {
        toast(info.hasUpdate ? `发现新版本 v${info.latest}` : '已是最新版本',
          info.hasUpdate ? 'success' : 'info');
      }
    } catch (err) {
      S.update.checking = false;
      S.update.error = err.message;
      renderUpdateSlot();
      renderSettingsUpdate();
      if (showToast) toast(`检查更新失败：${err.message}`, 'error', 7000);
    }
  }

  function updateProgressDom(percent, stage) {
    const value = Math.max(0, Math.min(100, Number(percent) || 0));
    S.update.progress = value;
    if (stage) S.update.stage = stage;
    const bar = $('#update-progress-bar');
    if (bar) bar.style.width = `${value}%`;
    const percentEl = $('#update-progress-percent');
    if (percentEl) percentEl.textContent = `${value}%`;
    const stageEl = $('#update-progress-stage');
    if (stageEl) stageEl.textContent = S.update.stage || '正在更新…';
  }

  function openUpdateProgressModal() {
    openModal(`
      <div class="modal-head">
        <span class="brand-mark big">${icon('download')}</span>
        <div><h2>正在更新 Beatport 下载器</h2><p>更新过程中请不要关闭程序，完成后会自动重启。</p></div>
      </div>
      <div class="update-progress">
        <div class="progress-track"><div class="progress-bar" id="update-progress-bar" style="width:0%"></div></div>
        <div class="update-progress-label">
          <span id="update-progress-stage">正在准备更新…</span>
          <span id="update-progress-percent">0%</span>
        </div>
      </div>
      <div class="update-error" id="update-progress-error" hidden></div>
      <div class="modal-actions" id="update-progress-actions" hidden>
        <button class="btn ghost" id="update-progress-close" type="button">关闭</button>
      </div>`, { wide: true, closable: false });
    const closeBtn = $('#update-progress-close');
    if (closeBtn) closeBtn.addEventListener('click', () => closeModal());
    updateProgressDom(0, '正在准备更新…');
  }

  function showUpdateError(message) {
    S.update.applying = false;
    closeModal();
    const card = openModal(`
      <div class="modal-head">
        <span class="brand-mark big">${icon('alert')}</span>
        <div><h2>更新未完成</h2><p>当前版本仍然可以正常使用。</p></div>
      </div>
      <div class="update-error">${escHtml(message || '更新失败')}</div>
      <div class="modal-actions">
        <button class="btn ghost" data-close type="button">关闭</button>
      </div>`, { wide: true });
    if (card) {
      const close = card.querySelector('[data-close]');
      if (close) close.addEventListener('click', closeModal);
    }
    renderUpdateSlot();
    renderSettingsUpdate();
    toast(message || '更新失败', 'error', 8000);
    pumpDownloads();
  }

  function showUpdateDialog() {
    const u = S.update;
    if (!u.hasUpdate) {
      checkUpdatesManually(true);
      return;
    }
    const published = formatUpdateTime(u.publishedAt);
    const notes = u.notes || '（此版本没有填写更新说明）';
    const card = openModal(`
      <div class="modal-head">
        <span class="brand-mark big">${icon('download')}</span>
        <div>
          <h2>发现新版本 v${escHtml(u.latest)}</h2>
          <p>当前版本 v${escHtml(currentAppVersion())}${u.repo ? ` · ${escHtml(u.repo)}` : ''}</p>
        </div>
      </div>
      <div class="update-modal-meta">
        <span>${published ? `发布于 ${escHtml(published)}` : 'GitHub Release'}</span>
        ${u.asset ? `<span>更新包：${escHtml(u.asset)}</span>` : ''}
      </div>
      <pre class="update-notes">${escHtml(notes)}</pre>
      <div class="hint">更新会关闭当前程序并自动重启。正在下载的任务会被中断；下载目录、账号和设置不会丢失。</div>
      <div class="modal-actions">
        <button class="btn ghost" data-close type="button">稍后</button>
        ${u.releaseUrl ? '<button class="btn ghost" id="update-open-release" type="button">发布页</button>' : ''}
        <button class="btn primary" id="update-start" type="button">${icon('download')}<span>立即更新</span></button>
      </div>`, { wide: true });
    if (!card) return;
    $('#update-start', card).addEventListener('click', () => startUpdate(false));
    const release = $('#update-open-release', card);
    if (release) release.addEventListener('click', () => {
      host('openExternal', { url: u.releaseUrl }).catch((err) => toast(err.message, 'error'));
    });
  }

  async function startUpdate(full = false) {
    if (S.update.applying) return;
    if (openTaskCount() > 0) {
      const ok = await confirmDialog({
        title: '更新会中断下载',
        message: `当前还有 ${openTaskCount()} 个未完成任务。\n`
          + '安装更新会关闭程序并重启后端，正在下载和排队中的任务都会被中断；\n'
          + '已经写了一半的文件可能留在下载目录。\n\n确定现在更新吗？',
        confirmText: '仍要更新',
        danger: true,
      });
      if (!ok) return;
    }
    S.update.applying = true;
    S.update.error = '';
    S.update.progress = 0;
    S.update.stage = '正在准备更新…';
    closeModal();
    openUpdateProgressModal();
    renderUpdateSlot();
    renderSettingsUpdate();
    try {
      const res = await host('applyUpdate', { full }, 60 * 60 * 1000);
      if (res && res.ok) {
        if (res.mock) {
          S.update.applying = false;
          closeModal();
          toast('预览模式：更新流程已模拟，不会重启', 'info', 4000);
          renderUpdateSlot();
          renderSettingsUpdate();
          return;
        }
        updateProgressDom(100, '更新已就绪，正在重启程序…');
        S.update.state = 'restarting';
        return;
      }
      S.update.applying = false;
      if (res && res.canFull) {
        const yes = await confirmDialog({
          title: '更新失败',
          message: `${res.message || '安装更新失败'}\n\n是否下载完整版重试？不会影响你的数据。`,
          confirmText: '下载完整版',
          danger: false,
        });
        if (yes) { startUpdate(true); return; }
      }
      showUpdateError((res && res.message) || '更新失败');
    } catch (err) {
      S.update.applying = false;
      showUpdateError(err.message);
    } finally {
      renderUpdateSlot();
      renderSettingsUpdate();
      if (!S.update.applying) pumpDownloads();
    }
  }

  function handleUpdateEvent(msg) {
    const u = S.update;
    if (!msg || !msg.state) return;
    if (msg.state === 'available' || msg.state === 'none') {
      if (msg.data) applyUpdateInfo(msg.data);
      return;
    }
    if (msg.state === 'error') {
      // The apply action returns a structured result; this event is only a
      // fallback for errors outside that flow.
      if (u.applying) return;
      u.error = msg.message || '更新失败';
      renderUpdateSlot();
      renderSettingsUpdate();
      return;
    }
    if (msg.state === 'downloading' || msg.state === 'applying' || msg.state === 'restarting') {
      u.applying = true;
      u.state = msg.state;
      const total = Number(msg.total) || 0;
      const received = Number(msg.received) || 0;
      let stage = msg.message || '';
      if (msg.state === 'downloading' && total > 0) {
        stage = `${stage || '正在下载更新包…'} ${fmtBytes(received)} / ${fmtBytes(total)}`;
      }
      updateProgressDom(msg.state === 'restarting' ? 100 : (Number(msg.percent) || 0), stage);
      renderUpdateSlot();
      renderSettingsUpdate();
    }
  }

  function renderSettingsUpdate() {
    const current = $('#update-current');
    if (!current) return;
    const u = S.update;
    const appVersion = currentAppVersion();
    $('#update-current').textContent = `v${appVersion}`;
    $('#update-latest').textContent = u.latest ? `v${u.latest}` : '—';
    let hint = '启动时会自动检查 GitHub Release。';
    let bad = false;
    if (u.applying) {
      hint = `正在更新：${u.stage || '准备中…'}（${Math.round(u.progress || 0)}%）`;
    } else if (u.checking) {
      hint = '正在连接 GitHub 检查最新版本…';
    } else if (u.error) {
      hint = `检查更新失败：${u.error}`;
      bad = true;
    } else if (u.hasUpdate) {
      hint = `发现新版本 v${u.latest}，点击「立即更新」开始下载。`;
    } else if (u.checkedAt) {
      hint = `已是最新版本（检查于 ${formatUpdateTime(u.checkedAt)}）。`;
    }
    const hintEl = $('#update-hint');
    if (hintEl) {
      hintEl.textContent = hint;
      hintEl.style.color = bad ? '#ff9fb0' : '';
    }
    const check = $('#update-check');
    if (check) check.disabled = !!u.checking || !!u.applying;
    const apply = $('#update-apply');
    if (apply) apply.hidden = !u.hasUpdate || !!u.applying;
    const release = $('#update-release');
    if (release) release.hidden = !u.releaseUrl;
  }


  function toggleRail() {
    S.navRail = !S.navRail;
    store.set('bpdl.rail', S.navRail);
    $('#app').classList.toggle('rail', S.navRail);
  }

  /* ---------------------------------------------------------------- chrome */

  function setupWindowbar() {
    const bar = $('#windowbar');
    if (!bar) return;
    bar.addEventListener('mousedown', (e) => {
      if (e.button !== 0 || e.target.closest('.win-actions')) return;
      const startX = e.clientX;
      const startY = e.clientY;
      let started = false;
      const cleanup = () => {
        document.removeEventListener('mousemove', onMove);
        window.removeEventListener('mouseup', onUp);
        window.removeEventListener('blur', onUp);
      };
      const onMove = (ev) => {
        if (started) return;
        if (Math.hypot(ev.clientX - startX, ev.clientY - startY) < 4) return;
        started = true;
        cleanup();
        host('windowDrag').catch(() => {});
      };
      const onUp = () => cleanup();
      document.addEventListener('mousemove', onMove, { passive: true });
      window.addEventListener('mouseup', onUp, { once: true });
      window.addEventListener('blur', onUp, { once: true });
    });
    bar.addEventListener('dblclick', (e) => {
      if (e.target.closest('.win-actions')) return;
      host('windowToggleMaximize').catch(() => {});
    });
    $$('.resize-handle').forEach((handle) => {
      handle.addEventListener('mousedown', (e) => {
        if (e.button !== 0) return;
        e.preventDefault();
        host('windowResizeStart', { params: { direction: handle.dataset.resize } }).catch(() => {});
      });
    });
    $('#win-min')?.addEventListener('click', () => host('windowMinimize').catch(() => {}));
    $('#win-max')?.addEventListener('click', () => host('windowToggleMaximize').catch(() => {}));
    $('#win-close')?.addEventListener('click', () => host('windowClose').catch(() => {}));
  }

  function renderChrome() {
    const pill = $('#backend-pill');
    if (pill) {
      pill.dataset.state = S.backend;
      const dot = $('#backend-dot');
      if (dot) dot.className = 'pulse';
    }
    const text = $('#backend-text');
    if (text) {
      text.textContent = {
        ready: '后端已连接',
        starting: '正在启动后端…',
        down: '后端未连接',
      }[S.backend] || '后端状态未知';
    }

    const banner = $('#conn-banner');
    const showBanner = S.backend === 'down';
    if (banner) banner.hidden = !showBanner;
    if (showBanner) {
      $('#conn-text').textContent = S.backendMessage || '与 Go 后端的连接已断开，下载与搜索暂时不可用。';
    }

    const accountName = $('#account-name');
    const accountSub = $('#account-sub');
    const name = S.username || 'Beatport 账号';
    if (accountName) accountName.textContent = S.authReady ? name : '未登录';
    if (accountSub) accountSub.textContent = S.authReady ? '已登录' : '点击登录账号';
    const accountBtn = $('#account-action');
    if (accountBtn) {
      accountBtn.title = S.authReady ? '账号设置' : '登录';
      accountBtn.innerHTML = icon(S.authReady ? 'user' : 'logout');
    }

    const active = activeDownloadCount();
    const openTasks = openTaskCount();
    const badge = $('#downloads-badge');
    if (badge) { badge.hidden = openTasks === 0; badge.textContent = String(openTasks); }
    const schedDot = $('#sched-dot');
    if (schedDot) schedDot.hidden = !S.scheduler.running;

    renderUpdateSlot();
    updatePageHead();
    updateSelectionDock();
  }

  /* --------------------------------------------------------------- handlers */

  function setBackendState(state, message) {
    const was = S.backend;
    S.backend = state || 'down';
    S.backendMessage = message || '';
    markDirty('chrome', 'settings');
    if (S.backend === 'ready' && was !== 'ready') {
      bootstrapData();
      if (!S.downloads.stoppingAll) pumpDownloads();
    }
    if (S.backend !== 'ready' && was === 'ready') {
      if (!S.downloads.stoppingAll) {
        interruptActiveDownloads('后端连接已断开，任务已中断');
      }
      if (S.backend === 'down') {
        toast('后端连接已断开，可点击左下角状态重连', 'error', 6000);
      }
    }
  }

  function setEventsConnected(connected, message) {
    S.eventsConnected = connected;
    if (!connected && message && S.backend === 'ready') {
      // The SSE channel reconnects on its own; keep the UI calm but log it.
      uiLog('warn', `事件通道：${message}`);
    }
  }

  async function bootstrapData() {
    if (S.bootstrapLoading) return;
    S.bootstrapLoading = true;
    try {
      await checkAuth();
      await Promise.all([loadGenres(), loadSettings(), loadNetworkSettings()]);
      await refreshScheduler();
      markDirty('chrome', 'discover', 'search', 'settings', 'scheduler');
      // Land on a populated page: the first successful backend handshake
      // loads the default "latest tracks" page once.
      if (!S.discoverAutoLoaded && S.authReady) {
        S.discoverAutoLoaded = true;
        if (!S.discover.items.length && !S.discover.loading) loadDiscover(true);
      }
    } catch (err) {
      uiLog('warn', `初始化数据失败：${err.message}`);
      toast(err.message, 'error');
    } finally {
      S.bootstrapLoading = false;
      S.bootstrapped = true;
    }
  }

  async function checkAuth() {
    try {
      const res = await rpc('authStatus');
      S.authReady = !!(res && res.ready);
      if (!S.authReady) {
        markDirty('chrome', 'settings');
        if (modalLayer().hidden) showLoginModal(true);
      }
      markDirty('chrome', 'settings');
    } catch (err) {
      S.authReady = false;
      markDirty('chrome', 'settings');
      uiLog('warn', `authStatus 失败：${err.message}`);
    }
  }

  async function loadGenres(force = false, notify = false) {
    if (S.genresLoading) {
      if (force) S.genresWantRefresh = true;
      return;
    }
    if (S.genresLoaded && !force) return;
    S.genresLoading = true;
    try {
      const res = await rpc('getGenres');
      const list = Array.isArray(res) ? res : [];
      const genres = list
        .map((g) => ({ name: (g && (g.name || g.Name)) || '', slug: (g && (g.slug || g.Slug)) || '' }))
        .filter((g) => g.name)
        .sort((a, b) => a.name.localeCompare(b.name, 'zh-Hans-CN'));
      if (genres.length) {
        S.genres = genres;
        S.genresLoaded = true;
        S.genreRetries = 0;
        renderGenreList();
        renderScheduler();
        markDirty('chrome');
      } else {
        // An empty list is not a successful load: the backend may still be
        // logging in or the proxy may be warming up. Keep the trigger honest
        // and retry instead of permanently showing "没有匹配的曲风".
        if (!S.genres.length) S.genresLoaded = false;
        renderGenreList();
        scheduleGenreRetry();
        if (notify) toast('曲风列表为空，正在重试…', 'warn');
      }
    } catch (err) {
      if (!S.genres.length) S.genresLoaded = false;
      uiLog('warn', `加载曲风失败：${err.message}`);
      renderGenreList();
      scheduleGenreRetry();
      if (notify) toast(`加载曲风失败：${err.message}`, 'warn');
    } finally {
      S.genresLoading = false;
      if (S.genresWantRefresh) {
        S.genresWantRefresh = false;
        setTimeout(() => loadGenres(true, false), 0);
      }
    }
  }

  function scheduleGenreRetry() {
    if (S.genreRetries >= 3) {
      S.genresLoaded = true;
      renderGenreList();
      return;
    }
    S.genreRetries += 1;
    setTimeout(() => {
      if (S.backend === 'ready' && !S.genres.length) loadGenres(true, false);
    }, 1200 * S.genreRetries);
  }

  async function refreshGenresAfterStartup() {
    // Make the startup check resilient: wait for the backend/auth handshake,
    // then force one fetch if the normal bootstrap came back empty.
    for (let i = 0; i < 40; i++) {
      if (S.backend === 'ready' && S.authReady !== null) break;
      await new Promise((resolve) => setTimeout(resolve, 500));
    }
    if (S.genres.length) return;
    S.genreRetries = 0;
    await loadGenres(true, false);
  }

  async function loadSettings() {
    try {
      const res = await rpc('getSettings');
      applySettingsPayload(res || {});
    } catch (err) {
      uiLog('warn', `读取设置失败：${err.message}`);
      toast(`读取设置失败：${err.message}`, 'error');
    }
  }

  async function loadNetworkSettings() {
    try {
      const res = await host('getNetworkSettings');
      const net = S.settings.network;
      net.proxy = (res && res.proxy) || '';
      net.sortByGenre = !!(res && res.sort_by_genre);
      net.genreSortMode = (res && res.genre_sort_mode) || 'genre';
      net.configPath = (res && res.configPath) || '';
      net.ownsBackend = !(res && res.ownsBackend === false);
      renderSettings();
    } catch (err) {
      uiLog('warn', `读取网络设置失败：${err.message}`);
    }
  }

  function applySettingsPayload(res) {
    const st = S.settings;
    st.quality = res.quality || st.quality;
    st.trackExists = res.track_exists || st.trackExists;
    st.dir = res.downloads_directory || st.dir;
    st.writable = res.writable !== false;
    st.dirError = res.error || '';
    st.template = res.track_file_template || '';
    st.naming = parseTemplate(st.template);
    markDirty('settings');
    if (!st.writable && st.dir) {
      toast(`下载目录不可写：${st.dirError || st.dir}`, 'error', 8000);
    }
    applyNamePreview();
  }

  function parseTemplate(tmpl) {
    return {
      number: String(tmpl || '').includes('{number}'),
      artists: String(tmpl || '').includes('{artists}'),
      mix: String(tmpl || '').includes('{mix_name}'),
    };
  }

  function buildTemplate(naming) {
    let t = '';
    if (naming.number) t += '{number}. ';
    if (naming.artists) t += '{artists} - ';
    t += '{name}';
    if (naming.mix) t += ' ({mix_name})';
    return t;
  }

  function applyNamePreview() {
    const el = $('#name-preview');
    if (!el) return;
    let demo = buildTemplate(S.settings.naming)
      .replace('{number}', '01')
      .replace('{artists}', 'Conspiracy 23')
      .replace('{name}', 'Silly Things Intro')
      .replace('{mix_name}', 'Original Mix');
    el.textContent = `${demo}.flac`;
  }

  /* -------------------------------------------------------------- discover */

  function setupDiscover() {
    const list = S.discover;
    const kindSeg = $('#kind-seg');
    kindSeg.addEventListener('click', (e) => {
      const btn = e.target.closest('.seg');
      if (!btn) return;
      list.kind = btn.dataset.kind;
      $$('.seg', kindSeg).forEach((b) => b.classList.toggle('active', b === btn));
      if (list.items.length) loadDiscover(true);
    });

    $('#genre-trigger').addEventListener('click', (e) => {
      e.stopPropagation();
      toggleGenrePopover();
    });
    $('#genre-search').addEventListener('input', renderGenreList);
    $('#genre-apply').addEventListener('click', () => toggleGenrePopover(false));
    $('#genre-clear').addEventListener('click', () => {
      list.genres = [];
      list.subgenres = [];
      renderGenreList();
      updateGenreTrigger();
    });
    document.addEventListener('mousedown', (e) => {
      const pop = $('#genre-popover');
      if (pop && !pop.hidden && !pop.contains(e.target) && e.target.id !== 'genre-trigger' && !e.target.closest('#genre-trigger')) {
        toggleGenrePopover(false);
      }
      const sched = $('#sched-genre-popover');
      if (sched && !sched.hidden && !sched.contains(e.target) && !e.target.closest('#sched-genre-trigger')) {
        toggleSchedulerGenrePopover(false);
      }
    });

    $('#date-chips').addEventListener('click', (e) => {
      const chip = e.target.closest('.chip');
      if (!chip) return;
      list.preset = chip.dataset.preset;
      $$('.chip', $('#date-chips')).forEach((c) => c.classList.toggle('active', c === chip));
      applyDatePreset(list.preset);
    });
    $('#date-today').addEventListener('click', () => {
      const today = dateIso(new Date());
      $('#date-from').value = today;
      $('#date-to').value = today;
      syncCustomRange();
    });
    ['#date-from', '#date-to'].forEach((sel) => {
      $(sel).addEventListener('change', syncCustomRange);
    });

    $('#discover-load').addEventListener('click', () => loadDiscover(true));
    $('#discover-page-size').addEventListener('click', (e) => {
      const btn = e.target.closest('[data-size]');
      if (!btn) return;
      const size = Number(btn.dataset.size);
      if (![50, 100, 200].includes(size) || size === S.discover.perPage) return;
      S.discover.perPage = size;
      store.set('bpdl.discover.perPage', size);
      $('#discover-scroll').scrollTop = 0;
      renderDiscover();
      loadDiscover(true);
    });
    $('#discover-select-all').addEventListener('click', () => setAllSelected('discover', true));
    $('#discover-clear').addEventListener('click', () => setAllSelected('discover', false));
    $('#discover-check-all').addEventListener('change', (e) => setAllSelected('discover', e.target.checked));

    const scroll = $('#discover-scroll');
    scroll.addEventListener('scroll', () => {
      if (!S.discover.hasMore || S.discover.loading) return;
      if (scroll.scrollTop + scroll.clientHeight >= scroll.scrollHeight - 260) loadDiscover(false);
    }, { passive: true });

    const rows = $('#discover-rows');
    rows.addEventListener('click', (e) => handleRowClick(e, 'discover'));

    window.__vl = window.__vl || {};
    window.__vl.discover = createVirtualList({
      scroll,
      spacer: $('#discover-spacer'),
      rows,
      rowHeight: 58,
      renderRow: (node, item, index) => renderResultRow(node, item, index, 'discover'),
    });

    // initial date value
    $('#date-from').value = dateIso(new Date(Date.now() - 7 * 86400000));
    $('#date-to').value = dateIso(new Date());
    updateGenreTrigger();
    applyDatePreset('latest');
  }

  function handleRowClick(e, listId) {
    const row = e.target.closest('.row');
    if (!row) return;
    const index = Number(row.dataset.index);
    const actBtn = e.target.closest('[data-act]');
    if (actBtn) {
      e.stopPropagation();
      const item = listState(listId).items[index];
      if (!item) return;
      if (actBtn.dataset.act === 'enqueue') {
        enqueueUrls([item.url], { [item.url]: displayTitle(item) });
      } else if (actBtn.dataset.act === 'open') {
        host('openExternal', { url: item.url }).catch((err) => toast(err.message, 'error'));
      }
      return;
    }
    toggleRow(listId, index, e.shiftKey);
  }

  function displayTitle(item) {
    const name = item.name || item.title || '';
    const mix = item.mix_name && item.mix_name !== name ? ` (${item.mix_name})` : '';
    return `${name}${mix}`.trim() || item.url;
  }

  function toggleGenrePopover(force) {
    const pop = $('#genre-popover');
    const trigger = $('#genre-trigger');
    if (!pop || !trigger) return;
    const open = typeof force === 'boolean' ? force : pop.hidden;
    pop.hidden = !open;
    trigger.setAttribute('aria-expanded', open ? 'true' : 'false');
    if (open) {
      renderGenreList();
      setTimeout(() => $('#genre-search').focus(), 20);
    }
  }

  async function loadSubgenres(force = false) {
    if (S.catalog.loading) return;
    if (S.catalog.loaded && !force) return;
    S.catalog.loading = true;
    S.catalog.error = '';
    renderGenreList();
    renderSchedulerGenrePicker();
    try {
      const res = await host('getGenreCatalog');
      const list = (res && res.subgenres) || [];
      S.catalog.items = list
        .map((s) => ({
          id: Number(s.id) || 0,
          name: s.name || '',
          slug: s.slug || '',
          genreName: s.genreName || s.genre_name || '',
          genreId: Number(s.genreId) || 0,
        }))
        .filter((s) => s.id && s.name)
        .sort((a, b) => a.name.localeCompare(b.name, 'en'));
      S.catalog.loaded = true;
    } catch (err) {
      S.catalog.error = err.message;
      S.catalog.loaded = false;
    } finally {
      S.catalog.loading = false;
      renderGenreList();
      renderSchedulerGenrePicker();
    }
  }

  function updateGenreTabs() {
    $$('#genre-tabs .genre-tab').forEach((btn) => {
      const active = btn.dataset.genreTab === S.discover.genreTab;
      btn.classList.toggle('active', active);
      btn.setAttribute('aria-selected', active ? 'true' : 'false');
    });
    const search = $('#genre-search');
    if (search) search.placeholder = S.discover.genreTab === 'subgenre' ? '搜索子曲风…' : '搜索曲风…';
  }

  function updateGenreFooter() {
    const hint = $('#genre-hint');
    if (!hint) return;
    if (S.discover.genreTab === 'subgenre') {
      const n = S.discover.subgenres.length;
      hint.textContent = n ? `已选 ${n} 个子曲风` : '子曲风与曲风二选一';
    } else {
      const n = S.discover.genres.length;
      hint.textContent = n ? `已选 ${n} 个曲风` : '可多选';
    }
  }

  function renderGenreList() {
    const wrap = $('#genre-list');
    if (!wrap) return;
    updateGenreTabs();
    updateGenreFooter();
    const keyword = ($('#genre-search').value || '').trim().toLowerCase();
    if (S.discover.genreTab === 'subgenre') {
      if (!S.catalog.loaded && !S.catalog.loading && !S.catalog.error) {
        loadSubgenres();
      }
      if (S.catalog.loading) {
        wrap.innerHTML = '<div class="log-empty">正在读取子曲风…</div>';
        return;
      }
      if (S.catalog.error) {
        wrap.innerHTML = `<div class="log-empty">加载子曲风失败：${escHtml(S.catalog.error)}<br><button class="btn tiny ghost" type="button" data-subgenre-retry>重试</button></div>`;
        return;
      }
      const selected = new Set(S.discover.subgenres.map((s) => s.id));
      const items = S.catalog.items.filter((s) => !keyword
        || s.name.toLowerCase().includes(keyword)
        || s.genreName.toLowerCase().includes(keyword));
      if (!items.length) {
        wrap.innerHTML = '<div class="log-empty">没有匹配的子曲风</div>';
        return;
      }
      wrap.innerHTML = `<div class="subgenre-head"><span></span><span>子曲风</span><span>所属曲风</span></div>
        ${items.map((s) => {
          const on = selected.has(s.id);
          return `<button class="subgenre-row" type="button" role="option" data-subgenre-id="${s.id}" aria-selected="${on}">
            <span class="tick">${on ? icon('check') : ''}</span>
            <span class="subgenre-name">${escHtml(s.name)}</span>
            <span class="subgenre-parent" title="${escHtml(s.genreName)}">${escHtml(s.genreName)}</span>
          </button>`;
        }).join('')}`;
      return;
    }
    const selected = new Set(S.discover.genres);
    if (!S.genres.length) {
      wrap.innerHTML = `<div class="log-empty">${S.genresLoaded ? '没有匹配的曲风' : '正在加载曲风…'}</div>`;
      return;
    }
    const items = S.genres.filter((g) => !keyword || g.name.toLowerCase().includes(keyword));
    if (!items.length) {
      wrap.innerHTML = '<div class="log-empty">没有匹配的曲风</div>';
      return;
    }
    wrap.innerHTML = items.map((g) => `
      <button class="genre-option" type="button" role="option" data-genre="${escHtml(g.name)}" aria-selected="${selected.has(g.name)}">
        <span class="tick">${selected.has(g.name) ? icon('check') : ''}</span>
        <span>${escHtml(g.name)}</span>
      </button>`).join('');
  }

  $('#genre-tabs')?.addEventListener('click', (e) => {
    const btn = e.target.closest('[data-genre-tab]');
    if (!btn) return;
    S.discover.genreTab = btn.dataset.genreTab === 'subgenre' ? 'subgenre' : 'genre';
    renderGenreList();
    if (S.discover.genreTab === 'subgenre') loadSubgenres();
    $('#genre-search')?.focus();
  });

  $('#genre-list')?.addEventListener('click', (e) => {
    if (e.target.closest('[data-subgenre-retry]')) {
      loadSubgenres(true);
      return;
    }
    const subBtn = e.target.closest('[data-subgenre-id]');
    if (subBtn) {
      const id = Number(subBtn.dataset.subgenreId);
      const item = S.catalog.items.find((s) => s.id === id);
      if (!item) return;
      const at = S.discover.subgenres.findIndex((s) => s.id === id);
      if (at >= 0) S.discover.subgenres.splice(at, 1);
      else {
        if (S.discover.genres.length) S.discover.genres = [];
        S.discover.subgenres.push(item);
      }
      renderGenreList();
      updateGenreTrigger();
      return;
    }
    const btn = e.target.closest('[data-genre]');
    if (!btn) return;
    const name = btn.dataset.genre;
    const list = S.discover.genres;
    const at = list.indexOf(name);
    if (at >= 0) list.splice(at, 1);
    else {
      if (S.discover.subgenres.length) S.discover.subgenres = [];
      list.push(name);
    }
    renderGenreList();
    updateGenreTrigger();
  });

  function updateGenreTrigger() {
    const trigger = $('#genre-trigger');
    const label = $('#genre-label');
    if (!trigger || !label) return;
    const subs = S.discover.subgenres;
    const gens = S.discover.genres;
    if (subs.length) {
      label.textContent = subs.length === 1 ? subs[0].name : `${subs[0].name} 等 ${subs.length} 个子曲风`;
      trigger.title = subs.map((s) => `${s.name}（${s.genreName}）`).join('、');
    } else if (!gens.length) {
      label.textContent = '全部曲风';
      trigger.title = '全部曲风';
    } else {
      label.textContent = gens.length === 1 ? gens[0] : `${gens[0]} 等 ${gens.length} 个曲风`;
      trigger.title = gens.join('、');
    }
    updateGenreFooter();
  }

  function applyDatePreset(preset) {
    const extra = $('#date-extra');
    const now = new Date();
    if (preset === 'latest') {
      extra.hidden = true;
      return;
    }
    extra.hidden = false;
    if (preset === 'week') {
      $('#date-from').value = dateIso(new Date(now.getTime() - 7 * 86400000));
      $('#date-to').value = dateIso(now);
    } else if (preset === 'month') {
      const from = new Date(now); from.setMonth(from.getMonth() - 1);
      $('#date-from').value = dateIso(from);
      $('#date-to').value = dateIso(now);
    } else if (preset === 'halfyear') {
      const from = new Date(now); from.setMonth(from.getMonth() - 6);
      $('#date-from').value = dateIso(from);
      $('#date-to').value = dateIso(now);
    }
    syncCustomRange();
  }

  function syncCustomRange() {
    S.discover.from = $('#date-from').value || '';
    S.discover.to = $('#date-to').value || '';
  }

  async function loadDiscover(reset) {
    const list = S.discover;
    if (list.loading) return;
    if (reset) {
      list.page = 0;
      list.hasMore = false;
      list.items = [];
      list.seen = new Set();
      list.selected.clear();
      list.anchor = -1;
      list.error = '';
      updateSelectionDock();
    }
    list.loading = true;
    markDirty('discover');
    const page = list.page + 1;
    const params = {
      kind: list.kind,
      page,
      per_page: list.perPage,
    };
    if (list.subgenres.length) {
      params.subgenre_ids = list.subgenres.map((s) => s.id);
    } else if (list.genres.length) {
      params.genres = list.genres.slice();
    }
    if (list.preset !== 'latest') {
      params.date_from = list.from;
      params.date_to = list.to;
    }
    try {
      const res = await host('getLatestDiscover', { params }, 5 * 60 * 1000);
      const rows = (res && res.results) || [];
      const added = [];
      rows.forEach((raw) => {
        if (!raw || !raw.url) return;
        if (list.seen.has(raw.url)) return;
        list.seen.add(raw.url);
        added.push(normalizeItem(raw, list.kind));
      });
      list.items = list.items.concat(added);
      list.page = (res && res.page) || page;
      list.hasMore = !!(res && res.has_more);
      list.error = '';
      if (added.length && reset) list.anchor = -1;
    } catch (err) {
      list.error = err.message || '获取失败';
      toast(`获取最新失败：${list.error}`, 'error', 6000);
    } finally {
      list.loading = false;
      markDirty('discover', 'chrome');
      window.__vl?.discover?.setItems(list.items);
    }
  }

  function normalizeItem(raw, kind) {
    const artists = typeof raw.artists === 'string' ? raw.artists : joinArtists(raw.artists);
    return {
      url: raw.url || '',
      id: raw.id || 0,
      name: raw.name || raw.title || '',
      title: raw.title || raw.name || '',
      mix_name: raw.mix_name || '',
      artists: artists || '',
      genre: (raw.genre && raw.genre.name) || raw.genre || '',
      subgenre: (raw.subgenre && raw.subgenre.name) || raw.subgenre || '',
      type: raw.type || (kind === 'releases' ? 'release' : 'track'),
      cover: raw.cover || raw.image || '',
    };
  }

  function joinArtists(list) {
    if (!Array.isArray(list)) return '';
    const names = list.map((a) => {
      if (typeof a === 'string') return a;
      if (a && typeof a === 'object') return a.name || a.title || '';
      return '';
    }).filter(Boolean);
    // The Go backend returns a single-element array whose name may itself be
    // "Artist A, Artist B"; dedupe that shape so names are not repeated.
    return Array.from(new Set(names.join(', ').split(', ').filter(Boolean))).join(', ');
  }

  function renderDiscover() {
    const list = S.discover;
    const rows = $('#discover-rows');
    if (!rows) return;
    const count = $('#discover-count');
    if (count) {
      const sel = selectedCount('discover');
      count.textContent = `${list.items.length} 首${sel ? ` · 已选 ${sel}` : ''}`;
    }
    $$('#discover-page-size .seg').forEach((btn) => {
      btn.classList.toggle('active', Number(btn.dataset.size) === list.perPage);
    });
    const state = $('#discover-state');
    if (state) {
      if (list.loading && !list.items.length) {
        state.hidden = false;
        state.className = 'list-state';
        state.innerHTML = `${icon('loader', 'ic spinner')}<span>正在从 Beatport 获取${list.kind === 'releases' ? '专辑' : '曲目'}…</span>`;
      } else if (list.error && !list.items.length) {
        state.hidden = false;
        state.className = 'list-state error';
        state.innerHTML = `${icon('alert')}<span>${escHtml(list.error)}，点击「获取最新」重试</span>`;
      } else if (!list.items.length) {
        state.hidden = false;
        state.className = 'list-state';
        state.innerHTML = `<div class="empty-wrap">
          <span class="empty-icon">${icon('sparkle')}</span>
          <p>选择曲风与日期后点击「获取最新」开始发现</p>
        </div>`;
      } else {
        state.hidden = false;
        state.className = 'list-state';
        state.innerHTML = list.loading
          ? `${icon('loader', 'ic spinner')}<span>正在加载更多…</span>`
          : (list.hasMore
            ? `<span>已加载 ${list.items.length} 首，继续下滑加载更多</span>`
            : `<span>已加载全部 ${list.items.length} 首</span>`);
        if (!list.hasMore && !list.loading) state.innerHTML = `<span>已加载全部 ${list.items.length} 首</span>`;
      }
    }
    const checkAll = $('#discover-check-all');
    if (checkAll) {
      const sel = selectedCount('discover');
      checkAll.checked = list.items.length > 0 && sel === list.items.length;
      checkAll.indeterminate = sel > 0 && sel < list.items.length;
    }
    const card = state ? state.closest('.list-card') : null;
    if (card) card.classList.toggle('empty', list.items.length === 0);
    window.__vl?.discover?.refresh();
    updateSelectionDock();
  }

  /* ----------------------------------------------------------------- search */

  function setupSearch() {
    const rows = $('#search-rows');
    rows.addEventListener('click', (e) => handleRowClick(e, 'search'));
    $('#search-btn').addEventListener('click', doSearch);
    $('#search-input').addEventListener('keydown', (e) => {
      if (e.key === 'Enter') doSearch();
    });
    $('#search-select-all').addEventListener('click', () => setAllSelected('search', true));
    $('#search-clear').addEventListener('click', () => setAllSelected('search', false));
    $('#search-check-all').addEventListener('change', (e) => setAllSelected('search', e.target.checked));
    $('#search-state').addEventListener('click', (e) => {
      const chip = e.target.closest('[data-q]');
      if (!chip) return;
      $('#search-input').value = chip.dataset.q;
      doSearch();
    });

    window.__vl = window.__vl || {};
    window.__vl.search = createVirtualList({
      scroll: $('#search-scroll'),
      spacer: $('#search-spacer'),
      rows,
      rowHeight: 58,
      renderRow: (node, item, index) => renderResultRow(node, item, index, 'search'),
    });
  }

  async function doSearch() {
    const input = $('#search-input');
    const q = (input.value || '').trim();
    if (!q) { input.focus(); return; }
    if (S.search.loading) return;
    S.search.query = q;
    S.search.loading = true;
    S.search.error = '';
    S.search.items = [];
    S.search.selected.clear();
    S.search.anchor = -1;
    markDirty('search');
    window.__vl?.search?.setItems([]);
    updateSelectionDock();
    try {
      // A pasted Beatport URL is enqueued directly, matching the CLI behavior.
      if (/^https?:\/\//i.test(q)) {
        await enqueueUrls([q], { [q]: q });
        S.search.loading = false;
        markDirty('search');
        return;
      }
      const res = await rpc('search', { query: q });
      const arr = Array.isArray(res) ? res : [];
      S.search.items = arr.map((raw) => ({
        url: raw.url,
        id: raw.id,
        name: raw.title || raw.name || '',
        title: raw.title || raw.name || '',
        artists: raw.artists || '',
        genre: raw.genre || '',
        type: raw.type || 'track',
      })).filter((it) => it.url);
    } catch (err) {
      S.search.error = err.message || '搜索失败';
      toast(`搜索失败：${S.search.error}`, 'error', 6000);
    } finally {
      S.search.loading = false;
      markDirty('search');
      window.__vl?.search?.setItems(S.search.items);
    }
  }

  function renderSearch() {
    const list = S.search;
    const count = $('#search-count');
    if (count) {
      const sel = selectedCount('search');
      count.textContent = `${list.items.length} 条${sel ? ` · 已选 ${sel}` : ''}`;
    }
    const state = $('#search-state');
    if (state) {
      if (list.loading) {
        state.hidden = false;
        state.className = 'list-state';
        state.innerHTML = `${icon('loader', 'ic spinner')}<span>搜索中…</span>`;
      } else if (list.error) {
        state.hidden = false;
        state.className = 'list-state error';
        state.innerHTML = `${icon('alert')}<span>${escHtml(list.error)}</span>`;
      } else if (!list.items.length) {
        state.hidden = false;
        state.className = 'list-state';
        state.innerHTML = `<div class="empty-wrap">
          <span class="empty-icon">${icon('search')}</span>
          <p>输入关键词开始搜索，或粘贴 Beatport 曲目链接直接加入下载队列</p>
          <div class="suggest-row">
            <button class="chip" data-q="Afro House" type="button">Afro House</button>
            <button class="chip" data-q="Anyma" type="button">Anyma</button>
            <button class="chip" data-q="Drum &amp; Bass" type="button">Drum &amp; Bass</button>
            <button class="chip" data-q="Charlotte de Witte" type="button">Charlotte de Witte</button>
          </div>
        </div>`;
      } else {
        state.hidden = true;
      }
    }
    const checkAll = $('#search-check-all');
    if (checkAll) {
      const sel = selectedCount('search');
      checkAll.checked = list.items.length > 0 && sel === list.items.length;
      checkAll.indeterminate = sel > 0 && sel < list.items.length;
    }
    const card = state ? state.closest('.list-card') : null;
    if (card) card.classList.toggle('empty', list.items.length === 0);
    window.__vl?.search?.refresh();
    updateSelectionDock();
  }

  /* -------------------------------------------------------------- downloads */

  function activeDownloadCount() {
    let n = 0;
    S.downloads.map.forEach((r) => { if (r.status === 'downloading' || r.status === 'queued') n++; });
    return n;
  }

  function totalDownloadSpeed() {
    let speed = 0;
    S.downloads.map.forEach((r) => {
      if (r.status === 'downloading') speed += Number(r.speed) || 0;
    });
    return speed;
  }

  function openTaskCount() {
    let n = 0;
    S.downloads.map.forEach((r) => { if (isNonTerminal(r)) n++; });
    return n;
  }

  function nextDownloadSeq() {
    S.downloads.nextSeq += 1;
    return S.downloads.nextSeq;
  }

  function setDownloadRow(url, patch) {
    const prev = S.downloads.map.get(url) || { url };
    const row = Object.assign({}, prev, patch, { url });
    if (!row.seq) row.seq = nextDownloadSeq();
    S.downloads.map.set(url, row);
    persistDownloads();
  }

  function upsertEventRow(ev) {
    const url = ev.url || '';
    if (!url) return null;
    let row = S.downloads.map.get(url);
    if (!row) {
      row = {
        url,
        title: ev.title || url,
        status: 'queued',
        percent: 0,
        received: 0,
        total: 0,
        seq: nextDownloadSeq(),
        addedAt: Date.now(),
      };
      S.downloads.map.set(url, row);
    } else if (ev.title) {
      row.title = ev.title;
    }
    return row;
  }

  function handleEvents(events) {
    if (!events.length) return;
    let touchDownloads = false;
    let touchLog = false;
    events.forEach((ev) => {
      const type = ev && ev.event;
      if (!type) return;
      if (type.indexOf('download.') === 0) {
        touchDownloads = handleDownloadEvent(ev) || touchDownloads;
      } else if (type === 'log') {
        pushLog(ev.message || '', '');
        touchLog = true;
      } else if (type === 'scheduler.found') {
        pushLog(`定时任务发现 ${ev.count || 0} 个新项目，已加入下载队列`, 'accent');
        touchLog = true;
      } else if (type === 'auth.loggedOut') {
        S.authReady = false;
        markDirty('chrome', 'settings');
        toast('账号已退出登录', 'warn');
        if (modalLayer().hidden) showLoginModal(true);
      } else {
        pushLog(`[${type}] ${JSON.stringify(ev)}`, '');
        touchLog = true;
      }
    });
    if (touchDownloads) {
      markDirty('downloads', 'chrome');
      if (S.view === 'downloads') window.__vl?.downloads?.refresh();
    }
    if (touchLog) markDirty('scheduler');
  }

  function handleDownloadEvent(ev) {
    const type = ev.event;
    const url = ev.url || '';
    if (!url) return false;
    const existing = S.downloads.map.get(url);
    // Ignore late events from a backend that was stopped or a row the user
    // removed on purpose.
    if (S.downloads.muted.has(url)) return false;
    if (existing && existing.status === 'stopped') return false;
    const now = Date.now();
    if (type === 'download.started') {
      const row = upsertEventRow(ev);
      Object.assign(row, {
        status: 'downloading',
        percent: 0,
        received: 0,
        total: 0,
        startedAt: now,
        speed: 0,
        lastSample: null,
        error: '',
        reason: '',
      });
      persistDownloads();
      return true;
    }
    if (type === 'download.progress') {
      const row = upsertEventRow(ev);
      const received = Number(ev.received) || 0;
      const total = Number(ev.total) || 0;
      row.status = 'downloading';
      row.received = received;
      row.total = total;
      row.percent = Number(ev.percent) || (total > 0 ? Math.floor((received / total) * 100) : 0);
      if (row.lastSample && received >= row.lastSample.received) {
        const dt = (now - row.lastSample.at) / 1000;
        if (dt > 0.2) {
          const inst = (received - row.lastSample.received) / dt;
          row.speed = row.speed ? row.speed * 0.65 + inst * 0.35 : inst;
          row.lastSample = { received, at: now };
        }
      } else {
        row.lastSample = { received, at: now };
      }
      row.updatedAt = now;
      persistDownloads();
      return true;
    }
    if (type === 'download.done') {
      const row = upsertEventRow(ev);
      Object.assign(row, {
        status: 'done',
        percent: 100,
        path: ev.path || '',
        speed: 0,
        updatedAt: now,
        error: '',
      });
      persistDownloads();
      pumpDownloads();
      return true;
    }
    if (type === 'download.error') {
      const row = upsertEventRow(ev);
      Object.assign(row, {
        status: 'error',
        error: ev.error || '下载失败',
        percent: 0,
        speed: 0,
        updatedAt: now,
      });
      persistDownloads();
      pumpDownloads();
      return true;
    }
    if (type === 'download.skipped') {
      const row = upsertEventRow(ev);
      const reason = ev.reason || 'exists';
      Object.assign(row, {
        status: 'skipped',
        reason,
        percent: 0,
        speed: 0,
        updatedAt: now,
        detail: reason === 'duplicate' ? '已在下载队列中' : '本地文件已存在',
      });
      persistDownloads();
      pumpDownloads();
      return true;
    }
    return false;
  }

  const DOWNLOADS_STORE = 'bpdl.downloads.v2';
  let persistTimer = 0;

  function persistDownloads() {
    // Progress events arrive in bursts; coalesce disk writes to keep the UI
    // thread free while a full queue is downloading.
    if (persistTimer) return;
    persistTimer = setTimeout(() => {
      persistTimer = 0;
      const finished = [];
      S.downloads.map.forEach((r) => {
        if (r.status === 'done' || r.status === 'error' || r.status === 'skipped' || r.status === 'stopped') {
          finished.push({
            url: r.url, title: r.title, status: r.status, path: r.path,
            error: r.error, reason: r.reason, detail: r.detail, percent: 100,
            updatedAt: r.updatedAt || Date.now(), total: r.total || 0, seq: r.seq || 0,
          });
        }
      });
      store.set(DOWNLOADS_STORE, finished.slice(-400));
    }, 900);
  }

  function restoreDownloads() {
    const rows = store.get(DOWNLOADS_STORE, []);
    if (!Array.isArray(rows)) return;
    let maxSeq = 0;
    rows.forEach((r) => { if (r && Number(r.seq) > maxSeq) maxSeq = Number(r.seq); });
    S.downloads.nextSeq = Math.max(S.downloads.nextSeq, maxSeq);
    rows.forEach((r) => {
      if (!r || !r.url) return;
      const row = Object.assign({}, r, { restored: true, speed: 0, lastSample: null });
      if (!row.seq) row.seq = nextDownloadSeq();
      S.downloads.map.set(r.url, row);
    });
    S.downloads.restored = rows.length;
  }

  // Stable queue order: active tasks first, then finished ones, each in the
  // order they were added. Progress/updatedAt must never influence ordering,
  // otherwise rows jump to the top on every SSE progress event.
  function filteredDownloads() {
    const all = Array.from(S.downloads.map.values());
    const isActive = (r) => isNonTerminal(r);
    all.sort((a, b) => {
      const ga = isActive(a) ? 0 : 1;
      const gb = isActive(b) ? 0 : 1;
      if (ga !== gb) return ga - gb;
      return (a.seq || 0) - (b.seq || 0);
    });
    switch (S.downloads.filter) {
      case 'active': return all.filter((r) => isActive(r));
      case 'done': return all.filter((r) => r.status === 'done');
      case 'error': return all.filter((r) => r.status === 'error' || r.status === 'skipped' || r.status === 'stopped');
      default: return all;
    }
  }

  function statusMeta(row) {
    switch (row.status) {
      case 'waiting': return S.downloads.pausedAll
        ? { cls: 'paused', text: '已暂停' }
        : { cls: 'queued', text: '排队中' };
      case 'paused': return { cls: 'paused', text: '已暂停' };
      case 'queued': return { cls: 'queued', text: '正在开始' };
      case 'downloading': return { cls: 'downloading', text: '下载中' };
      case 'done': return { cls: 'done', text: '已完成' };
      case 'error': return { cls: 'error', text: '失败' };
      case 'stopped': return { cls: 'stopped', text: '已停止' };
      case 'skipped': return { cls: 'skipped', text: '已跳过' };
      default: return { cls: 'queued', text: '等待中' };
    }
  }

  function renderDownloadRow(node, row, index) {
    const meta = statusMeta(row);
    const selected = S.downloads.selected.has(row.url);
    const percent = clamp(Number(row.percent) || 0, 0, 100);
    const title = row.title || row.url;
    const barState = row.status === 'done'
      ? { cls: 'progress-bar done', width: '100%' }
      : row.status === 'error' || row.status === 'stopped'
        ? { cls: 'progress-bar error', width: `${Math.max(percent, 6)}%` }
        : row.status === 'skipped'
          ? { cls: 'progress-bar error', width: '0%' }
          : row.status === 'downloading' && !row.total
            ? { cls: 'progress-bar indeterminate', width: '' }
            : { cls: 'progress-bar', width: `${Math.max(percent, row.status === 'downloading' ? 3 : 0)}%` };
    const percentLabel = `${percent}%`;
    const speedText = row.status === 'downloading' ? fmtSpeed(row.speed) : '';

    // Progress events arrive every ~70 ms. Once a row is built, only its
    // dynamic fields are patched: recreating the progress bar would restart the
    // CSS transition and make the row visibly stutter.
    const canPatch = node.dataset.pbuilt === '1' && node.dataset.purl === row.url
      && node.dataset.pstatus === row.status && node.dataset.ptitle === title
      && node.dataset.psel === (selected ? '1' : '0');
    if (canPatch) {
      const bar = node.querySelector('.progress-bar');
      if (bar) { bar.className = barState.cls; bar.style.width = barState.width; }
      const label = node.querySelector('.dl-percent');
      if (label) label.textContent = percentLabel;
      const speed = node.querySelector('.dl-speed');
      if (speed) speed.textContent = speedText;
      const chip = node.querySelector('.chip-status');
      if (chip) {
        chip.className = `chip-status ${meta.cls}`;
        chip.innerHTML = `<span class="dot"></span>${escHtml(meta.text)}`;
      }
      return;
    }

    const progressHtml = `<div class="progress-track"><div class="${barState.cls}"${barState.width ? ` style="width:${barState.width}"` : ''}></div></div>`;
    node.innerHTML = `
      <span class="cell-cb">${checkboxHtml(selected)}</span>
      <span class="dl-title">
        <strong title="${escHtml(title)}">${escHtml(title)}</strong>
      </span>
      <span class="dl-progress">
        ${progressHtml}
        <span class="progress-label"><span class="dl-percent">${escHtml(percentLabel)}</span><span class="dl-speed">${escHtml(speedText)}</span></span>
      </span>
      <span class="dl-status">
        <span class="chip-status ${meta.cls}"><span class="dot"></span>${escHtml(meta.text)}</span>
        <span class="cell-actions">
          ${row.status === 'waiting' ? `<button class="icon-btn" data-act="pause" title="暂停">${icon('pause')}</button>` : ''}
          ${row.status === 'paused' ? `<button class="icon-btn" data-act="resume" title="继续">${icon('play')}</button>` : ''}
          ${row.status === 'done' && row.path ? `<button class="icon-btn" data-act="reveal" title="在资源管理器中显示">${icon('folder')}</button>` : ''}
          ${row.status === 'error' || row.status === 'skipped' || row.status === 'stopped' ? `<button class="icon-btn" data-act="retry" title="重试">${icon('refresh')}</button>` : ''}
          <button class="icon-btn" data-act="open" title="在浏览器打开">${icon('external')}</button>
        </span>
      </span>`;
    node.dataset.url = row.url;
    node.dataset.pbuilt = '1';
    node.dataset.purl = row.url;
    node.dataset.pstatus = row.status;
    node.dataset.ptitle = title;
    node.dataset.psel = selected ? '1' : '0';
    node.className = `dl-row${selected ? ' selected' : ''}`;
  }

  function renderDownloads() {
    const rows = filteredDownloads();
    const all = Array.from(S.downloads.map.values());
    const active = all.filter((r) => r.status === 'queued' || r.status === 'downloading').length;
    $('#stat-active').textContent = String(active);
    $('#stat-done').textContent = String(all.filter((r) => r.status === 'done').length);
    $('#stat-skipped').textContent = String(all.filter((r) => r.status === 'skipped').length);
    $('#stat-error').textContent = String(all.filter((r) => r.status === 'error' || r.status === 'stopped').length);
    const state = $('#download-state');
    if (state) {
      if (!rows.length) {
        state.hidden = false;
        state.innerHTML = `<div class="empty-wrap">
          <span class="empty-icon">${icon('download')}</span>
          <p>${S.downloads.map.size ? '当前筛选没有任务' : '还没有下载任务，去「发现最新」挑选几首吧'}</p>
          ${S.downloads.map.size ? '' : '<button class="btn small primary" type="button" data-goto="discover">去发现最新</button>'}
        </div>`;
      } else {
        state.hidden = true;
      }
      const card = state.closest('.list-card');
      if (card) card.classList.toggle('empty', rows.length === 0);
    }
    const pauseBtn = $('#dl-pause-all');
    if (pauseBtn) {
      pauseBtn.innerHTML = S.downloads.pausedAll
        ? `${icon('play')}<span>继续队列</span>`
        : `${icon('pause')}<span>暂停队列</span>`;
      pauseBtn.classList.toggle('primary', S.downloads.pausedAll);
      pauseBtn.classList.toggle('ghost', !S.downloads.pausedAll);
    }
    const stopBtn = $('#dl-stop-all');
    if (stopBtn) stopBtn.disabled = openTaskCount() === 0;
    window.__vl?.downloads?.setItems(rows);
    updateDownloadSelectionUI();
    markDirty('chrome');
  }

  function toggleDownloadSelection(index, shift) {
    const rows = filteredDownloads();
    const row = rows[index];
    if (!row) return;
    const list = S.downloads;
    if (shift && list.anchor >= 0 && list.anchor !== index) {
      const target = !list.selected.has(row.url);
      const lo = Math.min(list.anchor, index);
      const hi = Math.max(list.anchor, index);
      for (let i = lo; i <= hi; i++) {
        const it = rows[i];
        if (!it) continue;
        if (target) list.selected.add(it.url); else list.selected.delete(it.url);
      }
    } else {
      if (list.selected.has(row.url)) list.selected.delete(row.url);
      else list.selected.add(row.url);
      list.anchor = index;
    }
    markDirty('downloads');
    window.__vl?.downloads?.refresh();
  }

  function clearDownloadSelection() {
    S.downloads.selected.clear();
    S.downloads.anchor = -1;
    markDirty('downloads');
    window.__vl?.downloads?.refresh();
  }

  function updateDownloadSelectionUI() {
    const rows = filteredDownloads();
    const selectedVisible = rows.filter((r) => S.downloads.selected.has(r.url));
    const bulk = $('#dl-bulk');
    if (bulk) {
      bulk.hidden = selectedVisible.length === 0;
      const count = $('#dl-bulk-count');
      if (count) count.textContent = String(selectedVisible.length);
    }
    const checkAll = $('#dl-check-all');
    if (checkAll) {
      checkAll.checked = rows.length > 0 && selectedVisible.length === rows.length;
      checkAll.indeterminate = selectedVisible.length > 0 && selectedVisible.length < rows.length;
    }
  }

  function pauseSelectedDownloads() {
    const selected = filteredDownloads().filter((r) => S.downloads.selected.has(r.url));
    let paused = 0, active = 0, done = 0;
    selected.forEach((r) => {
      if (r.status === 'waiting') { r.status = 'paused'; paused++; }
      else if (r.status === 'queued' || r.status === 'downloading') active++;
      else if (!isNonTerminal(r)) done++;
    });
    markDirty('downloads', 'chrome');
    window.__vl?.downloads?.refresh();
    if (paused) toast(`已暂停 ${paused} 首排队任务`, 'success');
    if (active) toast(`其中有 ${active} 首正在下载，无法单独暂停；可点「停止全部」`, 'warn', 6000);
    if (!paused && !active && done) toast('所选任务已结束，无需暂停', 'info');
  }

  function resumeSelectedDownloads() {
    const selected = filteredDownloads().filter((r) => S.downloads.selected.has(r.url));
    let resumed = 0;
    selected.forEach((r) => {
      if (r.status === 'paused') { r.status = 'waiting'; resumed++; }
    });
    if (S.downloads.pausedAll) {
      toast('队列处于暂停状态，请先点「继续队列」', 'warn');
    }
    markDirty('downloads', 'chrome');
    window.__vl?.downloads?.refresh();
    if (resumed) {
      toast(`已恢复 ${resumed} 首排队任务`, 'success');
      pumpDownloads();
    }
  }

  async function deleteSelectedDownloads() {
    const selected = filteredDownloads().filter((r) => S.downloads.selected.has(r.url));
    if (!selected.length) return;
    const blocked = selected.filter((r) => r.status === 'queued' || r.status === 'downloading');
    if (blocked.length) {
      const ok = await confirmDialog({
        title: '删除所选任务',
        message: `所选 ${selected.length} 首中有 ${blocked.length} 首正在下载或已提交。\n`
          + '后端没有单任务取消接口，需要先「停止全部」并重启后端才能移除它们；\n'
          + '已经写了一半的文件可能留在下载目录里。\n\n确定停止全部并删除所选吗？',
        confirmText: '停止并删除',
        danger: true,
      });
      if (!ok) return;
      await stopAllDownloads({ deleteSelected: true, skipConfirm: true });
      return;
    }
    selected.forEach((r) => S.downloads.map.delete(r.url));
    selected.forEach((r) => S.downloads.muted.add(r.url));
    clearDownloadSelection();
    persistDownloads();
    markDirty('downloads', 'chrome');
    window.__vl?.downloads?.refresh();
    toast(`已从队列移除 ${selected.length} 首`, 'info');
  }

  function togglePauseAllDownloads() {
    S.downloads.pausedAll = !S.downloads.pausedAll;
    if (!S.downloads.pausedAll) {
      // Resuming the queue also resumes individually paused rows.
      S.downloads.map.forEach((r) => { if (r.status === 'paused') r.status = 'waiting'; });
      pumpDownloads();
      toast('队列已继续', 'success');
    } else {
      toast('队列已暂停：正在下载的任务会继续完成', 'info');
    }
    markDirty('downloads', 'chrome');
    window.__vl?.downloads?.refresh();
  }

  async function stopAllDownloads(options) {
    const opts = options || {};
    if (openTaskCount() === 0) { toast('当前没有进行中的任务', 'info'); return; }
    if (!opts.skipConfirm) {
      const ok = await confirmDialog({
        title: opts.deleteSelected ? '停止并删除所选' : '停止全部下载',
        message: '这会立即终止后端进程并重启它：所有排队与下载中的任务都会被停止。\n'
          + '已经写了一半的文件可能留在下载目录里，需要手动删除或之后重新下载。\n\n确定停止吗？',
        confirmText: opts.deleteSelected ? '停止并删除' : '停止全部',
        danger: true,
      });
      if (!ok) return;
    }
    const btn = $('#dl-stop-all');
    if (btn) btn.disabled = true;
    S.downloads.stoppingAll = true;
    try {
      await host('stopDownloads');
      S.downloads.map.forEach((r) => {
        if (isNonTerminal(r)) {
          S.downloads.muted.add(r.url);
          r.status = 'stopped';
          r.error = '已手动停止';
          r.speed = 0;
          r.percent = 0;
          r.updatedAt = Date.now();
        }
      });
      S.downloads.pausedAll = false;
      S.downloads.expansions = [];
      if (opts.deleteSelected) {
        S.downloads.selected.forEach((url) => {
          S.downloads.muted.add(url);
          S.downloads.map.delete(url);
        });
      }
      clearDownloadSelection();
      persistDownloads();
      markDirty('downloads', 'chrome');
      window.__vl?.downloads?.refresh();
      toast(opts.deleteSelected ? '已停止全部并删除所选任务，后端已重启' : '已停止全部任务，后端已重启', 'success');
    } catch (err) {
      toast(`停止失败：${err.message}`, 'error', 8000);
      interruptActiveDownloads('停止失败，后端状态未知');
    } finally {
      S.downloads.stoppingAll = false;
      if (btn) btn.disabled = openTaskCount() === 0;
    }
  }

  function setupDownloads() {
    const rows = $('#download-rows');
    rows.addEventListener('click', (e) => {
      const node = e.target.closest('.dl-row');
      if (!node) return;
      const btn = e.target.closest('[data-act]');
      const row = S.downloads.map.get(node.dataset.url);
      if (!row) return;
      if (btn) {
        e.stopPropagation();
        if (btn.dataset.act === 'reveal') {
          host('revealPath', { path: row.path, size: Number(row.total) || 0 })
            .then((res) => {
              if (res && res.path && res.path !== row.path) {
                row.path = res.path;
                persistDownloads();
                markDirty('downloads');
                window.__vl?.downloads?.refresh();
              }
            })
            .catch((err) => toast(err.message, 'error'));
        } else if (btn.dataset.act === 'retry') {
          S.downloads.map.delete(row.url);
          enqueueUrls([row.url], { [row.url]: row.title });
        } else if (btn.dataset.act === 'pause') {
          if (row.status === 'waiting') {
            row.status = 'paused';
            markDirty('downloads', 'chrome');
            window.__vl?.downloads?.refresh();
          }
        } else if (btn.dataset.act === 'resume') {
          if (row.status === 'paused') {
            row.status = 'waiting';
            markDirty('downloads', 'chrome');
            window.__vl?.downloads?.refresh();
            pumpDownloads();
          }
        } else if (btn.dataset.act === 'open') {
          host('openExternal', { url: row.url }).catch((err) => toast(err.message, 'error'));
        }
        return;
      }
      toggleDownloadSelection(Number(node.dataset.index), e.shiftKey);
    });

    $('#dl-filter').addEventListener('click', (e) => {
      const btn = e.target.closest('.seg');
      if (!btn) return;
      S.downloads.filter = btn.dataset.filter;
      $$('.seg', $('#dl-filter')).forEach((b) => b.classList.toggle('active', b === btn));
      S.downloads.selected.clear();
      S.downloads.anchor = -1;
      renderDownloads();
    });

    $('#dl-pause-all').addEventListener('click', togglePauseAllDownloads);
    $('#dl-stop-all').addEventListener('click', () => stopAllDownloads());
    $('#dl-check-all').addEventListener('change', (e) => {
      const rowsNow = filteredDownloads();
      if (e.target.checked) rowsNow.forEach((r) => S.downloads.selected.add(r.url));
      else S.downloads.selected.clear();
      markDirty('downloads');
      window.__vl?.downloads?.refresh();
    });
    $('#dl-pause-selected').addEventListener('click', pauseSelectedDownloads);
    $('#dl-resume-selected').addEventListener('click', resumeSelectedDownloads);
    $('#dl-delete-selected').addEventListener('click', deleteSelectedDownloads);
    $('#dl-clear-selection').addEventListener('click', clearDownloadSelection);

    $('#dl-open-dir').addEventListener('click', () => {
      const dir = S.settings.dir;
      if (!dir) { toast('下载目录尚未设置', 'warn'); return; }
      host('openPath', { path: dir }).catch((err) => toast(err.message, 'error'));
    });

    $('#download-state').addEventListener('click', (e) => {
      if (e.target.closest('[data-goto="discover"]')) {
        setView('discover');
        pulseView('discover');
      }
    });

    $('#dl-clear-done').addEventListener('click', async () => {
      const terminal = (r) => r.status === 'done' || r.status === 'error' || r.status === 'skipped' || r.status === 'stopped';
      const n = Array.from(S.downloads.map.values()).filter(terminal).length;
      if (!n) { toast('没有可清除的已结束任务', 'info'); return; }
      const ok = await confirmDialog({
        title: '清除已结束任务',
        message: `将从列表中移除 ${n} 条已完成 / 失败 / 跳过 / 停止的记录（不会删除本地音频文件）。`,
        confirmText: '清除',
        danger: true,
      });
      if (!ok) return;
      S.downloads.map.forEach((r, url) => {
        if (terminal(r)) { S.downloads.muted.add(url); S.downloads.map.delete(url); }
      });
      S.downloads.selected.clear();
      persistDownloads();
      renderDownloads();
      toast('已清除已结束记录', 'success');
    });

    window.__vl = window.__vl || {};
    window.__vl.downloads = createVirtualList({
      scroll: $('#download-scroll'),
      spacer: $('#download-spacer'),
      rows,
      rowHeight: 42,
      renderRow: renderDownloadRow,
    });
  }

  /* -------------------------------------------------------------- scheduler */

  function pushLog(message, kind) {
    if (!message) return;
    S.scheduler.logs.push({ t: fmtClock(), m: message, kind: kind || '' });
    if (S.scheduler.logs.length > 500) S.scheduler.logs.splice(0, S.scheduler.logs.length - 500);
    if (S.view === 'scheduler') markDirty('scheduler');
  }

  function renderScheduler() {
    const st = S.scheduler;
    const chip = $('#sched-status');
    if (chip) {
      chip.classList.toggle('running', st.running);
      chip.innerHTML = `<span class="pulse"></span><span>${st.running ? '运行中' : '已停止'}</span>`;
    }
    const intervalLabel = { 86400: '一天', 604800: '一周', 2592000: '一个月', 7776000: '三个月' }[st.seconds] || '设定间隔';
    const noteTitle = $('#sched-note-title');
    if (noteTitle) noteTitle.textContent = st.running ? `运行中 · 每${intervalLabel}扫描一次` : '等待启动';
    const noteText = $('#sched-note-text');
    if (noteText) {
      const parts = [];
      if (st.genres.length) parts.push(`${st.genres.length} 个曲风`);
      if (st.subgenres.length) parts.push(`${st.subgenres.length} 个子曲风`);
      const target = parts.length ? parts.join(' + ') : '全站';
      noteText.textContent = st.running
        ? `正在持续追踪${target}最新发布，发现的新曲目会自动加入下载队列。`
        : '启动后会按设定间隔扫描 Beatport 最新发布，并自动跳过已存在的文件。';
    }
    $$('#sched-intervals .option').forEach((btn) => {
      btn.classList.toggle('active', Number(btn.dataset.seconds) === st.seconds);
    });
    renderSchedulerGenrePicker();
    const log = $('#sched-log');
    if (log) {
      if (!st.logs.length) {
        log.innerHTML = '<div class="log-empty">还没有日志。启动定时任务后，扫描结果会显示在这里。</div>';
      } else {
        const stick = log.scrollTop + log.clientHeight >= log.scrollHeight - 40;
        log.innerHTML = st.logs.map((line) => `
          <div class="log-line ${line.kind}"><span class="t">${escHtml(line.t)}</span><span class="m">${escHtml(line.m)}</span></div>`).join('');
        if (stick) log.scrollTop = log.scrollHeight;
      }
    }
  }

  function toggleSchedulerGenrePopover(force) {
    const pop = $('#sched-genre-popover');
    const trigger = $('#sched-genre-trigger');
    if (!pop || !trigger) return;
    const open = typeof force === 'boolean' ? force : pop.hidden;
    pop.hidden = !open;
    trigger.setAttribute('aria-expanded', open ? 'true' : 'false');
    if (open) {
      renderSchedulerGenrePicker();
      if (S.scheduler.genreTab === 'subgenre') loadSubgenres();
    }
  }

  function updateSchedulerGenreTrigger() {
    const st = S.scheduler;
    const label = $('#sched-genre-label');
    if (label) {
      const g = st.genres.length;
      const sub = st.subgenres.length;
      if (!g && !sub) label.textContent = '全部曲风';
      else if (g && !sub) label.textContent = g === 1 ? st.genres[0] : `${st.genres[0]} 等 ${g} 个曲风`;
      else if (!g && sub) label.textContent = sub === 1 ? st.subgenres[0].name : `${st.subgenres[0].name} 等 ${sub} 个子曲风`;
      else label.textContent = `${g} 个曲风 + ${sub} 个子曲风`;
    }
    const trigger = $('#sched-genre-trigger');
    if (trigger) {
      const titles = st.genres.concat(st.subgenres.map((s) => `${s.name}（${s.genreName}）`));
      trigger.title = titles.length ? titles.join('、') : '全部曲风';
    }
    $$('#sched-genre-tabs .genre-tab').forEach((btn) => {
      const active = btn.dataset.schedGenreTab === st.genreTab;
      btn.classList.toggle('active', active);
      btn.setAttribute('aria-selected', active ? 'true' : 'false');
    });
    const search = $('#sched-genre-search');
    if (search) search.placeholder = st.genreTab === 'subgenre' ? '搜索子曲风…' : '搜索曲风…';
    const hint = $('#sched-genre-hint');
    if (hint) {
      const n = st.genreTab === 'subgenre' ? st.subgenres.length : st.genres.length;
      hint.textContent = n ? `已选 ${n} 个${st.genreTab === 'subgenre' ? '子曲风' : '曲风'}` : '可多选';
    }
  }

  function renderSchedulerGenrePicker() {
    updateSchedulerGenreTrigger();
    const pop = $('#sched-genre-popover');
    if (!pop || pop.hidden) return;
    const wrap = $('#sched-genre-list');
    if (!wrap) return;
    const st = S.scheduler;
    const keyword = ($('#sched-genre-search').value || '').trim().toLowerCase();
    if (st.genreTab === 'subgenre') {
      if (!S.catalog.loaded && !S.catalog.loading && !S.catalog.error) loadSubgenres();
      if (S.catalog.loading) {
        wrap.innerHTML = '<div class="log-empty">正在读取子曲风…</div>';
        return;
      }
      if (S.catalog.error) {
        wrap.innerHTML = `<div class="log-empty">加载子曲风失败：${escHtml(S.catalog.error)}<br><button class="btn tiny ghost" type="button" data-sched-subgenre-retry>重试</button></div>`;
        return;
      }
      const selected = new Set(st.subgenres.map((s) => s.id));
      const items = S.catalog.items.filter((s) => !keyword
        || s.name.toLowerCase().includes(keyword)
        || s.genreName.toLowerCase().includes(keyword));
      if (!items.length) {
        wrap.innerHTML = '<div class="log-empty">没有匹配的子曲风</div>';
        return;
      }
      wrap.innerHTML = `<div class="subgenre-head"><span></span><span>子曲风</span><span>所属曲风</span></div>
        ${items.map((s) => {
          const on = selected.has(s.id);
          return `<button class="subgenre-row" type="button" role="option" data-sched-subgenre-id="${s.id}" aria-selected="${on}">
            <span class="tick">${on ? icon('check') : ''}</span>
            <span class="subgenre-name">${escHtml(s.name)}</span>
            <span class="subgenre-parent" title="${escHtml(s.genreName)}">${escHtml(s.genreName)}</span>
          </button>`;
        }).join('')}`;
      return;
    }
    const selected = new Set(st.genres);
    if (!S.genres.length) {
      wrap.innerHTML = `<div class="log-empty">${S.genresLoaded ? '没有匹配的曲风' : '正在加载曲风…'}</div>`;
      return;
    }
    const items = S.genres.filter((g) => !keyword || g.name.toLowerCase().includes(keyword));
    if (!items.length) {
      wrap.innerHTML = '<div class="log-empty">没有匹配的曲风</div>';
      return;
    }
    wrap.innerHTML = items.map((g) => `
      <button class="genre-option" type="button" role="option" data-sched-genre="${escHtml(g.name)}" aria-selected="${selected.has(g.name)}">
        <span class="tick">${selected.has(g.name) ? icon('check') : ''}</span>
        <span>${escHtml(g.name)}</span>
      </button>`).join('');
  }

  async function refreshScheduler() {
    try {
      const res = await host('schedulerStatus');
      S.scheduler.running = !!(res && res.running);
      markDirty('scheduler', 'chrome');
    } catch (err) {
      uiLog('warn', `schedulerStatus 失败：${err.message}`);
    }
  }

  function setupScheduler() {
    $('#sched-intervals').addEventListener('click', (e) => {
      const btn = e.target.closest('.option');
      if (!btn) return;
      S.scheduler.seconds = Number(btn.dataset.seconds);
      renderScheduler();
    });
    $('#sched-genre-trigger').addEventListener('click', (e) => {
      e.stopPropagation();
      toggleSchedulerGenrePopover();
    });
    $('#sched-genre-tabs').addEventListener('click', (e) => {
      const btn = e.target.closest('[data-sched-genre-tab]');
      if (!btn) return;
      S.scheduler.genreTab = btn.dataset.schedGenreTab === 'subgenre' ? 'subgenre' : 'genre';
      renderSchedulerGenrePicker();
      if (S.scheduler.genreTab === 'subgenre') loadSubgenres();
      $('#sched-genre-search')?.focus();
    });
    $('#sched-genre-search').addEventListener('input', renderSchedulerGenrePicker);
    $('#sched-genre-list').addEventListener('click', (e) => {
      if (e.target.closest('[data-sched-subgenre-retry]')) {
        loadSubgenres(true);
        return;
      }
      const subBtn = e.target.closest('[data-sched-subgenre-id]');
      if (subBtn) {
        const id = Number(subBtn.dataset.schedSubgenreId);
        const item = S.catalog.items.find((s) => s.id === id);
        if (!item) return;
        const at = S.scheduler.subgenres.findIndex((s) => s.id === id);
        if (at >= 0) S.scheduler.subgenres.splice(at, 1);
        else S.scheduler.subgenres.push(item);
        renderSchedulerGenrePicker();
        return;
      }
      const genreBtn = e.target.closest('[data-sched-genre]');
      if (!genreBtn) return;
      const name = genreBtn.dataset.schedGenre;
      const at = S.scheduler.genres.indexOf(name);
      if (at >= 0) S.scheduler.genres.splice(at, 1);
      else S.scheduler.genres.push(name);
      renderSchedulerGenrePicker();
    });
    $('#sched-genre-clear').addEventListener('click', () => {
      S.scheduler.genres = [];
      S.scheduler.subgenres = [];
      renderSchedulerGenrePicker();
    });
    $('#sched-genre-apply').addEventListener('click', () => toggleSchedulerGenrePopover(false));
    $('#sched-log-clear').addEventListener('click', () => {
      S.scheduler.logs = [];
      renderScheduler();
    });
    $('#sched-start').addEventListener('click', async () => {
      const btn = $('#sched-start');
      btn.disabled = true;
      try {
        const payload = {
          interval_seconds: S.scheduler.seconds,
          genres: S.scheduler.genres.slice(),
          subgenre_ids: S.scheduler.subgenres.map((s) => s.id),
        };
        await host('startScheduler', { params: payload }, 90 * 1000);
        S.scheduler.running = true;
        const target = payload.genres.length || payload.subgenre_ids.length
          ? `（${payload.genres.length} 个曲风，${payload.subgenre_ids.length} 个子曲风）`
          : '（全站）';
        pushLog(`定时任务已启动${target}`, 'accent');
        markDirty('scheduler', 'chrome');
        toast('定时任务已启动', 'success');
      } catch (err) {
        toast(`启动失败：${err.message}`, 'error');
      } finally {
        btn.disabled = false;
      }
    });
    $('#sched-stop').addEventListener('click', async () => {
      try {
        await host('stopScheduler');
        S.scheduler.running = false;
        pushLog('定时任务已停止', '');
        markDirty('scheduler', 'chrome');
        toast('定时任务已停止', 'info');
      } catch (err) {
        toast(`停止失败：${err.message}`, 'error');
      }
    });
  }

  /* ---------------------------------------------------------------- settings */

  function renderSettings() {
    const st = S.settings;
    $$('#quality-grid .option').forEach((btn) => {
      btn.classList.toggle('active', btn.dataset.quality === st.quality);
    });
    const exists = $('#track-exists');
    if (exists && st.trackExists && exists.value !== st.trackExists) exists.value = st.trackExists;

    $$('#name-checks input[data-name]').forEach((input) => {
      input.checked = !!st.naming[input.dataset.name];
    });
    applyNamePreview();

    const path = $('#downloads-path');
    if (path) path.textContent = st.dir || '（未设置）';
    const dirHint = $('#dir-hint');
    if (dirHint) {
      dirHint.textContent = !st.dir
        ? '尚未设置下载目录'
        : (st.writable ? '目录可写，按曲风归档与标签写入正常。' : `目录不可写：${st.dirError || '请更换目录'}`);
      dirHint.style.color = st.dir && !st.writable ? '#ff9fb0' : '';
    }

    const net = st.network;
    const proxyInput = $('#proxy-input');
    if (proxyInput && document.activeElement !== proxyInput) proxyInput.value = net.proxy || '';
    const proxyHint = $('#proxy-hint');
    if (proxyHint) {
      proxyHint.textContent = net.proxy ? `当前代理：${net.proxy}` : '当前：直连（不使用代理）';
      proxyHint.style.color = '';
    }
    const genreEnabled = $('#genre-sort-enabled');
    if (genreEnabled) genreEnabled.checked = !!net.sortByGenre;
    $$('#genre-sort-mode .seg').forEach((btn) => {
      btn.classList.toggle('active', btn.dataset.mode === net.genreSortMode);
    });
    const genreHint = $('#genre-sort-hint');
    if (genreHint) {
      genreHint.textContent = net.sortByGenre
        ? '已启用：新下载完成后会移动到「下载目录/曲风/」下；已有文件不会自动移动。'
        : '未启用：新下载会直接放在下载目录根部。';
      genreHint.style.color = '';
    }

    const user = S.username || (S.authReady ? 'Beatport 用户' : '未登录');
    const nameEl = $('#settings-user');
    if (nameEl) nameEl.textContent = user;
    const authEl = $('#settings-auth');
    if (authEl) authEl.textContent = S.authReady ? '已认证，可下载' : '未登录，部分功能不可用';
    const avatar = $('#settings-avatar');
    if (avatar) avatar.textContent = (user[0] || 'B').toUpperCase();
    const loginBtn = $('#settings-login');
    if (loginBtn) loginBtn.hidden = !!S.authReady;
    const logoutBtn = $('#logout-btn');
    if (logoutBtn) logoutBtn.disabled = !S.authReady;

    const hello = S.hello || {};
    const diagBackend = $('#diag-backend');
    if (diagBackend) {
      diagBackend.textContent = { ready: '已连接', starting: '启动中', down: '未连接' }[S.backend] || '未知';
      diagBackend.style.color = S.backend === 'ready' ? 'var(--accent)' : S.backend === 'down' ? '#ff9fb0' : '';
    }
    const addr = $('#diag-addr');
    if (addr) addr.textContent = hello.backendAddr || '127.0.0.1:8765';
    const wv = $('#diag-webview');
    if (wv) wv.textContent = hello.webviewVersion || '—';
    const mode = $('#diag-mode');
    if (mode) mode.textContent = hello.portable === false ? '用户目录模式' : '便携模式（数据在程序目录）';
    const perf = $('#diag-perf');
    if (perf) {
      perf.textContent = hello.performanceMode === 'low-memory'
        ? '低内存（软件渲染）'
        : hello.performanceMode === 'custom'
          ? '自定义浏览器参数'
          : 'GPU 加速';
    }
    const aboutVersion = $('#about-version');
    if (aboutVersion) aboutVersion.textContent = `Beatport 下载器 ${hello.appVersion || '1.0.0'}`;
    renderSettingsUpdate();
  }

  function setupSettings() {
    $('#theme-grid')?.addEventListener('click', (e) => {
      const btn = e.target.closest('[data-theme-choice]');
      if (!btn) return;
      applyTheme(btn.dataset.themeChoice);
      const labels = { mecha: '已切换为机能工业终端', 'mecha-light': '已切换为机能白灰', classic: '已切换为经典深色' };
      toast(labels[btn.dataset.themeChoice] || '主题已切换', 'success');
    });
    $('#quality-grid').addEventListener('click', async (e) => {
      const btn = e.target.closest('.option');
      if (!btn) return;
      const quality = btn.dataset.quality;
      if (!quality || quality === S.settings.quality) return;
      const prev = S.settings.quality;
      btn.disabled = true;
      try {
        await rpc('setQuality', { quality });
        S.settings.quality = quality;
        renderSettings();
        toast('音频格式已切换', 'success');
      } catch (err) {
        S.settings.quality = prev;
        renderSettings();
        toast(`无法切换音频格式：${err.message}`, 'error', 7000);
      } finally {
        btn.disabled = false;
      }
    });

    $('#track-exists').addEventListener('change', async (e) => {
      const mode = e.target.value;
      const prev = S.settings.trackExists;
      try {
        await rpc('setTrackExists', { mode });
        S.settings.trackExists = mode;
        toast('已存在文件策略已更新', 'success');
      } catch (err) {
        e.target.value = prev;
        toast(`更新失败：${err.message}`, 'error');
      }
    });

    let nameTimer = 0;
    $('#name-checks').addEventListener('change', (e) => {
      const input = e.target.closest('input[data-name]');
      if (!input) return;
      S.settings.naming[input.dataset.name] = input.checked;
      applyNamePreview();
      clearTimeout(nameTimer);
      const template = buildTemplate(S.settings.naming);
      nameTimer = setTimeout(async () => {
        if (template === S.settings.template) return;
        try {
          await rpc('setTrackFileTemplate', { template });
          S.settings.template = template;
          toast('文件名规则已保存', 'success', 2500);
        } catch (err) {
          S.settings.naming = parseTemplate(S.settings.template);
          renderSettings();
          toast(`保存失败：${err.message}`, 'error');
        }
      }, 450);
    });

    $('#pick-dir').addEventListener('click', async () => {
      try {
        const res = await host('pickFolder', { title: '选择下载目录' });
        const path = res && res.path;
        if (!path) return;
        await rpc('setDownloadsDirectory', { path });
        S.settings.dir = path;
        S.settings.writable = true;
        S.settings.dirError = '';
        renderSettings();
        toast('下载目录已更新', 'success');
      } catch (err) {
        toast(`无法设置下载目录：${err.message}`, 'error', 7000);
      }
    });

    $('#open-dir').addEventListener('click', () => {
      if (!S.settings.dir) return;
      host('openPath', { path: S.settings.dir }).catch((err) => toast(err.message, 'error'));
    });

    $('#settings-login').addEventListener('click', () => showLoginModal(false));
    $('#logout-btn').addEventListener('click', async () => {
      if (!S.authReady) return;
      const ok = await confirmDialog({
        title: '退出登录',
        message: '将清除本机凭据与认证缓存，下次启动需要重新登录。下载队列不受影响。',
        confirmText: '退出登录',
        danger: true,
      });
      if (!ok) return;
      try {
        await rpc('logout');
        S.authReady = false;
        markDirty('chrome', 'settings');
        toast('已退出登录', 'info');
        showLoginModal(true);
      } catch (err) {
        toast(`退出登录失败：${err.message}`, 'error');
      }
    });

    // ---- 网络代理 ---------------------------------------------------------
    async function saveProxySettings(value) {
      const proxy = String(value == null ? $('#proxy-input').value : value).trim();
      const open = openTaskCount();
      const ok = await confirmDialog({
        title: '保存代理并重启后端',
        message: proxy
          ? `将保存代理：${proxy}\n后端会重启以使代理立即生效。${open ? `\n当前有 ${open} 个未完成任务，会全部中断。` : ''}`
          : `将关闭代理（直连）并重启后端。${open ? `\n当前有 ${open} 个未完成任务，会全部中断。` : ''}`,
        confirmText: '保存并重启',
        danger: open > 0,
      });
      if (!ok) return;
      const btn = $('#proxy-save');
      const clear = $('#proxy-clear');
      if (btn) btn.disabled = true;
      if (clear) clear.disabled = true;
      S.downloads.maintenance = true;
      try {
        const res = await host('setProxy', { proxy });
        S.settings.network.proxy = (res && res.proxy) || '';
        await loadNetworkSettings();
        toast(proxy ? '代理已保存，后端已重启' : '已改为直连，后端已重启', 'success');
        markDirty('settings', 'downloads', 'chrome');
      } catch (err) {
        toast(`保存代理失败：${err.message}`, 'error', 9000);
      } finally {
        S.downloads.maintenance = false;
        if (btn) btn.disabled = false;
        if (clear) clear.disabled = false;
        pumpDownloads();
      }
    }

    $('#proxy-save').addEventListener('click', () => saveProxySettings());
    $('#proxy-clear').addEventListener('click', () => saveProxySettings(''));
    $('#proxy-input').addEventListener('keydown', (e) => {
      if (e.key === 'Enter') { e.preventDefault(); saveProxySettings(); }
    });

    // ---- 软件更新 ---------------------------------------------------------
    $('#update-check').addEventListener('click', () => checkUpdatesManually(true));
    $('#update-apply').addEventListener('click', showUpdateDialog);
    $('#update-release').addEventListener('click', () => {
      if (!S.update.releaseUrl) return;
      host('openExternal', { url: S.update.releaseUrl }).catch((err) => toast(err.message, 'error'));
    });

    // ---- 曲风分类 ---------------------------------------------------------
    $('#genre-sort-mode').addEventListener('click', (e) => {
      const btn = e.target.closest('.seg');
      if (!btn) return;
      $$('#genre-sort-mode .seg').forEach((b) => b.classList.toggle('active', b === btn));
    });

    $('#genre-sort-save').addEventListener('click', async () => {
      const enabled = $('#genre-sort-enabled').checked;
      const mode = ($('#genre-sort-mode .seg.active') || {}).dataset
        ? $('#genre-sort-mode .seg.active').dataset.mode
        : 'genre';
      const open = openTaskCount();
      const ok = await confirmDialog({
        title: '保存曲风分类设置',
        message: `将${enabled ? '启用' : '关闭'}按曲风分类（${mode === 'subgenre_or_genre' ? '子曲风优先' : '主曲风'}），`
          + '并重启后端使设置生效。\n已有文件不会被移动，只影响之后完成的下载。'
          + (open ? `\n当前有 ${open} 个未完成任务，会全部中断。` : ''),
        confirmText: '保存并重启',
        danger: open > 0,
      });
      if (!ok) return;
      const btn = $('#genre-sort-save');
      if (btn) btn.disabled = true;
      S.downloads.maintenance = true;
      try {
        await host('setGenreSort', { enabled, mode });
        await loadNetworkSettings();
        toast(enabled ? '已启用曲风分类，后端已重启' : '已关闭曲风分类，后端已重启', 'success', 5000);
        markDirty('settings', 'downloads', 'chrome');
      } catch (err) {
        toast(`保存失败：${err.message}`, 'error', 9000);
      } finally {
        S.downloads.maintenance = false;
        if (btn) btn.disabled = false;
        pumpDownloads();
      }
    });

    $('#diag-reconnect').addEventListener('click', async () => {
      const btn = $('#diag-reconnect');
      btn.disabled = true;
      try {
        await host('restartBackend');
        toast('后端已重启', 'success');
      } catch (err) {
        toast(`重启后端失败：${err.message}`, 'error', 8000);
      } finally {
        btn.disabled = false;
      }
    });

    $('#diag-log').addEventListener('click', async () => {
      try {
        const res = await host('backendTail');
        const pre = $('#log-tail');
        pre.hidden = false;
        pre.textContent = (res && res.text) || '（日志为空）';
        pre.scrollTop = pre.scrollHeight;
      } catch (err) {
        toast(err.message, 'error');
      }
    });

    $('#diag-logs-dir').addEventListener('click', () => {
      host('openLogs').catch((err) => toast(err.message, 'error'));
    });
  }

  /* ------------------------------------------------------------------ boot */

  function setupChromeEvents() {
    $$('.nav-item').forEach((btn) => btn.addEventListener('click', () => {
      const view = btn.dataset.view;
      setView(view);
      pulseView(view);
      if (view === 'search') setTimeout(() => $('#search-input').focus(), 30);
    }));
    $('#collapse-btn')?.addEventListener('click', toggleRail);
    $('#account-row').addEventListener('click', () => {
      if (S.authReady) setView('settings');
      else showLoginModal(false);
    });
    $('#account-row').addEventListener('keydown', (e) => {
      if (e.key === 'Enter' || e.key === ' ') {
        e.preventDefault();
        if (S.authReady) setView('settings');
        else showLoginModal(false);
      }
    });
    $('#account-action')?.addEventListener('click', (e) => {
      e.stopPropagation();
      if (S.authReady) setView('settings');
      else showLoginModal(false);
    });
    $('#backend-pill').addEventListener('click', async () => {
      if (S.backend === 'ready') {
        toast('后端连接正常', 'success', 2200);
        return;
      }
      try {
        await host('restartBackend');
        toast('已请求重启后端', 'info');
      } catch (err) {
        toast(err.message, 'error', 7000);
      }
    });
    $('#conn-retry').addEventListener('click', async () => {
      try {
        await host('restartBackend');
        toast('已请求重启后端', 'info');
      } catch (err) {
        toast(err.message, 'error', 7000);
      }
    });
    $('#conn-logs').addEventListener('click', () => host('openLogs').catch(() => {}));
    const updateSlot = $('#update-slot');
    if (updateSlot) {
      updateSlot.addEventListener('click', (e) => {
        if (e.target.closest('[data-update-open]')) showUpdateDialog();
      });
    }
    $('#selection-dock').addEventListener('click', (e) => {
      const listId = $('#selection-dock').dataset.list || currentListId();
      if (e.target.closest('#dock-clear')) { setAllSelected(listId, false); return; }
      if (e.target.closest('#dock-invert')) {
        const st = listState(listId);
        const next = new Set();
        st.items.forEach((it) => { if (!st.selected.has(it.url)) next.add(it.url); });
        st.selected = next;
        markDirty(listId);
        window.__vl?.[listId]?.refresh();
        updateSelectionDock();
        return;
      }
      if (e.target.closest('#dock-download')) {
        const st = listState(listId);
        const urls = [];
        const titles = {};
        st.items.forEach((it) => {
          if (st.selected.has(it.url)) { urls.push(it.url); titles[it.url] = displayTitle(it); }
        });
        enqueueUrls(urls, titles);
      }
    });
    document.addEventListener('keydown', (e) => {
      if (e.key === 'Escape') {
        if (S.update.applying) return;
        if (!modalLayer().hidden) { closeModal(); return; }
        if (!$('#genre-popover').hidden) { toggleGenrePopover(false); return; }
      if (!$('#sched-genre-popover').hidden) { toggleSchedulerGenrePopover(false); return; }
        if (S.view === 'downloads' && S.downloads.selected.size) { clearDownloadSelection(); return; }
        if (!$('#selection-dock').hidden) { setAllSelected(currentListId(), false); }
        return;
      }
      if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'k') {
        e.preventDefault();
        setView('search');
        $('#search-input').focus();
        return;
      }
      if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'b') {
        e.preventDefault();
        toggleRail();
        return;
      }
      if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'a' && !isEditable(e.target)) {
        if (S.view === 'downloads') {
          e.preventDefault();
          filteredDownloads().forEach((r) => S.downloads.selected.add(r.url));
          S.downloads.anchor = -1;
          markDirty('downloads');
          window.__vl?.downloads?.refresh();
          return;
        }
        if (S.view === 'discover' || S.view === 'search') {
          e.preventDefault();
          setAllSelected(currentListId(), true);
          return;
        }
      }
      if (e.key === '/' && !isEditable(e.target)) {
        e.preventDefault();
        setView('search');
        $('#search-input').focus();
      }
    });
  }

  function installBridgeListener() {
    if (!bridge.available) return;
    window.chrome.webview.addEventListener('message', (e) => {
      let data = e.data;
      if (typeof data === 'string') {
        try { data = JSON.parse(data); } catch { return; }
      }
      onHostMessage(data);
    });
  }

  function init() {
    restoreDownloads();
    if (S.navRail) $('#app').classList.add('rail');

    // Preview/debug helpers: ?view=downloads opens a specific page, and
    // ?mock=1 forces the fixture bridge even inside WebView2.
    const params = new URLSearchParams(location.search);
    const forcedView = params.get('view');
    if (forcedView && PAGE_META[forcedView]) S.view = forcedView;
    const forceMock = params.get('mock') === '1';

    setupChromeEvents();
    setupDiscover();
    setupSearch();
    setupDownloads();
    setupScheduler();
    setupSettings();
    setupWindowbar();

    applyTheme(S.theme, false);
    setView(PAGE_META[S.view] ? S.view : 'discover');
    renderChrome();
    renderDiscover();
    renderSearch();
    renderDownloads();
    renderScheduler();
    renderSettings();
    window.__vl?.discover?.setItems([]);
    window.__vl?.search?.setItems([]);
    window.__vl?.downloads?.setItems(filteredDownloads());

    if (bridge.available && !forceMock) {
      installBridgeListener();
      rawPost({ kind: 'ui', action: 'ready' });
    } else {
      bridge.available = false;
      Mock.start();
    }
    setTimeout(checkUpdatesOnStartup, 900);
    setTimeout(refreshGenresAfterStartup, 1500);
    if (params.get('genretab') === 'subgenre') {
      setTimeout(() => {
        toggleGenrePopover(true);
        S.discover.genreTab = 'subgenre';
        loadSubgenres();
      }, 2200);
    }
    if (params.get('updialog') === '1') {
      setTimeout(() => { if (S.update.hasUpdate) showUpdateDialog(); }, 1900);
    }
    uiLog('info', `界面已加载，模式=${bridge.available && !forceMock ? 'webview2' : 'mock'}`);
    if (params.get('selftest') === '1') {
      // Hidden smoke test used by the build pipeline: drives the UI headlessly
      // and reports the outcome through document.title.
      setTimeout(runSelfTest, 1400);
    }
  }

  // A compact end-to-end UI smoke test. It only runs with ?selftest=1 and is
  // never reachable from normal interaction.
  async function runSelfTest() {
    const results = [];
    const wait = (ms) => new Promise((r) => setTimeout(r, ms));
    const check = (name, cond, extra) => { results.push(`${cond ? 'PASS' : 'FAIL'} ${name}${extra ? `(${extra})` : ''}`); return cond; };
    try {
      for (let i = 0; i < 40 && S.discover.items.length === 0; i++) await wait(150);
      await wait(450);
      check('discover-loaded', S.discover.items.length > 0, String(S.discover.items.length));
      check('windowbar', !!$('#windowbar') && !!$('#win-close') && !$('#collapse-btn'), 'custom');
      check('genres-loaded', S.genres.length > 0, String(S.genres.length));
      check('scheduler-picker', !!$('#sched-genre-trigger') && !!$('#sched-genre-popover'), 'present');
      check('artists-string', !String(S.discover.items[0] && S.discover.items[0].artists).includes('[object'),
        S.discover.items[0] && S.discover.items[0].artists);

      let firstRow = $('#discover-rows .row[data-index="20"]') || $('#discover-rows .row');
      if (!firstRow) {
        window.__vl?.discover?.refresh();
        await wait(150);
        firstRow = $('#discover-rows .row[data-index="20"]') || $('#discover-rows .row');
      }
      check('row-rendered', !!firstRow, String($$('#discover-rows .row').length));
      if (firstRow) {
        firstRow.dispatchEvent(new MouseEvent('click', { bubbles: true }));
        check('row-select', S.discover.selected.size === 1, String(S.discover.selected.size));
        check('dock-visible', $('#selection-dock').hidden === false);
      }

      const before = S.downloads.map.size;
      $('#dock-download').dispatchEvent(new MouseEvent('click', { bubbles: true }));
      await wait(700);
      check('enqueue', S.downloads.map.size > before, `${before}->${S.downloads.map.size}`);

      $('.nav-item[data-view="downloads"]').dispatchEvent(new MouseEvent('click', { bubbles: true }));
      await wait(350);
      if (!$$('#download-rows .dl-row').length) {
        renderDownloads();
        window.__vl?.downloads?.refresh();
        await wait(250);
      }
      check('downloads-view', $$('#download-rows .dl-row').length > 0,
        `rows=${$$('#download-rows .dl-row').length}, items=${filteredDownloads().length}, vl=${!!window.__vl?.downloads}, spacer=${$('#download-spacer').style.height}`);

      // Progress updates must never reorder rows.
      const orderBefore = filteredDownloads().map((r) => r.url);
      const sample = filteredDownloads().find((r) => r.status === 'downloading' || r.status === 'queued');
      if (sample) {
        handleDownloadEvent({
          event: 'download.progress', url: sample.url, title: sample.title,
          percent: 90, received: 900, total: 1000,
        });
      }
      const orderAfter = filteredDownloads().map((r) => r.url);
      check('downloads-order-stable', JSON.stringify(orderBefore) === JSON.stringify(orderAfter));

      const wasPaused = S.downloads.pausedAll;
      togglePauseAllDownloads();
      const pausedOk = S.downloads.pausedAll !== wasPaused;
      togglePauseAllDownloads();
      check('pause-toggle', pausedOk && S.downloads.pausedAll === wasPaused);

      // Stop-all flow (mock host restarts instantly).
      stopAllDownloads();
      await wait(300);
      const stopBtn = Array.from(document.querySelectorAll('#modal-layer .btn'))
        .find((b) => b.textContent.trim() === '停止全部');
      if (stopBtn) stopBtn.click();
      await wait(1000);
      check('stop-all', Array.from(S.downloads.map.values()).every((r) => !isNonTerminal(r)),
        String(S.downloads.map.size));

      const doomed = filteredDownloads().find((r) => r.status === 'stopped'
        || r.status === 'done' || r.status === 'error' || r.status === 'skipped');
      if (doomed) {
        S.downloads.selected.add(doomed.url);
        await deleteSelectedDownloads();
        await wait(120);
        check('delete-selected', !S.downloads.map.has(doomed.url));
      }

      $('.nav-item[data-view="settings"]').dispatchEvent(new MouseEvent('click', { bubbles: true }));
      await wait(200);
      check('quality-active', $$('#quality-grid .option.active').length === 1);
      check('theme-picker', $$('#theme-grid .theme-choice').length === 3, String($$('#theme-grid .theme-choice').length));
      const lightTheme = $('[data-theme-choice="mecha-light"]');
      if (lightTheme) {
        lightTheme.click();
        await wait(80);
        check('theme-light', document.documentElement.dataset.mechaVariant === 'light', document.documentElement.dataset.mechaVariant);
      }
      const classicTheme = $('[data-theme-choice="classic"]');
      if (classicTheme) {
        classicTheme.click();
        await wait(80);
        check('theme-classic', document.documentElement.dataset.theme === 'classic', document.documentElement.dataset.theme);
        $('[data-theme-choice="mecha"]')?.click();
        await wait(80);
      }
      if (new URLSearchParams(location.search).get('update') === '1') {
        await wait(250);
        const pill = $('#update-slot .update-pill');
        check('update-pill', !!pill);
        if (pill) {
          pill.click();
          await wait(150);
          check('update-dialog', !!$('.modal .update-notes'));
          closeModal();
          openUpdateProgressModal();
          updateProgressDom(42, '正在下载更新包…');
          check('update-progress', !!$('#update-progress-bar') && $('#update-progress-bar').style.width === '42%');
          closeModal();
          await startUpdate(false);
          check('update-apply', !S.update.applying && modalLayer().hidden);
        }
      }

      $('.nav-item[data-view="discover"]').dispatchEvent(new MouseEvent('click', { bubbles: true }));
      await wait(120);
      $('#genre-trigger').dispatchEvent(new MouseEvent('click', { bubbles: true }));
      await wait(80);
      const subTab = $('[data-genre-tab="subgenre"]');
      if (subTab) {
        subTab.dispatchEvent(new MouseEvent('click', { bubbles: true }));
        await wait(400);
        check('subgenre-table', $$('#genre-list .subgenre-row').length > 0, String($$('#genre-list .subgenre-row').length));
        $('[data-genre-tab="genre"]').dispatchEvent(new MouseEvent('click', { bubbles: true }));
        await wait(80);
      }
      const genreBtn = $('#genre-list .genre-option');
      if (genreBtn) genreBtn.dispatchEvent(new MouseEvent('click', { bubbles: true }));
      check('genre-pick', S.discover.genres.length === 1, S.discover.genres.join(','));
      $('#genre-apply').dispatchEvent(new MouseEvent('click', { bubbles: true }));

      $('.nav-item[data-view="search"]').dispatchEvent(new MouseEvent('click', { bubbles: true }));
      $('#search-input').value = 'Anyma';
      $('#search-btn').dispatchEvent(new MouseEvent('click', { bubbles: true }));
      await wait(900);
      check('search-results', S.search.items.length > 0, String(S.search.items.length));

      const failed = results.filter((r) => r.startsWith('FAIL'));
      document.title = `SELFTEST ${failed.length ? 'FAIL' : 'OK'} :: ${results.join(' | ')}`;
      const panel = document.createElement('pre');
      panel.id = 'selftest-report';
      panel.style.cssText = 'position:fixed;left:12px;bottom:12px;z-index:999;max-width:760px;padding:12px;border-radius:10px;'
        + 'background:#000d;border:1px solid #333;color:#0f9;font:12px/1.5 Consolas,monospace;white-space:pre-wrap';
      panel.textContent = `SELFTEST ${failed.length ? 'FAIL' : 'OK'}\n${results.join('\n')}`;
      document.body.appendChild(panel);
    } catch (err) {
      document.title = `SELFTEST FAIL :: exception ${err && err.message} :: ${results.join(' | ')}`;
      const panel = document.createElement('pre');
      panel.id = 'selftest-report';
      panel.style.cssText = 'position:fixed;left:12px;bottom:12px;z-index:999;padding:12px;background:#000d;border:1px solid #333;color:#f88;font:12px/1.5 Consolas,monospace;white-space:pre-wrap';
      panel.textContent = `SELFTEST EXCEPTION: ${err && (err.stack || err.message)}\n${results.join('\n')}`;
      document.body.appendChild(panel);
    }
  }

  /* =========================================================================
     Mock bridge — used only when opened outside WebView2 (design preview and
     headless screenshots). It mirrors the real host protocol.
     ========================================================================= */

  const Mock = (() => {
    const GENRES = ['Afro House', 'Bass House', 'Breaks', 'Deep House', 'Disco', 'Drum & Bass',
      'Electronica', 'Funky House', 'Hard Techno', 'Indie Dance', 'Melodic House & Techno',
      'Minimal / Deep Tech', 'Organic House', 'Progressive House', 'Psy-Trance', 'Tech House',
      'Techno', 'Trance', 'UK Garage / Bassline', 'House'];

    const ARTISTS = ['Adam Beyer', 'Charlotte de Witte', 'Anyma', 'ARTBAT', 'CamelPhat', 'Fisher',
      'Chris Lake', 'Dom Dolla', 'John Summit', 'Purple Disco Machine', 'Solomun', 'Tale Of Us',
      'Boris Brejcha', 'Amelie Lens', 'Nina Kraviz', 'Sub Focus', 'Dimension', 'Above & Beyond',
      'Armin van Buuren', 'Ben Böhmer', 'Marsh', 'Lane 8', 'Yotto', 'Tinlicker', 'Eli Brown',
      'Peggy Gou', 'Honey Dijon', 'Vintage Culture', 'Mochakk', 'Kölsch', 'Adriatique', 'Mathame',
      'Massano', 'Kevin de Vries', 'Space Motion', 'Silver Panda', 'Enamour'];

    const TRACKS = ['Strobe', 'Conspiracy', 'Silly Things', 'Innerbloom', 'Opus', 'Sun & Moon',
      'Losing It', 'Free Your Mind', 'Blur', 'The Age Of Love', 'Ghost Voices', 'Rave',
      'Kick That', 'Don\'t Go', 'Acid Phase', 'Warehouse', 'No Sleep', 'After Dark', 'Voyage',
      'Eclipse', 'Nightfall', 'Resonance', 'Mirage', 'Pulse', 'Gravity', 'Reflection',
      'Momentum', 'Horizon', 'Orbit', 'Neon Sky', 'Solar', 'Frequency', 'Echoes', 'Static',
      'Undertow', 'Aurora', 'Cascade', 'Drift', 'Fracture', 'Prism'];

    const MIXES = ['Original Mix', 'Extended Mix', 'Club Mix', 'Dub Mix', 'Radio Edit'];

    const SUBGENRES = [
      { id: 9001, name: 'Acid', genreName: 'Techno' },
      { id: 9002, name: 'Peak Time', genreName: 'Techno' },
      { id: 9003, name: 'Hypnotic', genreName: 'Techno' },
      { id: 9004, name: 'Deep Tech', genreName: 'Minimal / Deep Tech' },
      { id: 9005, name: 'Afro Melodic', genreName: 'Afro House' },
      { id: 9006, name: 'Soulful', genreName: 'House' },
      { id: 9007, name: 'Jackin', genreName: 'House' },
      { id: 9008, name: 'Progressive', genreName: 'Progressive House' },
      { id: 9009, name: 'Melodic', genreName: 'Melodic House & Techno' },
      { id: 9010, name: 'Liquid', genreName: 'Drum & Bass' },
      { id: 9011, name: 'Neurofunk', genreName: 'Drum & Bass' },
      { id: 9012, name: 'Jump Up', genreName: 'Drum & Bass' },
      { id: 9013, name: 'Deep Dubstep', genreName: '140 / Deep Dubstep / Grime' },
      { id: 9014, name: 'Psytrance', genreName: 'Psy-Trance' },
      { id: 9015, name: 'Uplifting', genreName: 'Trance' },
      { id: 9016, name: 'Indie Dance', genreName: 'Indie Dance' },
      { id: 9017, name: 'Nu Disco', genreName: 'Disco' },
      { id: 9018, name: 'Amapiano', genreName: 'Amapiano' },
      { id: 9019, name: 'Afrobeats', genreName: 'African' },
      { id: 9020, name: 'Bassline', genreName: 'UK Garage / Bassline' },
    ];

    function slug(s) {
      return String(s).toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '');
    }

    function makeTrack(i) {
      const name = TRACKS[i % TRACKS.length];
      const artist = ARTISTS[(i * 7) % ARTISTS.length];
      const artist2 = ARTISTS[(i * 13 + 5) % ARTISTS.length];
      const genre = GENRES[(i * 3) % GENRES.length];
      const mix = MIXES[i % MIXES.length];
      const id = 19000000 + i * 137;
      return {
        url: `https://www.beatport.com/track/${slug(name)}/${id}`,
        id,
        name,
        title: name,
        mix_name: mix,
        artists: i % 4 === 0 ? `${artist}, ${artist2}` : artist,
        genre,
        type: 'track',
      };
    }

    function makeRelease(i) {
      const name = `${TRACKS[(i * 5) % TRACKS.length]} EP`;
      const id = 4500000 + i * 91;
      return {
        url: `https://www.beatport.com/release/${slug(name)}/${id}`,
        id,
        name,
        title: name,
        artists: ARTISTS[(i * 11) % ARTISTS.length],
        genre: '',
        type: 'release',
      };
    }

    const allTracks = Array.from({ length: 120 }, (_, i) => makeTrack(i));
    const allReleases = Array.from({ length: 40 }, (_, i) => makeRelease(i));
    let onMessage = null;
    let genreCalls = 0;

    function send(msg) {
      setTimeout(() => { if (onMessage) onMessage(msg); }, 80 + Math.random() * 90);
    }

    function reply(id, kind, result) {
      send({ kind: `${kind}.result`, id, result });
    }

    function fail(id, kind, message) {
      send({ kind: `${kind}.error`, id, error: { message } });
    }

    function start() {
      onMessage = onHostMessage;
      send({ kind: 'hello', data: {
        appVersion: '1.0.0', webviewVersion: '157.0.4341.60', backendAddr: '127.0.0.1:8765',
        backendLog: 'D:/Project/BPDL/bin/logs/backend.log', guiLog: 'D:/Project/BPDL/bin/logs/gui.log',
        dataDir: 'D:/Project/BPDL/bin/data', exeDir: 'D:/Project/BPDL/bin',
        configFile: 'D:/Project/BPDL/bin/beatportdl-config.yml', portable: true, debug: false,
      } });
      setTimeout(() => send({ kind: 'backend', state: 'ready', message: '' }), 260);
      setTimeout(() => send({ kind: 'events', connected: true, message: '' }), 320);
      // Seed a couple of downloads so the queue view is meaningful in previews.
      setTimeout(() => {
        const evs = [          { event: 'download.started', url: allTracks[0].url, title: `${allTracks[0].name} (${allTracks[0].mix_name})` },
          { event: 'download.progress', url: allTracks[0].url, title: `${allTracks[0].name} (${allTracks[0].mix_name})`, percent: 42, received: 25 * 1024 * 1024, total: 60 * 1024 * 1024 },
          { event: 'download.started', url: allTracks[3].url, title: `${allTracks[3].name} (${allTracks[3].mix_name})` },
          { event: 'download.progress', url: allTracks[3].url, title: `${allTracks[3].name} (${allTracks[3].mix_name})`, percent: 78, received: 40 * 1024 * 1024, total: 51 * 1024 * 1024 },
          { event: 'download.done', url: allTracks[7].url, title: `${allTracks[7].name} (${allTracks[7].mix_name})`, path: 'C:/Users/Public/Downloads/BeatportDL/Techno/Dark Room.flac' },
          { event: 'download.skipped', url: allTracks[9].url, title: `${allTracks[9].name} (${allTracks[9].mix_name})`, reason: 'exists' },
          { event: 'download.error', url: allTracks[11].url, title: `${allTracks[11].name} (${allTracks[11].mix_name})`, error: 'integrity check failed: cannot decode audio' },
          { event: 'log', message: 'scheduler: starting scan' },
        ];
        send({ kind: 'events.batch', events: evs });
      }, 2000);
      if (location.search.indexOf('preselect=1') >= 0) {
        setTimeout(() => {
          filteredDownloads().slice(0, 3).forEach((r) => S.downloads.selected.add(r.url));
          markDirty('downloads');
          window.__vl?.downloads?.setItems(filteredDownloads());
        }, 2600);
      }
    }

    function receive(msg) {
      const kind = msg.kind;
      if (kind === 'ui' && msg.action === 'ready') return;
      if (kind === 'ui' && msg.action === 'log') { return; }
      if (kind !== 'rpc' && kind !== 'host') return;
      const t = kind === 'rpc' ? 'rpc' : 'host';
      switch (msg.method || msg.action) {
        case 'authStatus': return reply(msg.id, t, { ready: location.search.indexOf('login=1') < 0 });
        case 'getGenres': {
          genreCalls += 1;
          if (location.search.includes('genresempty=1') && genreCalls === 1) {
            return reply(msg.id, t, []);
          }
          return reply(msg.id, t, GENRES.map((name) => ({ name, slug: slug(name) })));
        }
        case 'getGenreCatalog':
          return reply(msg.id, t, { genres: [], subgenres: SUBGENRES });
        case 'getLatestBySubgenres': {
          const p = msg.params || {};
          const page = Number(p.page) || 1;
          const per = Number(p.per_page) || 60;
          const start = (page - 1) * per;
          const items = allTracks.slice(start, start + per).map((it) => ({
            url: it.url, id: it.id, name: it.name, title: it.title,
            mix_name: it.mix_name, artists: it.artists, genre: it.genre, type: 'track',
          }));
          return reply(msg.id, t, {
            results: items, page, per_page: per,
            count: allTracks.length, has_more: start + per < allTracks.length,
          });
        }
        case 'getLatestDiscover': {
          const p = msg.params || {};
          const kindWant = p.kind === 'releases' ? 'releases' : 'tracks';
          const page = Number(p.page) || 1;
          const per = Number(p.per_page) || 50;
          const pool = kindWant === 'releases' ? allReleases : allTracks;
          const genres = Array.isArray(p.genres) ? p.genres : [];
          const filtered = genres.length ? pool.filter((it) => !it.genre || genres.includes(it.genre)) : pool;
          const start = (page - 1) * per;
          const cover = "data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='80' height='80'%3E%3Crect width='80' height='80' fill='%23151a16'/%3E%3Ccircle cx='40' cy='40' r='18' fill='%23c7b25c' opacity='.45'/%3E%3C/svg%3E";
          const results = filtered.slice(start, start + per).map((it) => ({
            url: it.url, id: it.id, name: it.name, title: it.title,
            mix_name: it.mix_name, artists: it.artists, genre: it.genre,
            type: kindWant === 'releases' ? 'release' : 'track', cover,
          }));
          return reply(msg.id, t, {
            results, page, per_page: per,
            count: filtered.length, has_more: start + per < filtered.length,
          });
        }
        case 'getSettings': return reply(msg.id, t, {
          downloads_directory: 'C:/Users/Public/Downloads/BeatportDL',
          writable: true, quality: 'lossless', track_exists: 'skip',
          track_file_template: '{number}. {artists} - {name} ({mix_name})',
        });
        case 'getLatest': {
          const kindWant = (msg.params && msg.params.kind) === 'releases' ? 'releases' : 'tracks';
          const page = (msg.params && msg.params.page) || 1;
          const pool = kindWant === 'releases' ? allReleases : allTracks;
          const genres = (msg.params && msg.params.genres) || (msg.params && msg.params.genre ? [msg.params.genre] : []);
          let filtered = pool;
          if (genres.length) filtered = pool.filter((it) => !it.genre || genres.includes(it.genre));
          const per = 60;
          const start = (page - 1) * per;
          const results = filtered.slice(start, start + per);
          return reply(msg.id, t, { results, page, per_page: per, count: filtered.length, has_more: start + per < filtered.length });
        }
        case 'search': {
          const q = String((msg.params && msg.params.query) || '').toLowerCase();
          const results = allTracks.filter((it) => it.name.toLowerCase().includes(q) || it.artists.toLowerCase().includes(q))
            .slice(0, 40).map((it) => ({ type: 'track', id: it.id, title: `${it.name} (${it.mix_name})`, artists: it.artists, url: it.url, genre: it.genre }));
          return reply(msg.id, t, results);
        }
        case 'download': {
          const urls = (msg.params && msg.params.urls) || [];
          const titleOf = (url) => {
            const slug = String(url).split('/').filter(Boolean);
            return (slug[slug.length - 1] || 'track') + ' (Original Mix)';
          };
          urls.forEach((url, i) => {
            setTimeout(() => {
              const title = titleOf(url);
              send({ kind: 'events.batch', events: [{ event: 'download.started', url, title }] });
              let pct = 0;
              const total = 40 * 1024 * 1024;
              const tick = setInterval(() => {
                pct = Math.min(100, pct + 15 + Math.floor(Math.random() * 20));
                if (pct >= 100) {
                  clearInterval(tick);
                  send({ kind: 'events.batch', events: [
                    { event: 'download.progress', url, title, percent: 100, received: total, total },
                    { event: 'download.done', url, title, path: `C:/Users/Public/Downloads/BeatportDL/Techno/${title}.flac` },
                  ] });
                } else {
                  send({ kind: 'events.batch', events: [
                    { event: 'download.progress', url, title, percent: pct, received: Math.floor(total * pct / 100), total },
                  ] });
                }
              }, 420 + i * 60);
            }, 250 + i * 45);
          });
          return reply(msg.id, t, { enqueued: urls.length, skipped: 0 });
        }
        case 'getNetworkSettings':
          return reply(msg.id, t, {
            proxy: '', sort_by_genre: true, genre_sort_mode: 'genre',
            configPath: 'D:/Project/BPDL/bin/beatportdl-config.yml', ownsBackend: true,
          });
        case 'checkUpdate': {
          const wantsUpdate = new URLSearchParams(location.search).get('update') === '1';
          const info = {
            current: '1.0.0',
            latest: wantsUpdate ? '1.1.0' : '1.0.0',
            hasUpdate: wantsUpdate,
            name: wantsUpdate ? 'v1.1.0 · Beatport 下载器' : 'v1.0.0',
            notes: wantsUpdate
              ? '• 新增 GitHub 自动更新与右上角更新提示\n• 下载队列支持暂停 / 停止 / 删除\n• 设置页新增网络代理与曲风分类\n• 下载顺序更加稳定'
              : '',
            publishedAt: wantsUpdate ? new Date().toISOString() : '',
            releaseUrl: 'https://github.com/ichiryu0021/beatportdl/releases',
            repo: 'ichiryu0021/beatportdl',
            asset: wantsUpdate ? 'bpdl-hotfix-v1.1.0.zip' : '',
            fullAsset: wantsUpdate ? 'bpdl-v1.1.0-full.zip' : '',
            checkedAt: new Date().toISOString(),
          };
          return reply(msg.id, t, info);
        }
        case 'applyUpdate': {
          setTimeout(() => send({ kind: 'update', state: 'downloading', percent: 36, received: 18 * 1024 * 1024, total: 50 * 1024 * 1024, message: '正在下载更新包…' }), 350);
          setTimeout(() => send({ kind: 'update', state: 'applying', percent: 88, stage: 'preparing', message: '正在校验并安装…' }), 800);
          setTimeout(() => reply(msg.id, t, { ok: true, restarting: false, mock: true }), 1200);
          return;
        }
        case 'setProxy':
          return reply(msg.id, t, { proxy: msg.proxy || '', restarted: true });
        case 'setGenreSort':
          return reply(msg.id, t, {
            sort_by_genre: !!msg.enabled, genre_sort_mode: msg.mode || 'genre', restarted: true,
          });
        case 'stopDownloads':
          return reply(msg.id, t, { stopped: true, restarted: true });
        case 'schedulerStatus': return reply(msg.id, t, { running: false });
        case 'startScheduler':
        case 'stopScheduler':
        case 'setQuality':
        case 'setTrackExists':
        case 'setTrackFileTemplate':
        case 'setDownloadsDirectory':
        case 'login':
        case 'logout': return reply(msg.id, t, { ok: true });
        case 'pickFolder': return reply(msg.id, t, { path: 'C:/Users/Public/Downloads/BeatportDL' });
        case 'backendTail': return reply(msg.id, t, { text: '[mock] backend log not available' });
        case 'info': return reply(msg.id, t, { appVersion: '1.0.0' });
        default:
          if (msg.action === 'restartBackend') return reply(msg.id, t, { ok: true });
          return fail(msg.id, t, `mock: 未实现 ${msg.method || msg.action}`);
      }
    }

    return { start, receive };
  })();

  /* ---------------------------------------------------------------- go */

  // Surface unexpected script errors in the host log so failures in the field
  // can be diagnosed from logs/gui.log without opening DevTools.
  window.addEventListener('error', (e) => {
    try {
      uiLog('error', `${e.message} @ ${e.filename}:${e.lineno}:${e.colno}`);
    } catch { /* noop */ }
  });
  window.addEventListener('unhandledrejection', (e) => {
    try {
      const reason = e.reason && (e.reason.message || e.reason);
      uiLog('error', `unhandled rejection: ${reason}`);
    } catch { /* noop */ }
  });

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }
})();


