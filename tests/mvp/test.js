/* mvp test — verify kwcc_ui C function is callable from JS */

print("=== mvp test started ===");

/* test 1: call kwcc_ui — if this works, C function is registered */
var r1 = kwcc_ui("button", "Hello");
print("kwcc_ui('button','Hello') returned:", r1);

/* test 2: call with more args */
var r2 = kwcc_ui("slider", "volume", 0.5, 0, 100);
print("kwcc_ui('slider',...) returned:", r2);

/* test 3: basic JS still works */
var x = 1 + 2 * 3;
print("math works:", "" + x);  /* convert to string for print stub */

var arr = [1, 2, 3];
arr.push(4);
print("array:", arr.join(","));

/* test 4: verify kwcc_ui is a function */
print("typeof kwcc_ui:", typeof kwcc_ui);

print("=== mvp test passed ===");
