/* runtime/bus.js — EventBus: exact match + * suffix wildcard + onGroup/offGroup */

var $bus = null;

function createBus() {
    /* Each entry: { pattern, group, cb } */
    var listeners = [];

    /* Match: exact or * suffix wildcard
       "calc/btn" matches "calc/btn"
       "calc/*"   matches "calc/btn", "calc/btn/7", etc. */
    function match(pattern, topic) {
        if (pattern === topic) return 1;
        var starEnd = pattern.length - 1;
        if (starEnd > 0 && pattern.charAt(starEnd) === "*") {
            var prefix = pattern.slice(0, starEnd);
            if (topic.slice(0, prefix.length) === prefix) return 1;
        }
        return 0;
    }

    function emit(topic, action, data) {
        var i;
        for (i = 0; i < listeners.length; i++) {
            if (match(listeners[i].pattern, topic)) {
                listeners[i].cb(action, data);
            }
        }
    }

    function on(pattern, cb) {
        listeners.push({ pattern: pattern, group: "", cb: cb });
    }

    function onGroup(group, pattern, cb) {
        listeners.push({ pattern: pattern, group: group, cb: cb });
    }

    function offGroup(group) {
        var i = 0;
        while (i < listeners.length) {
            if (listeners[i].group === group) {
                listeners.splice(i, 1);
            } else {
                i++;
            }
        }
    }

    return {
        emit: emit,
        on: on,
        onGroup: onGroup,
        offGroup: offGroup
    };
}
