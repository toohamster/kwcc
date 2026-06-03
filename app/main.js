/* app/main.js — module registry + init (loaded once in C init) */

/* ── load runtime ── */
load("app/runtime/store.js");
load("app/runtime/bus.js");

/* ── configure memory pools ── */
$config.setAppSize(256 * 1024);
$config.setUserSize(1 * 1024 * 1024);

/* ── framework global variables ($ prefix) ── */
var $modules = new Object();
var $moduleKeys = [];
var $topics = new Object();
var $loadedFiles = new Object();

function loadJs(path, once) {
    if (once === undefined) once = 1; /* default: load once only */
    if (!$loadedFiles[path]) {
        $loadedFiles[path] = 0;
    }
    if (once) {
        if ($loadedFiles[path] > 0) return; /* already loaded, skip */
    }
    load(path);
    $loadedFiles[path] = $loadedFiles[path] + 1;
}

function registerModule(name, mod) {
    if ($modules[name]) return; /* already registered, skip */
    $modules[name] = mod;
    $moduleKeys.push(name);
}

function registerModuleView(name, renderFn) {
    if ($modules[name] && $modules[name].render) return; /* already registered, skip */
    $modules[name].render = renderFn;
}

function registerTopic(name, topics) {
    if ($topics[name]) return; /* already registered, skip */
    $topics[name] = topics;
}

function initStore() {
    var initState = new Object();
    var allActions = new Object();
    var i, key, m;
    for (i = 0; i < $moduleKeys.length; i++) {
        key = $moduleKeys[i];
        m = $modules[key];
        if (m.state) initState[key] = m.state;
        if (m.actions) allActions[key] = m.actions;
    }
    $store = createStore({ state: initState, actions: allActions });
}

function initEvents() {
    $bus = createBus();
    var i, key, m;
    for (i = 0; i < $moduleKeys.length; i++) {
        key = $moduleKeys[i];
        m = $modules[key];
        if (m.initEvents) m.initEvents();
    }
}

/* ── load modules (topics first, then logic, then view) ── */
loadJs("app/modules/examples/test/test.js");
loadJs("app/modules/examples/test/test_view.js");
loadJs("app/modules/examples/calc/calc.js");
loadJs("app/modules/examples/calc/calc.js");
loadJs("app/modules/examples/calc/calc_view.js");
loadJs("app/modules/examples/svg/svg.js");
loadJs("app/modules/examples/svg/svg_view.js");

/* ── run init ── */
initStore();
initEvents();

/* ── frame function (called every frame from C) ── */
function onFrame() {
    var i, key, m;
    for (i = 0; i < $moduleKeys.length; i++) {
        key = $moduleKeys[i];
        m = $modules[key];
        if (m.render) {
            /* Auto-sync module state to C layer (visible, etc.) */
            ui.sync(key, $store.state[key].visible);
            m.render($store.state[key]);
        }
    }
}
