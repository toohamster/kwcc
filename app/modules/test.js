/* modules/test.js — minimal test module for store + bus validation */

registerModule("test", {
    state: {
        count: 0
    },
    actions: {
        increment: function(s) {
            s.count = s.count + 1;
        },
        reset: function(s) {
            s.count = 0;
        }
    },
    initEvents: function() {
        $bus.onGroup("test", TOPIC.TEST_BTN, function(action, data) {
            $store.dispatch("test", "increment");
        });
    },
    cleanup: function() {
        $bus.offGroup("test");
    }
});
