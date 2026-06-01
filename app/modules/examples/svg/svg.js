/* examples/svg/svg.js — SVG example module (view-only, no actions) */

registerTopic("svg", {
    window_red: "svg/window/red",
    window_blue: "svg/window/blue"
});

registerModule("svg", {
    state: { visible: 1 },
    actions: {
        closeWindow: function(s) { s.visible = 0; }
    },
    initEvents: function() {
        $bus.on($topics.svg.window_red, function(action, data) {
            $store.dispatch("svg", "closeWindow");
        });
        $bus.on($topics.svg.window_blue, function(action, data) {
            $store.dispatch("svg", "closeWindow");
        });
    }
});
