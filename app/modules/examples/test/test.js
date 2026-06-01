/* examples/test/test.js — test module logic for store + bus validation */

registerTopic("test", {
    btn: "test/btn/inc",
    reset: "test/btn/reset",
    window: "test/window"
});

registerModule("test", {
    state: {
        count: 0,
        visible: 1
    },
    actions: {
        increment: function(s) {
            s.count = s.count + 1;
        },
        reset: function(s) {
            s.count = 0;
        },
        closeWindow: function(s) {
            s.visible = 0;
        }
    },
    initEvents: function() {
        $bus.onGroup("test", $topics.test.btn, function(action, data) {
            $store.dispatch("test", "increment");
        });
        $bus.onGroup("test", $topics.test.reset, function(action, data) {
            $store.dispatch("test", "reset");
        });
        $bus.on($topics.test.window, function(action, data) {
            $store.dispatch("test", "closeWindow");
        });
    },
    cleanup: function() {
        $bus.offGroup("test");
    }
});
