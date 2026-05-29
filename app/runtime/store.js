/* runtime/store.js — global state + dual-param dispatch + middleware */

var $store = null;

function createStore(opts) {
    var state = opts.state;
    var allActions = opts.actions;
    var middlewares = opts.middlewares || [];

    function dispatch(module, actionName, payload) {
        var mod = allActions[module];
        if (!mod || !mod[actionName]) {
            return;
        }
        var fn = mod[actionName];
        /* middleware chain (logging / snapshot / persistence) */
        var i;
        for (i = 0; i < middlewares.length; i++) {
            middlewares[i](module, actionName, payload, state);
        }
        fn(state[module], payload);
    }

    return {
        state: state,
        dispatch: dispatch
    };
}
