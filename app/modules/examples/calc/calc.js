/* examples/calc/calc.js — calculator state, actions, events */

registerTopic("calc", {
    digit_0: "calc/digit/0",
    digit_1: "calc/digit/1",
    digit_2: "calc/digit/2",
    digit_3: "calc/digit/3",
    digit_4: "calc/digit/4",
    digit_5: "calc/digit/5",
    digit_6: "calc/digit/6",
    digit_7: "calc/digit/7",
    digit_8: "calc/digit/8",
    digit_9: "calc/digit/9",
    dot: "calc/dot",
    op_plus: "calc/op/plus",
    op_minus: "calc/op/minus",
    op_mult: "calc/op/mult",
    op_div: "calc/op/div",
    equals: "calc/equals",
    clear: "calc/clear",
    window: "calc/window"
});

registerModule("calc", {
    state: {
        display: "0",
        prevValue: 0,
        operator: "",
        newNumber: 1
    },
    actions: {
        digit: function(s, data) {
            var n = data.value;
            if (s.newNumber) { s.display = "" + n; s.newNumber = 0; }
            else { s.display = s.display + "" + n; }
        },
        dot: function(s) {
            if (s.newNumber) { s.display = "0."; s.newNumber = 0; }
            else if (s.display.indexOf(".") < 0) { s.display = s.display + "."; }
        },
        op: function(s, data) {
            s.prevValue = parseFloat(s.display);
            s.operator = data.value;
            s.newNumber = 1;
        },
        equals: function(s) {
            var b;
            if (s.operator == "") { return; }
            b = parseFloat(s.display);
            if (s.operator == "+") { s.prevValue = s.prevValue + b; }
            if (s.operator == "-") { s.prevValue = s.prevValue - b; }
            if (s.operator == "*") { s.prevValue = s.prevValue * b; }
            if (s.operator == "/") { s.prevValue = b != 0 ? s.prevValue / b : "Error"; }
            s.display = "" + s.prevValue;
            s.operator = "";
            s.newNumber = 1;
        },
        clear: function(s) {
            s.display = "0"; s.prevValue = 0; s.operator = ""; s.newNumber = 1;
        }
    },
    initEvents: function() {
        var t = $topics.calc;
        $bus.on(t.digit_0, function(action, data) { $store.dispatch("calc", "digit", { value: 0 }); });
        $bus.on(t.digit_1, function(action, data) { $store.dispatch("calc", "digit", { value: 1 }); });
        $bus.on(t.digit_2, function(action, data) { $store.dispatch("calc", "digit", { value: 2 }); });
        $bus.on(t.digit_3, function(action, data) { $store.dispatch("calc", "digit", { value: 3 }); });
        $bus.on(t.digit_4, function(action, data) { $store.dispatch("calc", "digit", { value: 4 }); });
        $bus.on(t.digit_5, function(action, data) { $store.dispatch("calc", "digit", { value: 5 }); });
        $bus.on(t.digit_6, function(action, data) { $store.dispatch("calc", "digit", { value: 6 }); });
        $bus.on(t.digit_7, function(action, data) { $store.dispatch("calc", "digit", { value: 7 }); });
        $bus.on(t.digit_8, function(action, data) { $store.dispatch("calc", "digit", { value: 8 }); });
        $bus.on(t.digit_9, function(action, data) { $store.dispatch("calc", "digit", { value: 9 }); });
        $bus.on(t.dot, function(action, data) { $store.dispatch("calc", "dot"); });
        $bus.on(t.op_plus, function(action, data) { $store.dispatch("calc", "op", { value: "+" }); });
        $bus.on(t.op_minus, function(action, data) { $store.dispatch("calc", "op", { value: "-" }); });
        $bus.on(t.op_mult, function(action, data) { $store.dispatch("calc", "op", { value: "*" }); });
        $bus.on(t.op_div, function(action, data) { $store.dispatch("calc", "op", { value: "/" }); });
        $bus.on(t.equals, function(action, data) { $store.dispatch("calc", "equals"); });
        $bus.on(t.clear, function(action, data) { $store.dispatch("calc", "clear"); });
    }
});
