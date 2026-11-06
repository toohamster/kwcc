/* examples/form/form.js — 编辑控件示例（ui.textbox 单行 + 多行） */

registerTopic("form", {
    name: "form/field/name",
    bio: "form/field/bio",
    window: "form/window"
});

registerModule("form", {
    state: {
        visible: 1,
        name: "kwcc",
        bio: "lightweight self-contained desktop UI engine"
    },
    actions: {
        setName: function(s, payload) {
            s.name = payload;
        },
        setBio: function(s, payload) {
            s.bio = payload;
        },
        closeWindow: function(s) {
            s.visible = 0;
        }
    },
    initEvents: function() {
        $bus.on($topics.form.name, function(action) {
            if (action === "change") {
                $store.dispatch("form", "setName", ui.textboxValue("name"));
            }
        });
        $bus.on($topics.form.bio, function(action) {
            if (action === "change") {
                $store.dispatch("form", "setBio", ui.textboxValue("bio"));
            }
        });
        $bus.on($topics.form.window, function() {
            $store.dispatch("form", "closeWindow");
        });
    }
});
