---
title: Передача значений и функций
description: Как ваши переменные и функции из C++ попадают во view на Facet — скомпилированный или загруженный как плагин.
---

View не видит ничего из вашей программы, пока вы это не передадите. Как именно — зависит от того, компилирует ли файл `vw-facetc` или он загружается во время работы как [плагин](/ru/facet/plugins/).

## Скомпилированный Facet

### Через параметры view

Основной способ. Параметры view — это параметры шаблона, так что передавать можно что угодно: структуры, ссылки, лямбды.

```facet
view Bar(state, actions, format) {
    ui.Text(text: state.user + "  " + format(state.cpu))      // поле и переданная лямбда
    ui.Text(text: state.clock())                               // метод или std::function
    ui.Rectangle(width: 24, height: 24, input: {
        on_click: () => actions.exec("pavucontrol"),            // выполнится по клику
        on_right_click: actions.mute()                          // то же самое, записанное вызовом
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

- `state.user`, `state.cpu`, `state.clock()`, `format(state.cpu)` — обычный C++: поля, вызовы, всё, что примет clang. Опечатка — ошибка clang на строке `.facet`.
- Параметры — ссылки на ваши объекты, поэтому view читает текущие значения при каждой сборке. Поменяли `state` — следующая сборка это покажет.
- Обработчики видят `state` и `actions` как живые объекты, а не копии; см. [Обработчики](/ru/facet/expressions/#обработчики).
- Группируйте как удобно: один `state` с вложенными структурами (`state.bar.cava_bars`), несколько параметров или один объект и с данными, и с функциями.

### Через заголовок

Свободные функции, константы и глобальные переменные из заголовка работают в любом выражении:

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

Короткие помощники можно держать и в самом файле, в [блоке `cpp { }`](/ru/facet/cpp/#блоки-c).

### Из библиотеки

Библиотека — это заголовок плюс то, что нужно слинковать. Подключите заголовок через `include`, вызывайте как обычную функцию и слинкуйте в CMake:

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

Пространства имён — как в C++: `mpris::title`, а не `mpris.title`. Вызовы, которым нужна подготовка (`notify_init`), или длинные — в ваш C++ или в [блок `cpp { }`](/ru/facet/cpp/#блоки-c), а view вызывает вашу обёртку. Плагин не может вызвать библиотеку напрямую: оберните вызов в функцию и передайте её через `set()`.

### Функции vw

Функции `vw::` работают так же: `vw::spawn({"pavucontrol"})`, `vw::outputs().size()`. Большинство из них — функции, им нужны `()`: `vw::current_output().width`, а не `vw::current_output::width`. Для монитора, на котором рисуется view, короче `screen` — это то же самое:

```facet
ui.Text(text: state.vars.get_bat_str(screen.width >= 2560))
```

`screen` — свой монитор панели при [`App::per_output`](/ru/guides/shells/#панель-на-каждый-монитор) или тот, что указан в `Surface(output: "DP-1")`. Иначе — первый монитор.

### Из одного view в другой

View, который не является поверхностью, возвращает `vw::Widget`. Чтобы вставить его в другой view, зарегистрируйте его как [нативный виджет или компонент](/ru/facet/cpp/#нативные-виджеты) и передайте нужное аргументами.

## Плагины

У плагина нет заголовков и параметров шаблона. Хост передаёт всё через `set()`, по имени:

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

Значения копируются в плагин, поэтому вызывайте `set()` снова, когда они меняются, до следующей сборки. Аргументы `plugin.view(ui, "Bar", arguments)` тоже работают — как параметры view. Все виды значений перечислены в [Плагинах](/ru/facet/plugins/#значения).
