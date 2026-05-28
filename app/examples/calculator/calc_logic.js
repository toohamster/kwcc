/* calculator/calc_logic.js — Calculator state and logic */

if (typeof _calcInit == "undefined") {
    var display = "0";
    var prevValue = 0;
    var operator = "";
    var newNumber = 1;
    var _calcInit = 1;

    var _digit = function(n) {
        if (newNumber) { display = "" + n; newNumber = 0; }
        else { display = display + "" + n; }
    };

    var _dot = function() {
        if (newNumber) { display = "0."; newNumber = 0; }
        else if (display.indexOf(".") < 0) { display = display + "."; }
    };

    var _op = function(op) {
        prevValue = parseFloat(display);
        operator = op;
        newNumber = 1;
    };

    var _equals = function() {
        var b;
        if (operator == "") { return; }
        b = parseFloat(display);
        if (operator == "+") { prevValue = prevValue + b; }
        if (operator == "-") { prevValue = prevValue - b; }
        if (operator == "*") { prevValue = prevValue * b; }
        if (operator == "/") { prevValue = b != 0 ? prevValue / b : "Error"; }
        display = "" + prevValue;
        operator = "";
        newNumber = 1;
    };

    var _clear = function() {
        display = "0"; prevValue = 0; operator = ""; newNumber = 1;
    };
}
