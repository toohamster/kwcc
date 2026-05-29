/* modules/test_view.js — test module view */

registerModuleView("test", function(s) {
    ui.beginWindow("Test Counter", 50, 50, 260, 160, 0);
    ui.layoutRow(30, -1);
    ui.label("Count: " + s.count);
    ui.layoutRow(30, 100, 100);
    ui.button("+", TOPIC.TEST_BTN);
    ui.button("Reset", TOPIC.TEST_RESET);
    ui.endWindow();
});
