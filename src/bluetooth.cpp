#include "vw/bluetooth.hpp"

#include "dbus.hpp"

#include <algorithm>
#include <mutex>
#include <set>
#include <utility>

namespace vw::bluetooth {
namespace {

constexpr const char* bluez = "org.bluez";
constexpr const char* adapter_interface = "org.bluez.Adapter1";
constexpr const char* device_interface = "org.bluez.Device1";
constexpr const char* properties_interface = "org.freedesktop.DBus.Properties";

using dbus::Message;
using dbus::Value;

bool invoke(dbus::Connection* bus, const std::string& path, const char* interface, const char* method,
            std::vector<Value> arguments = {}) {
  return bus != nullptr &&
         bus->call(Message::method_call(bluez, path, interface, method, std::move(arguments))).has_value();
}

bool set_property(dbus::Connection* bus, const std::string& path, const char* interface, const char* property,
                  Value value) {
  return invoke(bus, path, properties_interface, "Set",
                {Value::string(interface), Value::string(property), Value::variant(std::move(value))});
}

struct Snapshot {
  bool available = false;
  std::vector<Adapter> adapters;
  std::vector<Device> devices;
};

class Service {
 public:
  Service() : monitor_(dbus::Bus::system, [this](dbus::Connection* bus) { refresh(bus); }) {
    monitor_.watch(bluez);
  }

  Snapshot snapshot() {
    monitor_.start();
    std::lock_guard lock(mutex_);
    Snapshot result = snapshot_;
    for (auto& device : result.devices) device.busy = busy_.contains(device.path);
    return result;
  }
  dbus::Monitor& monitor() {
    monitor_.start();
    return monitor_;
  }
  std::string last_error() {
    std::lock_guard lock(mutex_);
    return error_;
  }

  // Calls a Device1 method asynchronously on the monitor thread. then, when
  // set, runs there after success.
  void device_call(const std::string& path, const char* method,
                   std::function<void(dbus::Connection*)> then = {}) {
    {
      std::lock_guard lock(mutex_);
      busy_.insert(path);
      error_.clear();
    }
    monitor_.wake();
    monitor_.invoke([this, path, method, then = std::move(then)]() mutable {
      dbus::Connection* bus = monitor_.connection();
      bus->call_async(Message::method_call(bluez, path, device_interface, method), 60000,
                      [this, bus, path, then = std::move(then)](const Message* reply, const std::string& error) {
                        {
                          std::lock_guard lock(mutex_);
                          busy_.erase(path);
                          // BlueZ errors read "org.bluez.Error.X: message"; keep the message.
                          if (reply == nullptr) {
                            const auto colon = error.find(": ");
                            error_ = colon == std::string::npos ? error : error.substr(colon + 2);
                          }
                        }
                        if (reply != nullptr && then) then(bus);
                        monitor_.schedule_refresh();
                        monitor_.wake();
                      });
    });
  }

 private:
  void refresh(dbus::Connection* bus) {
    Snapshot next;
    const auto objects = bus->call_value(
        Message::method_call(bluez, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects"));
    next.available = objects && objects->is("a{oa{sa{sv}}}");
    if (next.available) {
      for (const Value& object : objects->items) {
        const std::string path = object.items[0].text;
        const Value& interfaces = object.items[1];
        if (const Value* adapter = interfaces.find(adapter_interface)) {
          next.adapters.push_back({
              .path = path,
              .name = dbus::get_string(adapter, "Alias"),
              .address = dbus::get_string(adapter, "Address"),
              .powered = dbus::get_bool(adapter, "Powered"),
              .discovering = dbus::get_bool(adapter, "Discovering"),
              .discoverable = dbus::get_bool(adapter, "Discoverable"),
          });
        }
        const Value* device = interfaces.find(device_interface);
        if (device == nullptr) continue;
        Device entry{
            .path = path,
            .adapter = dbus::get_string(device, "Adapter"),
            .name = dbus::get_string(device, "Name"),
            .address = dbus::get_string(device, "Address"),
            .icon = dbus::get_string(device, "Icon"),
            .paired = dbus::get_bool(device, "Paired"),
            .trusted = dbus::get_bool(device, "Trusted"),
            .connected = dbus::get_bool(device, "Connected"),
            .busy = false,
            .battery = std::nullopt,
        };
        // Alias falls back to the address for unnamed devices; keep those
        // only once paired.
        const std::string alias = dbus::get_string(device, "Alias");
        if (entry.name.empty() && !entry.paired) continue;
        if (!alias.empty()) entry.name = alias;
        if (const Value* battery = interfaces.find("org.bluez.Battery1")) {
          const auto percentage = dbus::get_int(battery, "Percentage", -1);
          if (percentage >= 0) entry.battery = static_cast<std::uint8_t>(percentage);
        }
        next.devices.push_back(std::move(entry));
      }
    }
    std::ranges::sort(next.adapters, {}, &Adapter::path);
    std::ranges::sort(next.devices, [](const Device& a, const Device& b) {
      if (a.connected != b.connected) return a.connected;
      if (a.paired != b.paired) return a.paired;
      return a.name < b.name;
    });
    std::lock_guard lock(mutex_);
    snapshot_ = std::move(next);
  }

  std::mutex mutex_;
  Snapshot snapshot_;
  std::set<std::string> busy_;
  std::string error_;
  dbus::Monitor monitor_;
};

Service& service() {
  static Service instance;
  return instance;
}

bool known(const std::string& path) {
  const auto devices = service().snapshot().devices;
  return std::ranges::find(devices, path, &Device::path) != devices.end();
}

bool adapter_call(const char* method) {
  const auto first = adapter();
  if (!first) return false;
  const bool ok = invoke(service().monitor().connection(), first->path, adapter_interface, method);
  if (ok) service().monitor().schedule_refresh();
  return ok;
}

}  // namespace

bool available() { return service().snapshot().available; }

std::optional<Adapter> adapter() {
  auto list = service().snapshot().adapters;
  if (list.empty()) return std::nullopt;
  return list.front();
}

std::vector<Adapter> adapters() { return service().snapshot().adapters; }
std::vector<Device> devices() { return service().snapshot().devices; }

bool set_powered(bool powered) {
  const auto first = adapter();
  if (!first) return false;
  const bool ok = set_property(service().monitor().connection(), first->path, adapter_interface, "Powered",
                               Value::boolean(powered));
  if (ok) service().monitor().schedule_refresh();
  return ok;
}

bool set_discovering(bool discovering) {
  return adapter_call(discovering ? "StartDiscovery" : "StopDiscovery");
}

bool connect(const std::string& device) {
  if (!known(device)) return false;
  service().device_call(device, "Connect");
  return true;
}

bool disconnect(const std::string& device) {
  if (!known(device)) return false;
  service().device_call(device, "Disconnect");
  return true;
}

bool pair(const std::string& device) {
  if (!known(device)) return false;
  service().device_call(device, "Pair", [device](dbus::Connection* bus) {
    set_property(bus, device, device_interface, "Trusted", Value::boolean(true));
    service().device_call(device, "Connect");
  });
  return true;
}

bool forget(const std::string& device) {
  const auto devices = service().snapshot().devices;
  const auto found = std::ranges::find(devices, device, &Device::path);
  if (found == devices.end()) return false;
  const bool ok = invoke(service().monitor().connection(), found->adapter, adapter_interface, "RemoveDevice",
                         {Value::object_path(device)});
  if (ok) service().monitor().schedule_refresh();
  return ok;
}

std::string last_error() { return service().last_error(); }
int event_fd() { return service().monitor().fd(); }
bool drain_events() { return service().monitor().drain(); }

}  // namespace vw::bluetooth
