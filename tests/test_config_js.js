/* tests/test_config_js.js — Phase 6 integration tests for $config */

var test_pass = 0;
var test_fail = 0;

function assert(cond, name) {
    if (cond) {
        test_pass = test_pass + 1;
        print("  PASS: " + name);
    } else {
        test_fail = test_fail + 1;
        print("  FAIL: " + name);
    }
}

/* Test 1: setMaxPools */
$config.setMaxPools("l5", 4);
$config.setMaxPools("*", 4);
assert(true, "setMaxPools no crash");

/* Test 2: appSetInt + appGet */
$config.appSetInt("test/num", 42);
var num = $config.appGet("test/num", "0");
assert(num === "42", "appSetInt/appGet (42) => " + num);

/* Test 3: appSetString + appGet */
$config.appSetString("test/name", "myapp");
var name = $config.appGet("test/name", "default");
assert(name === "myapp", "appSetString/appGet => " + name);

/* Test 4: appGet with default */
var def = $config.appGet("test/nonexistent", "fallback");
assert(def === "fallback", "appGet default => " + def);

/* Test 5: appSetBool */
$config.appSetBool("test/enabled", true);
var enabled = $config.appGet("test/enabled", "0");
assert(enabled === "1", "appSetBool(true) => " + enabled);

$config.appSetBool("test/disabled", false);
var disabled = $config.appGet("test/disabled", "1");
assert(disabled === "0", "appSetBool(false) => " + disabled);

/* Test 6: appSetJson + appGet (returns parsed object) */
$config.appSetJson("test/json", { key: "value" });
var json_obj = $config.appGet("test/json", "");
assert(json_obj !== null && json_obj.key === "value", "appSetJson/appGet returns parsed object (key=" + json_obj.key + ")");

/* Test 6b: appSetJsonString + appGet (returns string, JS-side parse optional) */
$config.appSetJsonString("test/json_str", '{"a":"1"}');
var json_str = $config.appGet("test/json_str", "");
var json_obj2 = JSON.parse(json_str);
assert(json_obj2.a === "1", "appSetJsonString/appGet returns string, JS parse works (a=" + json_obj2.a + ")");

/* Test 7: appSetTlv + appGetTlv with path */
$config.appSetTlv("test/tlv", { timeout: "30", enabled: "true" });
var tlv_val = $config.appGetTlv("test/tlv", "timeout");
assert(tlv_val === "30", "appSetTlv + getTlvPath(timeout) => " + tlv_val);

/* Test 8: appGetTlv without path (returns JSON) */
var tlv_json = $config.appGetTlv("test/tlv");
assert(tlv_json !== "", "appGetTlv (no path) returns JSON");

/* Test 9: appRelease */
$config.appRelease("test/num");
/* release 只是 ref_count--，数据要等 GC 才清除，不验证读取 */
assert(true, "appRelease no crash");

/* Test 10: coreSetTlv */
$config.coreSetTlv("test/core", { max_fds: "16" });
/* Core 域只有 set，JS 侧无 getter，跳过验证 */
assert(true, "coreSetTlv no crash");

/* Summary */
print("=== $config Integration Tests: " + test_pass + " passed, " + test_fail + " failed ===");
