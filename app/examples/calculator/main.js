/* calculator/main.js — kwcc calculator example */

/* Load calculator logic (state persists across frames) */
load("app/examples/calculator/calc_logic.js");

/* PingFang SC loaded as default font in main.m — no extra load needed */

/* ── window 280x350, MU_OPT_NOCLOSE=64 ── */
ui.beginWindow("计算器", 200, 80, 280, 350, 64);

/* ── display: full-width (-1 = fill), right-aligned text ── */
ui.layoutRow(40, -1);
ui.display(display);

/* ── test label with Chinese text ── */
ui.layoutRow(16);
ui.label("中文测试");

/* ── row 1: 7 8 9 + ── */
ui.layoutRow(30, 55, 55, 55, 55);
if (ui.button("7")) { _digit(7); }
if (ui.button("8")) { _digit(8); }
if (ui.button("9")) { _digit(9); }
if (ui.button("+")) { _op("+"); }

/* ── row 2: 4 5 6 - ── */
ui.layoutRow(30, 55, 55, 55, 55);
if (ui.button("4")) { _digit(4); }
if (ui.button("5")) { _digit(5); }
if (ui.button("6")) { _digit(6); }
if (ui.button("-")) { _op("-"); }

/* ── row 3: 1 2 3 x ── */
ui.layoutRow(30, 55, 55, 55, 55);
if (ui.button("1")) { _digit(1); }
if (ui.button("2")) { _digit(2); }
if (ui.button("3")) { _digit(3); }
if (ui.button("x")) { _op("*"); }

/* ── row 4: 0 . C / ── */
ui.layoutRow(30, 55, 55, 55, 55);
if (ui.button("0")) { _digit(0); }
if (ui.button(".")) { _dot(); }
if (ui.button("C")) { _clear(); }
if (ui.button("/")) { _op("/"); }

/* ── row 5: = (full-width, resets 4-column state) ── */
ui.layoutRow(30, -1);
if (ui.button("=")) { _equals(); }

ui.endWindow();
