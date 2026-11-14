---
title: Plugins
description: Load .facet files at runtime with facet::Plugin, so people can extend a shell without a compiler.
---

Facet usually compiles ahead of time. A shell that takes plugins cannot ask every user for a compiler, so `facet::Plugin` reads a `.facet` file at runtime and draws its views directly, through the same helpers compiled Facet uses. A view looks and behaves the same either way.

```cpp
#include <vw/facet_runtime.hpp>

facet::Plugin plugin = facet::Plugin::load("~/.config/myshell/plugins/weather.facet");
plugin.set("city", "Lisbon")
      .set("temperature", weather.celsius())
      .set("refresh", [&](facet::Arguments) { weather.refresh(); return facet::Value{}; });

app.surface([&](vw::Ui& ui) { plugin.view(ui, "Panel"); });
```

```facet
let [ vw.ui as ui, vw.layout as layout ] in

view Panel() {
    layout.Row(gap: 12, padding: 16, bg: "#1b2732", radius: 20) {
        ui.Text(city + "  " + temperature + "°", size: 18, color: "#e8eef4")
        ui.Rectangle(width: 32, height: 32, radius: 16, color: "#3a4a58",
                     input: { on_click: refresh() })
    }
}
```

Link `vw::facet` (libvw-facet, built with `-DVW_WITH_FACET=ON`).

## What a plugin can reach

Only what the host gives it:

| From the host | In the plugin |
| --- | --- |
| `plugin.set("name", value)` | `name`, in every view |
| `plugin.view(ui, "View", arguments)` | the view's parameters |
| `plugin.component("Graph", draw)` | `Native(name: Graph, …)` |
| `plugin.component("JES.spectralium.Button", draw)` | `S.Button(…)` after `let [ JES.spectralium as S ]` |

A plugin cannot run C++: a file with `cpp { }` blocks, `cpp { }` handler bodies or `include` lines is refused when it loads, with the line it stopped at. Everything else in the language works: elements, controls, layouts with `flex` and `"N%"` sizes, `for`, `if`, `let`, `id:` names (also read ahead), `properties` blocks, `animate`, `screen`, and handlers.

## Values

`facet::Value` is what crosses between the host and a plugin: null, a number, a boolean, text, a `vw::Color`, a list, an object or a function.

```cpp
plugin.set("battery", facet::Value::Object{{"percent", 76}, {"charging", true}});
plugin.set("apps", facet::Value::List{"firefox", "foot", "zwwm"});
plugin.set("launch", [&](facet::Arguments arguments) {
  launcher.run(arguments[0].text());
  return facet::Value{};
});
```

The plugin reads them as `battery.percent`, `apps.length`, `for app in apps { … }` and `on_click: launch(app)`. Numbers are doubles, so `1 / 2` is `0.5` (compiled Facet follows C++ and gives `0`). Text joins with `+`. Lists and objects never change in place: give the plugin a fresh value when your state changes, before the next build.

Handlers get their event as one object:

| Handler | Argument |
| --- | --- |
| `on_click` | `{ x, y, button }` |
| `on_scroll`, `on_scroll_x` | `{ x, y, delta }` |
| `on_drag_start`, `on_drag`, `on_drag_end` | `{ x, y, start_x, start_y, dx, dy, delta_x, delta_y, button, cancelled }` |
| `on_touch` | `{ phase, id, x, y, start_x, start_y, fingers }`, `phase` being `"down"`, `"motion"`, `"up"` or `"cancel"` |
| `on_hover` | `true` or `false` |
| TextField `on_change`, `on_submit`; Dropdown `on_change` | the text |
| Toggle `on_change` | the new state |
| Slider `on_change` | the value |

## Components

A component the host registers receives the plugin's arguments as one object, with handlers as functions and `#rrggbb` text as a color, plus its children, built when it asks for them:

```cpp
plugin.component("Graph", [](vw::Ui& ui, const facet::Value& props, const facet::Children& children) {
  vw::RectangleProps back;
  back.width = static_cast<float>(props["width"].number());
  back.height = 80;
  back.color = props["color"].color();
  return facet::on(ui, ui.rectangle(back), children(ui));
});
```

## Mistakes

`Plugin::load`, `Plugin::parse` and `plugin.view` throw `facet::ScriptError`, whose `what()` is `file:line:column: message`; `line`, `column` and `message` are fields too. Catch it around `view` and the rest of your shell keeps drawing. Handlers run later, from vw's event loop, so what they throw goes to `plugin.on_error(…)` instead (stderr by default).

## Editing a plugin live

`plugin.reload()` reads the file again if it changed since it was loaded, keeping what `set` and `component` registered. Call it at the start of the build and a saved plugin shows on the next frame. A version that does not parse throws and the old one stays.
