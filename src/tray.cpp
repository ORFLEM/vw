#include "vw/tray.hpp"

#include "dbus.hpp"

#include <cairo.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <unistd.h>

namespace vw::tray {
namespace {

namespace fs = std::filesystem;
constexpr const char* watcher_name = "org.kde.StatusNotifierWatcher";
constexpr const char* watcher_path = "/StatusNotifierWatcher";
constexpr const char* item_interface = "org.kde.StatusNotifierItem";
constexpr const char* menu_interface = "com.canonical.dbusmenu";
constexpr const char* watcher_xml = R"XML(
  <method name="RegisterStatusNotifierItem"><arg type="s" direction="in"/></method>
  <method name="RegisterStatusNotifierHost"><arg type="s" direction="in"/></method>
  <property name="RegisteredStatusNotifierItems" type="as" access="read"/>
  <property name="IsStatusNotifierHostRegistered" type="b" access="read"/>
  <property name="ProtocolVersion" type="i" access="read"/>
  <signal name="StatusNotifierItemRegistered"><arg type="s"/></signal>
  <signal name="StatusNotifierItemUnregistered"><arg type="s"/></signal>
  <signal name="StatusNotifierHostRegistered"/>
  <signal name="StatusNotifierHostUnregistered"/>
)XML";

using dbus::Match;
using dbus::Message;
using dbus::Value;

struct Entry {
  std::string bus;
  std::string path;
  Item item;
  std::string menu_path;
  std::optional<std::vector<MenuEntry>> menu;
  // Monitor-thread bookkeeping.
  std::uint64_t name_watch = 0;
  std::uint64_t item_signals = 0;
  std::uint64_t menu_signals = 0;
};

// "bus/path", "/path" (bus is the sender) or a bare bus name.
std::pair<std::string, std::string> parse_service(const std::string& service,
                                                  const std::string& sender) {
  if (service.starts_with('/')) return {sender, service};
  const auto slash = service.find('/');
  if (slash == std::string::npos) return {service, "/StatusNotifierItem"};
  return {service.substr(0, slash), service.substr(slash)};
}

std::string strip_mnemonics(const std::string& label) {
  std::string result;
  for (std::size_t index = 0; index < label.size(); ++index) {
    if (label[index] == '_') {
      if (index + 1 < label.size() && label[index + 1] == '_') result += '_', ++index;
      continue;
    }
    result += label[index];
  }
  return result;
}

fs::path cache_directory() {
  const char* runtime = std::getenv("XDG_RUNTIME_DIR");
  return fs::path(runtime != nullptr && *runtime != '\0' ? runtime : "/tmp") / "vw-tray";
}

// Writes the largest ARGB32 pixmap in an a(iiay) as a PNG named by its
// content; returns the path, or empty when there is none.
std::string pixmap_file(const Value* pixmaps) {
  if (pixmaps == nullptr || !pixmaps->is("a(iiay)")) return {};
  const Value* best = nullptr;
  std::int32_t best_width = 0;
  std::int32_t best_height = 0;
  for (const Value& pixmap : pixmaps->items) {
    const auto width = static_cast<std::int32_t>(pixmap.items[0].bits);
    const auto height = static_cast<std::int32_t>(pixmap.items[1].bits);
    if (width > 0 && height > 0 && static_cast<std::int64_t>(width) * height > static_cast<std::int64_t>(best_width) * best_height) {
      best = &pixmap.items[2];
      best_width = width;
      best_height = height;
    }
  }
  if (best == nullptr) return {};
  const std::size_t size = best->text.size();
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(best->text.data());
  if (size < static_cast<std::size_t>(best_width) * static_cast<std::size_t>(best_height) * 4) return {};

  std::uint64_t hash = 1469598103934665603ULL;  // FNV-1a
  for (std::size_t index = 0; index < size; ++index) hash = (hash ^ bytes[index]) * 1099511628211ULL;
  const fs::path directory = cache_directory();
  const fs::path file = directory / (std::to_string(best_width) + "x" + std::to_string(best_height) +
                                     "-" + std::to_string(hash) + ".png");
  std::error_code error;
  if (fs::exists(file, error)) return file.string();
  fs::create_directories(directory, error);

  // Network byte order, straight alpha → native-endian premultiplied.
  cairo_surface_t* surface =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, best_width, best_height);
  auto* pixels = cairo_image_surface_get_data(surface);
  const int stride = cairo_image_surface_get_stride(surface);
  for (std::int32_t y = 0; y < best_height; ++y) {
    auto* row = reinterpret_cast<std::uint32_t*>(pixels + static_cast<std::ptrdiff_t>(y) * stride);
    for (std::int32_t x = 0; x < best_width; ++x) {
      const std::uint8_t* source =
          bytes + (static_cast<std::size_t>(y) * static_cast<std::size_t>(best_width) + static_cast<std::size_t>(x)) * 4;
      const std::uint32_t alpha = source[0];
      const auto premultiply = [alpha](std::uint32_t channel) { return channel * alpha / 255; };
      row[x] = alpha << 24 | premultiply(source[1]) << 16 | premultiply(source[2]) << 8 |
               premultiply(source[3]);
    }
  }
  cairo_surface_mark_dirty(surface);
  const fs::path temporary = file.string() + ".tmp" + std::to_string(getpid());
  const bool written = cairo_surface_write_to_png(surface, temporary.c_str()) == CAIRO_STATUS_SUCCESS;
  cairo_surface_destroy(surface);
  if (!written) return {};
  fs::rename(temporary, file, error);
  return error ? std::string() : file.string();
}

// An icon name looked up in an item's private theme directory.
std::string theme_file(const std::string& directory, const std::string& name) {
  if (directory.empty() || name.empty()) return {};
  std::error_code error;
  std::string fallback;
  for (auto it = fs::recursive_directory_iterator(directory, error);
       !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
    const fs::path& file = it->path();
    if (file.stem() != name) continue;
    const auto extension = file.extension();
    if (extension == ".svg") return file.string();
    if (extension == ".png" && fallback.empty()) fallback = file.string();
  }
  return fallback;
}

MenuEntry parse_menu(const Value& node) {
  MenuEntry entry;
  if (!node.is("(ia{sv}av)")) return entry;
  const Value* properties = &node.items[1];
  entry.id = static_cast<std::int32_t>(node.items[0].bits);
  entry.label = strip_mnemonics(dbus::get_string(properties, "label"));
  entry.icon = dbus::get_string(properties, "icon-name");
  entry.enabled = dbus::get_bool(properties, "enabled", true);
  entry.visible = dbus::get_bool(properties, "visible", true);
  entry.separator = dbus::get_string(properties, "type") == "separator";
  const std::string toggle = dbus::get_string(properties, "toggle-type");
  entry.toggle = toggle == "checkmark" ? MenuEntry::Toggle::checkmark
                 : toggle == "radio"   ? MenuEntry::Toggle::radio
                                       : MenuEntry::Toggle::none;
  entry.checked = dbus::get_int(properties, "toggle-state") == 1;
  for (const Value& child : node.items[2].items)
    if (const Value& inner = child.unwrap(); inner.is("(ia{sv}av)")) entry.children.push_back(parse_menu(inner));
  return entry;
}

class Tray {
 public:
  Tray()
      : monitor_(dbus::Bus::session, [](dbus::Connection*) {},
                 [this](dbus::Connection* bus) { setup(bus); }) {}

  dbus::Monitor& monitor() {
    monitor_.start();
    return monitor_;
  }
  bool available() {
    monitor_.start();
    std::lock_guard lock(mutex_);
    return watcher_present_;
  }
  std::vector<Item> items() {
    monitor_.start();
    std::lock_guard lock(mutex_);
    std::vector<Item> result;
    for (const auto& key : order_) result.push_back(entries_.at(key).item);
    return result;
  }
  // Bus name and path of an item, or nullopt.
  std::optional<std::pair<std::string, std::string>> address(const std::string& id) {
    monitor_.start();
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(id);
    if (found == entries_.end()) return std::nullopt;
    return std::pair{found->second.bus, found->second.path};
  }

  void invalidate_menu(const std::string& id) {
    {
      std::lock_guard lock(mutex_);
      const auto found = entries_.find(id);
      if (found != entries_.end()) found->second.menu.reset();
    }
    monitor_.wake();
  }

  // Bus name and menu object of an item, or nullopt without a menu.
  std::optional<std::pair<std::string, std::string>> menu_address(const std::string& id) {
    monitor_.start();
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(id);
    if (found == entries_.end() || found->second.menu_path.empty()) return std::nullopt;
    return std::pair{found->second.bus, found->second.menu_path};
  }

  std::vector<MenuEntry> menu(const std::string& id) {
    std::string bus;
    std::string path;
    {
      monitor_.start();
      std::lock_guard lock(mutex_);
      const auto found = entries_.find(id);
      if (found == entries_.end() || found->second.menu_path.empty()) return {};
      if (found->second.menu) return *found->second.menu;
      bus = found->second.bus;
      path = found->second.menu_path;
    }
    dbus::Connection* connection = monitor_.connection();
    if (connection == nullptr) return {};
    (void)connection->call(Message::method_call(bus, path, menu_interface, "AboutToShow", {Value::int32(0)}), 500);
    const auto reply = connection->call(
        Message::method_call(bus, path, menu_interface, "GetLayout",
                             {Value::int32(0), Value::int32(-1), Value::array("s")}),
        1000);
    std::vector<MenuEntry> entries;
    if (reply && reply->body.size() == 2) entries = parse_menu(reply->body[1]).children;
    {
      std::lock_guard lock(mutex_);
      const auto found = entries_.find(id);
      if (found != entries_.end()) found->second.menu = entries;
    }
    // Invalidate the cache when the menu changes.
    monitor_.invoke([this, id, bus, path] {
      auto found = entries_.find(id);
      if (found == entries_.end() || found->second.menu_signals != 0) return;
      found->second.menu_signals = monitor_.connection()->subscribe(
          {bus, menu_interface, {}, path, {}, {}}, [this, id](const Message&) { invalidate_menu(id); });
    });
    return entries;
  }

 private:
  void setup(dbus::Connection* bus) {
    unique_name_ = bus->unique_name();
    bus->export_object(watcher_path, {
        watcher_name, watcher_xml,
        [this](const Message& call) -> dbus::Reply {
          const Value* argument = call.arg(0);
          if (argument == nullptr || !argument->is("s"))
            return dbus::Reply::error("org.freedesktop.DBus.Error.InvalidArgs", "expected a string");
          if (call.member == "RegisterStatusNotifierItem") {
            const auto [bus_name, path] = parse_service(argument->text, call.sender);
            add_item(bus_name, path);
          } else if (call.member == "RegisterStatusNotifierHost") {
            emit("StatusNotifierHostRegistered", {});
          } else {
            return dbus::Reply::error("org.freedesktop.DBus.Error.UnknownMethod", "unknown method");
          }
          return {};
        },
        [this] {
          std::vector<std::string> keys;
          {
            std::lock_guard lock(mutex_);
            keys = order_;
          }
          return std::vector<std::pair<std::string, Value>>{
              {"RegisteredStatusNotifierItems", Value::strings(keys)},
              {"IsStatusNotifierHostRegistered", Value::boolean(true)},
              {"ProtocolVersion", Value::int32(0)}};
        }});

    // Own the watcher name when free (queued otherwise, so vw takes over when
    // another bar exits), and follow whoever owns it.
    const Match acquired{"org.freedesktop.DBus", "org.freedesktop.DBus", "NameAcquired", {}, watcher_name, {}};
    bus->subscribe(acquired, [this](const Message&) {
      owns_watcher_ = true;
      emit("StatusNotifierHostRegistered", {});
    });
    const Match lost{"org.freedesktop.DBus", "org.freedesktop.DBus", "NameLost", {}, watcher_name, {}};
    bus->subscribe(lost, [this](const Message&) { owns_watcher_ = false; });
    if (bus->request_name(watcher_name, 0) == 1) owns_watcher_ = true;
    const std::string host = "org.kde.StatusNotifierHost-" + std::to_string(getpid());
    bus->request_name(host, 0);
    bus->watch_name(watcher_name, [this, bus](const std::string& owner) { watcher_appeared(bus, owner); },
                    [this] { watcher_vanished(); });
  }

  void watcher_appeared(dbus::Connection* bus, const std::string& owner) {
    {
      std::lock_guard lock(mutex_);
      watcher_present_ = true;
    }
    monitor_.wake();
    if (owner == unique_name_) return;  // our own watcher: items register with us
    // Another bar's watcher: register as a host and mirror its items.
    const std::string host = "org.kde.StatusNotifierHost-" + std::to_string(getpid());
    bus->call_async(Message::method_call(watcher_name, watcher_path, watcher_name, "RegisterStatusNotifierHost",
                                         {Value::string(host)}));
    if (external_signals_ != 0) bus->unsubscribe(external_signals_);
    external_signals_ = bus->subscribe({watcher_name, watcher_name, {}, watcher_path, {}, {}}, [this](const Message& signal) {
      const Value* service = signal.arg(0);
      if (service == nullptr || !service->is("s")) return;
      const auto [bus_name, path] = parse_service(service->text, "");
      if (bus_name.empty()) return;
      if (signal.member == "StatusNotifierItemRegistered") add_item(bus_name, path);
      else if (signal.member == "StatusNotifierItemUnregistered") remove_item(bus_name + path);
    });
    bus->call_async(Message::method_call(watcher_name, watcher_path, "org.freedesktop.DBus.Properties", "Get",
                                         {Value::string(watcher_name), Value::string("RegisteredStatusNotifierItems")}),
                    1000, [this](const Message* reply, const std::string&) {
                      if (reply == nullptr || reply->body.empty()) return;
                      for (const std::string& service : reply->body[0].string_list()) {
                        const auto [bus_name, path] = parse_service(service, "");
                        if (!bus_name.empty()) add_item(bus_name, path);
                      }
                    });
  }

  void watcher_vanished() {
    dbus::Connection* bus = monitor_.connection();
    if (external_signals_ != 0) {
      bus->unsubscribe(external_signals_);
      external_signals_ = 0;
    }
    // Items re-register with whichever watcher comes next.
    std::vector<std::string> keys;
    {
      std::lock_guard lock(mutex_);
      watcher_present_ = false;
      keys = order_;
    }
    for (const auto& key : keys) remove_item(key);
    monitor_.wake();
  }

  void emit(const char* signal, std::vector<Value> body) {
    if (!owns_watcher_) return;
    monitor_.connection()->emit(watcher_path, watcher_name, signal, std::move(body));
  }

  void add_item(const std::string& bus, const std::string& path) {
    const std::string key = bus + path;
    {
      std::lock_guard lock(mutex_);
      if (entries_.contains(key)) return;
      Entry entry;
      entry.bus = bus;
      entry.path = path;
      entry.item.id = key;
      entries_.emplace(key, std::move(entry));
      order_.push_back(key);
    }
    dbus::Connection* connection = monitor_.connection();
    const std::uint64_t watch = connection->watch_name(bus, {}, [this, key] { remove_item(key); });
    const std::uint64_t signals = connection->subscribe({bus, {}, {}, path, {}, {}}, [this, key](const Message& signal) {
      // NewIcon, NewTitle, NewStatus, NewToolTip, ...
      if (signal.member.starts_with("New")) fetch(key);
    });
    {
      std::lock_guard lock(mutex_);
      const auto found = entries_.find(key);
      if (found != entries_.end()) {
        found->second.name_watch = watch;
        found->second.item_signals = signals;
      }
    }
    emit("StatusNotifierItemRegistered", {Value::string(key)});
    fetch(key);
  }

  void remove_item(const std::string& key) {
    Entry entry;
    {
      std::lock_guard lock(mutex_);
      const auto found = entries_.find(key);
      if (found == entries_.end()) return;
      entry = std::move(found->second);
      entries_.erase(found);
      std::erase(order_, key);
    }
    dbus::Connection* connection = monitor_.connection();
    if (entry.name_watch != 0) connection->unwatch_name(entry.name_watch);
    if (entry.item_signals != 0) connection->unsubscribe(entry.item_signals);
    if (entry.menu_signals != 0) connection->unsubscribe(entry.menu_signals);
    emit("StatusNotifierItemUnregistered", {Value::string(key)});
    monitor_.wake();
  }

  void fetch(const std::string& key) {
    std::string bus;
    std::string path;
    {
      std::lock_guard lock(mutex_);
      const auto found = entries_.find(key);
      if (found == entries_.end()) return;
      bus = found->second.bus;
      path = found->second.path;
    }
    monitor_.connection()->call_async(
        Message::method_call(bus, path, "org.freedesktop.DBus.Properties", "GetAll", {Value::string(item_interface)}),
        2000, [this, key](const Message* reply, const std::string&) {
          if (reply != nullptr && !reply->body.empty()) update(key, &reply->body[0]);
        });
  }

  void update(const std::string& key, const Value* properties) {
    Item item;
    item.id = key;
    item.name = dbus::get_string(properties, "Id");
    item.title = dbus::get_string(properties, "Title");
    item.category = dbus::get_string(properties, "Category");
    const std::string status = dbus::get_string(properties, "Status");
    item.status = status == "Passive"          ? Status::passive
                  : status == "NeedsAttention" ? Status::needs_attention
                                               : Status::active;
    item.item_is_menu = dbus::get_bool(properties, "ItemIsMenu");
    const std::string menu_path = dbus::get_string(properties, "Menu");
    item.has_menu = menu_path.size() > 1;

    const bool attention = item.status == Status::needs_attention;
    std::string name = attention ? dbus::get_string(properties, "AttentionIconName") : "";
    const Value* pixmaps = properties->find(attention ? "AttentionIconPixmap" : "IconPixmap");
    if (attention && name.empty() && pixmaps == nullptr) {
      name = dbus::get_string(properties, "IconName");
      pixmaps = properties->find("IconPixmap");
    } else if (!attention) {
      name = dbus::get_string(properties, "IconName");
    }
    if (name.starts_with('/')) item.icon_path = name;
    else item.icon_path = theme_file(dbus::get_string(properties, "IconThemePath"), name);
    if (item.icon_path.empty() && !name.empty()) item.icon = name;
    if (item.icon_path.empty() && item.icon.empty()) item.icon_path = pixmap_file(pixmaps);

    if (const Value* tooltip = properties->find("ToolTip"); tooltip != nullptr && tooltip->is("(sa(iiay)ss)")) {
      item.tooltip_title = tooltip->items[2].text;
      item.tooltip_body = tooltip->items[3].text;
    }
    {
      std::lock_guard lock(mutex_);
      const auto found = entries_.find(key);
      if (found == entries_.end()) return;
      const std::string menu = item.has_menu ? menu_path : "";
      found->second.item = std::move(item);
      if (found->second.menu_path != menu) found->second.menu.reset();
      found->second.menu_path = menu;
    }
    monitor_.wake();
  }

  std::mutex mutex_;
  std::map<std::string, Entry> entries_;
  std::vector<std::string> order_;
  bool watcher_present_ = false;
  // Monitor thread only.
  std::string unique_name_;
  bool owns_watcher_ = false;
  std::uint64_t external_signals_ = 0;
  dbus::Monitor monitor_;
};

Tray& tray() {
  static Tray instance;
  return instance;
}

// Fire-and-forget: items may block while showing their own UI.
bool item_call(const std::string& id, const char* method, std::vector<Value> arguments) {
  const auto address = tray().address(id);
  dbus::Connection* bus = tray().monitor().connection();
  if (!address || bus == nullptr) return false;
  bus->call_async(Message::method_call(address->first, address->second, item_interface, method, std::move(arguments)));
  return true;
}

}  // namespace

void start() { tray().monitor(); }
bool available() { return tray().available(); }
std::vector<Item> items() { return tray().items(); }

bool activate(const std::string& id, int x, int y) {
  return item_call(id, "Activate", {Value::int32(x), Value::int32(y)});
}
bool secondary_activate(const std::string& id, int x, int y) {
  return item_call(id, "SecondaryActivate", {Value::int32(x), Value::int32(y)});
}
bool context_menu(const std::string& id, int x, int y) {
  return item_call(id, "ContextMenu", {Value::int32(x), Value::int32(y)});
}
bool scroll(const std::string& id, int delta, bool horizontal) {
  return item_call(id, "Scroll", {Value::int32(delta), Value::string(horizontal ? "horizontal" : "vertical")});
}

std::vector<MenuEntry> menu(const std::string& id) { return tray().menu(id); }

bool activate_menu(const std::string& id, std::int32_t entry) {
  const auto address = tray().menu_address(id);
  dbus::Connection* bus = tray().monitor().connection();
  if (!address || bus == nullptr) return false;
  // Fire-and-forget: the item may open a window before replying. It reports
  // layout changes, which invalidate the cached menu().
  bus->call_async(Message::method_call(address->first, address->second, menu_interface, "Event",
                                       {Value::int32(entry), Value::string("clicked"),
                                        Value::variant(Value::int32(0)), Value::uint32(0)}));
  return true;
}

int event_fd() { return tray().monitor().fd(); }
bool drain_events() { return tray().monitor().drain(); }

}  // namespace vw::tray
