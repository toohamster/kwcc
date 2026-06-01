/* examples/calc/calc_view.js — calculator module view */

registerModuleView("calc", function(s) {
    ui.beginWindow("计算器", 200, 80, 280, 350, 64, $topics.calc.window);

    /* display: full-width */
    ui.layoutRow(40, -1);
    ui.display(s.display);

    /* row 1: 7 8 9 + */
    ui.layoutRow(30, 55, 55, 55, 55);
    ui.button("7", $topics.calc.digit_7);
    ui.button("8", $topics.calc.digit_8);
    ui.button("9", $topics.calc.digit_9);
    ui.button("+", $topics.calc.op_plus);

    /* row 2: 4 5 6 - */
    ui.layoutRow(30, 55, 55, 55, 55);
    ui.button("4", $topics.calc.digit_4);
    ui.button("5", $topics.calc.digit_5);
    ui.button("6", $topics.calc.digit_6);
    ui.button("-", $topics.calc.op_minus);

    /* row 3: 1 2 3 x */
    ui.layoutRow(30, 55, 55, 55, 55);
    ui.button("1", $topics.calc.digit_1);
    ui.button("2", $topics.calc.digit_2);
    ui.button("3", $topics.calc.digit_3);
    ui.button("x", $topics.calc.op_mult);

    /* row 4: 0 . C / */
    ui.layoutRow(30, 55, 55, 55, 55);
    ui.button("0", $topics.calc.digit_0);
    ui.button(".", $topics.calc.dot);
    ui.button("C", $topics.calc.clear);
    ui.button("/", $topics.calc.op_div);

    /* row 5: = (full-width) */
    ui.layoutRow(30, -1);
    ui.button("=", $topics.calc.equals);

    ui.endWindow();
});
