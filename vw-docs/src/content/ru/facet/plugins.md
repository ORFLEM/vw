---
title: Плагины
description: Загружайте файлы .facet во время работы через facet::Plugin, чтобы оболочку можно было расширять без компилятора.
---

Обычно Facet компилируется заранее. Оболочка, принимающая плагины, не может требовать от каждого пользователя компилятор, поэтому `facet::Plugin` читает файл `.facet` во время работы и рисует его view напрямую, через те же помощники, что и скомпилированный Facet. View выглядит и ведёт себя одинаково в обоих случаях.

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

Линкуйте `vw::facet` (libvw-facet, собирается с `-DVW_WITH_FACET=ON`).

## Что доступно плагину

Только то, что даёт хост:

| От хоста | В плагине |
| --- | --- |
| `plugin.set("name", value)` | `name`, во всех view |
| `plugin.view(ui, "View", arguments)` | параметры view |
| `plugin.component("Graph", draw)` | `Native(name: Graph, …)` |
| `plugin.component("JES.spectralium.Button", draw)` | `S.Button(…)` после `let [ JES.spectralium as S ]` |

Плагин не может выполнять C++: файл с блоками `cpp { }`, телами обработчиков `cpp { }` или строками `include` отклоняется при загрузке, с указанием строки. Всё остальное в языке работает: элементы, контролы, раскладки с `flex` и размерами `"N%"`, `for`, `if`, `let`, имена `id:` (в том числе чтение наперёд), блоки `properties`, `animate`, `screen` и обработчики.

## Значения

`facet::Value` — то, что переходит между хостом и плагином: null, число, логическое значение, текст, `vw::Color`, список, объект или функция.

```cpp
plugin.set("battery", facet::Value::Object{{"percent", 76}, {"charging", true}});
plugin.set("apps", facet::Value::List{"firefox", "foot", "zwwm"});
plugin.set("launch", [&](facet::Arguments arguments) {
  launcher.run(arguments[0].text());
  return facet::Value{};
});
```

Плагин читает их как `battery.percent`, `apps.length`, `for app in apps { … }` и `on_click: launch(app)`. Числа — double, так что `1 / 2` равно `0.5` (скомпилированный Facet следует C++ и даёт `0`). Текст соединяется через `+`. Списки и объекты не меняются на месте: давайте плагину новое значение, когда меняется ваше состояние, до следующей сборки.

Обработчики получают событие одним объектом:

| Обработчик | Аргумент |
| --- | --- |
| `on_click` | `{ x, y, button }` |
| `on_scroll`, `on_scroll_x` | `{ x, y, delta }` |
| `on_drag_start`, `on_drag`, `on_drag_end` | `{ x, y, start_x, start_y, dx, dy, delta_x, delta_y, button, cancelled }` |
| `on_touch` | `{ phase, id, x, y, start_x, start_y, fingers }`, где `phase` — `"down"`, `"motion"`, `"up"` или `"cancel"` |
| `on_hover` | `true` или `false` |
| TextField `on_change`, `on_submit`; Dropdown `on_change` | текст |
| Toggle `on_change` | новое состояние |
| Slider `on_change` | значение |

## Компоненты

Компонент, зарегистрированный хостом, получает аргументы плагина одним объектом — обработчики как функции, текст `#rrggbb` как цвет — и своих детей, которые строятся, когда он их просит:

```cpp
plugin.component("Graph", [](vw::Ui& ui, const facet::Value& props, const facet::Children& children) {
  vw::RectangleProps back;
  back.width = static_cast<float>(props["width"].number());
  back.height = 80;
  back.color = props["color"].color();
  return facet::on(ui, ui.rectangle(back), children(ui));
});
```

## Ошибки

`Plugin::load`, `Plugin::parse` и `plugin.view` бросают `facet::ScriptError`, у которого `what()` — `file:line:column: message`; `line`, `column` и `message` тоже есть как поля. Ловите его вокруг `view`, и остальная оболочка продолжит рисоваться. Обработчики выполняются позже, из цикла событий vw, поэтому то, что они бросают, уходит в `plugin.on_error(…)` (по умолчанию в stderr).

## Правка плагина на лету

`plugin.reload()` перечитывает файл, если он изменился после загрузки, сохраняя то, что зарегистрировано через `set` и `component`. Вызывайте его в начале сборки — и сохранённый плагин появится в следующем кадре. Версия, которая не разбирается, бросает ошибку, а старая остаётся.
