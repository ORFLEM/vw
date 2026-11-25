#include "vw/upower.hpp"

#include "dbus.hpp"

#include <mutex>

namespace vw::upower {
namespace {

constexpr const char* upower_name = "org.freedesktop.UPower";
constexpr const char* upower_path = "/org/freedesktop/UPower";
constexpr const char* device_interface = "org.freedesktop.UPower.Device";
// power-profiles-daemon moved under UPower's name in 0.20; try both.
struct ProfilesBus {
  const char* name;
  const char* path;
};
constexpr ProfilesBus profiles_buses[] = {
    {"org.freedesktop.UPower.PowerProfiles", "/org/freedesktop/UPower/PowerProfiles"},
    {"net.hadess.PowerProfiles", "/net/hadess/PowerProfiles"},
};

struct Snapshot {
  bool available = false;
  bool on_battery = false;
  std::optional<Device> display;
  std::vector<Device> devices;
  std::vector<std::string> profiles;
  std::string profile;
  // Which profiles bus answered.
  const ProfilesBus* profiles_bus = nullptr;
};

// org.freedesktop.DBus.Properties.GetAll; the a{sv} dictionary or nothing.
std::optional<dbus::Value> get_all(dbus::Connection* bus, const char* name, const std::string& path,
                                   const char* interface) {
  if (bus == nullptr) return std::nullopt;
  return bus->call_value(dbus::Message::method_call(name, path, "org.freedesktop.DBus.Properties", "GetAll",
                                                    {dbus::Value::string(interface)}));
}

Device fetch_device(dbus::Connection* connection, const std::string& path) {
  Device device;
  device.path = path;
  const auto properties = get_all(connection, upower_name, path, device_interface);
  const dbus::Value* map = properties ? &*properties : nullptr;
  device.native_path = dbus::get_string(map, "NativePath");
  device.vendor = dbus::get_string(map, "Vendor");
  device.model = dbus::get_string(map, "Model");
  device.kind = static_cast<Kind>(dbus::get_int(map, "Type"));
  device.state = static_cast<State>(dbus::get_int(map, "State"));
  device.percentage = dbus::get_double(map, "Percentage");
  device.energy = dbus::get_double(map, "Energy");
  device.energy_full = dbus::get_double(map, "EnergyFull");
  device.energy_rate = dbus::get_double(map, "EnergyRate");
  device.time_to_empty = dbus::get_int(map, "TimeToEmpty");
  device.time_to_full = dbus::get_int(map, "TimeToFull");
  device.present = dbus::get_bool(map, "IsPresent");
  device.power_supply = dbus::get_bool(map, "PowerSupply");
  device.rechargeable = dbus::get_bool(map, "IsRechargeable");
  device.icon_name = dbus::get_string(map, "IconName");
  device.warning_level = static_cast<std::uint32_t>(dbus::get_int(map, "WarningLevel", 1));
  return device;
}

class Service {
 public:
  Service() : monitor_(dbus::Bus::system, [this](dbus::Connection* bus) { refresh(bus); }) {
    monitor_.watch(upower_name);
    for (const auto& bus : profiles_buses) monitor_.watch(bus.name);
  }

  Snapshot snapshot() {
    monitor_.start();
    std::lock_guard lock(mutex_);
    return snapshot_;
  }
  dbus::Monitor& monitor() {
    monitor_.start();
    return monitor_;
  }

 private:
  void refresh(dbus::Connection* bus) {
    Snapshot next;
    const auto manager = get_all(bus, upower_name, upower_path, upower_name);
    next.available = manager.has_value();
    next.on_battery = dbus::get_bool(manager ? &*manager : nullptr, "OnBattery");
    if (next.available) {
      if (const auto display = bus->call_value(dbus::Message::method_call(upower_name, upower_path, upower_name,
                                                                          "GetDisplayDevice"))) {
        Device device = fetch_device(bus, std::string(display->str()));
        if (device.present) next.display = std::move(device);
      }
      if (const auto list = bus->call_value(dbus::Message::method_call(upower_name, upower_path, upower_name,
                                                                       "EnumerateDevices")))
        for (const std::string& path : list->string_list()) next.devices.push_back(fetch_device(bus, path));
    }
    for (const auto& candidate : profiles_buses) {
      const auto properties = get_all(bus, candidate.name, candidate.path, candidate.name);
      if (!properties) continue;
      next.profiles_bus = &candidate;
      next.profile = dbus::get_string(&*properties, "ActiveProfile");
      if (const dbus::Value* list = properties->find("Profiles"); list != nullptr && list->is("aa{sv}"))
        for (const dbus::Value& entry : list->items) next.profiles.push_back(dbus::get_string(&entry, "Profile"));
      break;
    }
    std::lock_guard lock(mutex_);
    snapshot_ = std::move(next);
  }

  std::mutex mutex_;
  Snapshot snapshot_;
  dbus::Monitor monitor_;
};

Service& service() {
  static Service instance;
  return instance;
}

}  // namespace

bool available() { return service().snapshot().available; }
bool on_battery() { return service().snapshot().on_battery; }
std::optional<Device> display_device() { return service().snapshot().display; }
std::vector<Device> devices() { return service().snapshot().devices; }
std::vector<std::string> profiles() { return service().snapshot().profiles; }
std::string profile() { return service().snapshot().profile; }

bool set_profile(std::string_view name) {
  const Snapshot snapshot = service().snapshot();
  if (snapshot.profiles_bus == nullptr) return false;
  const std::string value(name);
  dbus::Connection* bus = service().monitor().connection();
  const bool ok = bus != nullptr &&
      bus->call(dbus::Message::method_call(snapshot.profiles_bus->name, snapshot.profiles_bus->path,
                                           "org.freedesktop.DBus.Properties", "Set",
                                           {dbus::Value::string(snapshot.profiles_bus->name),
                                            dbus::Value::string("ActiveProfile"),
                                            dbus::Value::variant(dbus::Value::string(value))}))
          .has_value();
  if (ok) service().monitor().schedule_refresh();
  return ok;
}

int event_fd() { return service().monitor().fd(); }
bool drain_events() { return service().monitor().drain(); }

}  // namespace vw::upower
