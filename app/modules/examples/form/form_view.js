/* examples/form/form_view.js — 表单示例视图：单行 + 多行编辑字段 */

registerModuleView("form", function(s) {
    ui.beginWindow("Form Demo", 500, 120, 340, 250, 0, $topics.form.window);
    ui.layoutRow(26, -1);
    ui.label("Name:");
    ui.layoutRow(30, -1);
    ui.textbox("name", s.name, $topics.form.name, 0);
    ui.layoutRow(26, -1);
    ui.label("Bio:");
    ui.layoutRow(70, -1);
    ui.textbox("bio", s.bio, $topics.form.bio, 1);
    ui.layoutRow(26, -1);
    ui.label("store: name=" + s.name);
    ui.endWindow();
});
