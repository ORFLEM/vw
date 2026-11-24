---
title: Passing values and functions
description: How your C++ variables and functions reach a Facet view, compiled or loaded as a plugin.
---

A view sees nothing of your program unless you hand it over. How depends on whether the file is compiled by `vw-facetc` or loaded at runtime as a [plugin](/facet/plugins/).

## Compiled Facet

### Through the view's parameters

The usual way. A view's parameters are template parameters, so pass anything: structs, references, lambdas.

```facet
view Bar(state, actions, format) {
    ui.Text(text: state.user + "  " + format(state.cpu))      // a field, and a lambda you passed
    ui.Text(text: state.clock())                               // a member function or std::function
    ui.Rectangle(width: 24, height: 24, input: {
        on_click: () => actions.exec("pavucontrol"),            // runs on click
        on_right_click: actions.mute()                          // the same, written as a call
    })
}
```

```cpp
struct State {
  std::string user;
  int cpu = 0;
  std::function<std::string()> clock;
};
struct Actions {
  void exec(const std::string& command) { vw::spawn({"sh", "-c", command}); }
  void mute();
};

State state;
Actions actions;
auto format = [](int percent) { return std::to_string(percent) + "%"; };
vw::App app([&](vw::Ui& ui) { views::Bar(ui, state, actions, format); });
```

- `state.user`, `state.cpu`, `state.clock()`, `format(state.cpu)` are plain C++: fields, calls, anything clang accepts. A typo is a clang error at the `.facet` line.
- Parameters are references to your objects, so a view reads the current values on every build. Change `state`, and the next build shows it.
- Handlers reach `state` and `actions` as the live objects, not copies; see [Handlers](/facet/expressions/#handlers).
- Group things however suits you: one `state` with nested structs (`state.bar.cava_bars`), several parameters, or one object holding both data and functions.

### Through a header

Free functions, constants and globals from a header work in any expression:

```facet
include "helpers.hpp"

view Clock() {
    ui.Text(text: format_time(now()) + "  " + volume_percent + "%")
}
```

```cpp
// helpers.hpp
inline std::string format_time(std::chrono::system_clock::time_point t);
inline std::chrono::system_clock::time_point now() { return std::chrono::system_clock::now(); }
inline int volume_percent = 40;
```

Short helpers can also live in the file itself, in a [`cpp { }` block](/facet/cpp/#c-blocks).

### From a library

A library is a header plus something to link. `include` its header, call it like any function, and link it in CMake:

```facet
include <libnotify/notify.h>
include "mpris.hpp"

view Player(state) {
    ui.Text(text: mpris::title(state.player))
    ui.Rectangle(width: 24, height: 24, input: {
        on_click: () => notify_notification_show(state.toast, nullptr)
    })
}
```

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(NOTIFY REQUIRED IMPORTED_TARGET libnotify)
target_link_libraries(my-widget PRIVATE vw::vw PkgConfig::NOTIFY)
```

Namespaces are C++: write `mpris::title`, not `mpris.title`. Calls that need setup (`notify_init`), or that are long, belong in your C++ or in a [`cpp { }` block](/facet/cpp/#c-blocks); the view then calls your wrapper. A plugin cannot call a library directly: wrap the call in a function and hand it over with `set()`.

### vw's own functions

`vw::` functions work the same way: `vw::spawn({"pavucontrol"})`, `vw::outputs().size()`. Most are functions, so they need `()`: `vw::current_output().width`, never `vw::current_output::width`. For the monitor being drawn on, `screen` is shorter and is the same thing:

```facet
ui.Text(text: state.vars.get_bat_str(screen.width >= 2560))
```

`screen` is the bar's own monitor with [`App::per_output`](/guides/shells/#one-bar-per-monitor), or the one in `Surface(output: "DP-1")`. Otherwise it is the first monitor.

### From one view to another

A view that is not a surface returns a `vw::Widget`. To place it inside another view, register it as a [native widget or component](/facet/cpp/#native-widgets) and pass what it needs as arguments.

## Plugins

A plugin has no headers and no template parameters. The host hands everything over with `set()`, by name:

```cpp
facet::Plugin plugin = facet::Plugin::load(path);
plugin.set("user", "orflem")
      .set("cpu", 12)
      .set("bar", facet::Value::Object{{"cava_bars", facet::Value::List{3, 8, 14}}})
      .set("exec", [](facet::Arguments arguments) {
        vw::spawn({"sh", "-c", arguments[0].text()});
        return facet::Value{};
      });
```

```facet
view Bar() {
    ui.Text(text: user + "  " + cpu + "%")
    for level in bar.cava_bars { ui.Rectangle(width: 4, height: level) }
    ui.Rectangle(width: 24, height: 24, input: { on_click: exec("pavucontrol") })
}
```

Values are copied into the plugin, so call `set()` again when they change, before the next build. Arguments of `plugin.view(ui, "Bar", arguments)` work too, as the view's parameters. [Plugins](/facet/plugins/#values) lists every kind of value.
