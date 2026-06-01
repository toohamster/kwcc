/* examples/svg/svg_view.js — SVG rendering view */

registerModuleView("svg", function(s) {
    ui.beginWindow("Inline: Red Star", 100, 100, 260, 260, 0, $topics.svg.window_red);
    ui.layoutRow(200, -1);
    ui.svg('<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200"><polygon points="100,10 40,198 190,78 10,78 160,198" fill="#e74c3c" stroke="#c0392b" stroke-width="3"/></svg>', 20, 20, 220, 200);
    ui.endWindow();

    ui.beginWindow("File: Blue Star", 400, 100, 260, 260, 0, $topics.svg.window_blue);
    ui.layoutRow(200, -1);
    ui.svg("app/modules/examples/svg/star.svg", 20, 20, 220, 200);
    ui.endWindow();
});
