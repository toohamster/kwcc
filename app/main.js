/* app/main.js — module registry + init (loaded once in C init) */

/* ── load runtime ── */
load("app/runtime/store.js");
load("app/runtime/bus.js");
load("app/constant/topic.js");

/* ── global module registry ── */
var modules = new Object();
var moduleKeys = [];

function registerModule(name, mod) {
    modules[name] = mod;
    moduleKeys.push(name);
}

function registerModuleView(name, renderFn) {
    modules[name].render = renderFn;
}

function initStore() {
    var initState = new Object();
    var allActions = new Object();
    var i, key, m;
    for (i = 0; i < moduleKeys.length; i++) {
        key = moduleKeys[i];
        m = modules[key];
        if (m.state) initState[key] = m.state;
        if (m.actions) allActions[key] = m.actions;
    }
    $store = createStore({ state: initState, actions: allActions });
}

function initEvents() {
    $bus = createBus();
    var i, key, m;
    for (i = 0; i < moduleKeys.length; i++) {
        key = moduleKeys[i];
        m = modules[key];
        if (m.initEvents) m.initEvents();
    }
}

/* ── load modules (logic first, then view) ── */
load("app/modules/test.js");
load("app/modules/test_view.js");

/* ── run init ── */
initStore();
initEvents();

/* ── frame function (called every frame from C) ── */
function onFrame() {
    var i, key, m;
    for (i = 0; i < moduleKeys.length; i++) {
        key = moduleKeys[i];
        m = modules[key];
        if (m.render) {
            m.render($store.state[key]);
        }
    }
}
