/* examples/test/test_view.js — test module view */

registerModuleView("test", function(s) {
    /* C 层挡板处理可见性，view 不需要判断 */
    ui.beginWindow("Test Counter", 50, 50, 260, 160, 0, $topics.test.window);
    ui.layoutRow(30, -1);
    ui.label("Count: " + s.count);
    ui.layoutRow(30, 100, 100);
    ui.button("+", $topics.test.btn);
    ui.button("Reset", $topics.test.reset);
    ui.endWindow();
});
