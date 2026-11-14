#pragma once

// Facet, interpreted: load a .facet file at runtime and draw its views, with
// no compiler involved. This is how a shell takes plugins written in Facet.
//
//   facet::Plugin plugin = facet::Plugin::load("weather.facet");
//   plugin.set("city", "Lisbon")
//         .set("refresh", [&](facet::Arguments) { weather.refresh(); return facet::Value{}; });
//   app.surface([&](vw::Ui& ui) { plugin.view(ui, "Panel"); });
//
// A plugin sees only what the host gives it: values and functions passed with
// set(), the arguments of view(), and components registered with component()
// (how `S.Button` or `Native(name: Graph)` resolve). It cannot run C++: files
// with cpp { } blocks or include lines need facetc instead.
//
// Built with -DVW_WITH_FACET=ON, as libvw-facet (CMake: vw::facet).

#include <vw/facet.hpp>
#include <vw/ui.hpp>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace facet {

class Value;
using Arguments = std::span<const Value>;
using Function = std::function<Value(Arguments)>;

// A Facet value: null, a number, a boolean, text, a color, a list, an object
// or a function. Lists and objects are shared and never change in place.
class Value {
 public:
  using List = std::vector<Value>;
  using Object = std::map<std::string, Value, std::less<>>;

  Value() = default;
  Value(std::nullptr_t) {}
  Value(double number) : data_(number) {}
  Value(float number) : data_(static_cast<double>(number)) {}
  Value(int number) : data_(static_cast<double>(number)) {}
  Value(long number) : data_(static_cast<double>(number)) {}
  Value(long long number) : data_(static_cast<double>(number)) {}
  Value(unsigned number) : data_(static_cast<double>(number)) {}
  Value(unsigned long number) : data_(static_cast<double>(number)) {}
  Value(unsigned long long number) : data_(static_cast<double>(number)) {}
  Value(bool flag) : data_(flag) {}
  Value(std::string text) : data_(std::move(text)) {}
  Value(std::string_view text) : data_(std::string(text)) {}
  Value(const char* text) : data_(std::string(text)) {}
  Value(vw::Color color) : data_(color) {}
  Value(List list) : data_(std::make_shared<const List>(std::move(list))) {}
  Value(Object object) : data_(std::make_shared<const Object>(std::move(object))) {}
  template <class F>
    requires(!std::is_same_v<std::decay_t<F>, Value> && std::is_invocable_r_v<Value, F, Arguments>)
  Value(F function) : data_(Function(std::move(function))) {}

  bool is_null() const { return std::holds_alternative<std::monostate>(data_); }
  bool is_number() const { return std::holds_alternative<double>(data_); }
  bool is_bool() const { return std::holds_alternative<bool>(data_); }
  bool is_text() const { return std::holds_alternative<std::string>(data_); }
  bool is_color() const { return std::holds_alternative<vw::Color>(data_); }
  bool is_list() const { return std::holds_alternative<std::shared_ptr<const List>>(data_); }
  bool is_object() const { return std::holds_alternative<std::shared_ptr<const Object>>(data_); }
  bool is_function() const { return std::holds_alternative<Function>(data_); }

  // Each throws std::runtime_error naming what the value is instead.
  double number() const;
  bool boolean() const;
  const std::string& text() const;
  const vw::Color& color() const;
  const List& list() const;
  const Object& object() const;
  const Function& function() const;

  // A field of an object; null when it has none.
  const Value& operator[](std::string_view field) const;
  // false, null, 0 and "" are false; everything else is true.
  bool truthy() const;
  // How + and string properties write it: 3, 2.5, true, the text itself.
  std::string to_string() const;
  // "number", "text", "list", …, for error messages.
  std::string_view kind() const;

  Value operator()(Arguments arguments) const { return function()(arguments); }

  bool operator==(const Value& other) const;

 private:
  std::variant<std::monostate, double, bool, std::string, vw::Color, std::shared_ptr<const List>,
               std::shared_ptr<const Object>, Function>
      data_;
};

// A mistake in a plugin, found while loading or while drawing it: where it
// is, and what. what() is "file:line:column: message".
struct ScriptError : std::runtime_error {
  std::string file;
  int line = 0;
  int column = 0;
  std::string message;
  ScriptError(std::string file_name, int line_number, int column_number, std::string text);
};

// A component a plugin can use, as S.Name(…) { … } or Native(name: Name, …):
// its arguments arrive as one object, with handlers as functions. The
// children are built when it calls them (facet::Children, as for compiled
// Facet components), so it can draw its background first.
using Component = std::function<vw::Widget(vw::Ui&, const Value& props, const Children& children)>;

class Plugin {
 public:
  // Parses the file; throws ScriptError for syntax mistakes and for C++ the
  // interpreter cannot run (cpp { } blocks, include).
  static Plugin load(const std::filesystem::path& path);
  // The same from text; `name` is used in error messages.
  static Plugin parse(std::string source, std::string name = "plugin.facet");

  Plugin(Plugin&&) noexcept;
  Plugin& operator=(Plugin&&) noexcept;
  ~Plugin();

  // The views the file declares, in order.
  std::vector<std::string> views() const;
  // A name every view can read: a value, an object of them, or a function.
  Plugin& set(std::string name, Value value);
  // What `S.Button` (from `let [ JES.spectralium as S ]`) or
  // `Native(name: Graph)` draws: registered by module path and name,
  // "JES.spectralium.Button", or the bare name for Native, "Graph".
  Plugin& component(std::string name, Component component);

  // Draws a view. A view that is a Surface draws the surface and returns an
  // empty widget. Throws ScriptError for mistakes found while drawing.
  vw::Widget view(vw::Ui& ui, std::string_view name, Arguments arguments = {}) const;

  // Loads the file again if it changed on disk since it was loaded, keeping
  // what set() and component() registered. True when it reloaded; throws
  // ScriptError (and keeps the old version) when the new one does not parse.
  bool reload();

  // What a handler threw, instead of letting it unwind into vw's event
  // loop. Defaults to printing it on stderr.
  Plugin& on_error(std::function<void(const std::exception&)> report);

 public:
  // Opaque: what a loaded plugin holds.
  struct State;

 private:
  explicit Plugin(std::unique_ptr<State> state);
  std::unique_ptr<State> state_;
};

}  // namespace facet
