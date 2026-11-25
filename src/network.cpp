#include "vw/network.hpp"

#include "dbus.hpp"

#include <algorithm>
#include <mutex>
#include <set>
#include <unordered_map>

namespace vw::network {
namespace {

constexpr const char* nm_name = "org.freedesktop.NetworkManager";
constexpr const char* nm_path = "/org/freedesktop/NetworkManager";
constexpr const char* device_interface = "org.freedesktop.NetworkManager.Device";
constexpr const char* wireless_interface = "org.freedesktop.NetworkManager.Device.Wireless";
constexpr const char* ap_interface = "org.freedesktop.NetworkManager.AccessPoint";
constexpr const char* active_interface = "org.freedesktop.NetworkManager.Connection.Active";
constexpr const char* settings_path = "/org/freedesktop/NetworkManager/Settings";
constexpr const char* settings_interface = "org.freedesktop.NetworkManager.Settings";
constexpr const char* connection_interface = "org.freedesktop.NetworkManager.Settings.Connection";
constexpr std::uint32_t device_ethernet = 1;
constexpr std::uint32_t device_wifi = 2;
constexpr std::uint32_t device_activated = 100;

struct Snapshot {
  Status status;
  std::vector<AccessPoint> access_points;
  // For actions: the Wi-Fi device, the AP object per SSID, and the saved
  // connection per SSID.
  std::string wifi_device;
  std::string wifi_active_connection;
  std::unordered_map<std::string, std::string> ap_paths;
  std::unordered_map<std::string, std::string> saved;
};

Kind kind_of(const std::string& type) {
  if (type.empty()) return Kind::none;
  if (type == "802-3-ethernet") return Kind::wired;
  if (type == "802-11-wireless") return Kind::wifi;
  if (type == "vpn" || type == "wireguard") return Kind::vpn;
  return Kind::other;
}

using dbus::Message;
using dbus::Value;

std::optional<Value> call(dbus::Connection* bus, const std::string& path, const char* interface,
                          const char* method, std::vector<Value> arguments = {}, int timeout_ms = 1000) {
  if (bus == nullptr) return std::nullopt;
  return bus->call_value(Message::method_call(nm_name, path, interface, method, std::move(arguments)), timeout_ms);
}

// Whether a call succeeded, for methods without a reply value.
bool invoke(dbus::Connection* bus, const std::string& path, const char* interface, const char* method,
            std::vector<Value> arguments = {}, int timeout_ms = 1000) {
  return bus != nullptr &&
         bus->call(Message::method_call(nm_name, path, interface, method, std::move(arguments)), timeout_ms)
             .has_value();
}

std::optional<Value> get_all(dbus::Connection* bus, const std::string& path, const char* interface) {
  return call(bus, path, "org.freedesktop.DBus.Properties", "GetAll", {Value::string(interface)});
}

std::string trimmed_bytes(std::string_view bytes) {
  std::string result(bytes);
  while (!result.empty() && result.back() == '\0') result.pop_back();
  return result;
}

// SSID → settings object of every saved Wi-Fi connection.
std::unordered_map<std::string, std::string> saved_connections(dbus::Connection* bus) {
  std::unordered_map<std::string, std::string> saved;
  const auto paths = call(bus, settings_path, settings_interface, "ListConnections");
  if (!paths) return saved;
  for (const auto& path : paths->string_list()) {
    const auto settings = call(bus, path, connection_interface, "GetSettings");
    const Value* wireless = settings ? settings->find("802-11-wireless") : nullptr;
    if (wireless == nullptr) continue;
    const Value* ssid = wireless->find("ssid");
    const std::string name = ssid == nullptr ? std::string() : trimmed_bytes(ssid->str());
    if (!name.empty()) saved.emplace(name, path);
  }
  return saved;
}

class Service {
 public:
  Service()
      : monitor_(dbus::Bus::system, [this](dbus::Connection* bus) { refresh(bus); }, {}, 150) {
    monitor_.watch(nm_name);
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
    Status& status = next.status;
    const auto manager_value = get_all(bus, nm_path, nm_name);
    const Value* manager = manager_value ? &*manager_value : nullptr;
    status.available = manager != nullptr;
    if (!status.available) {
      std::lock_guard lock(mutex_);
      snapshot_ = std::move(next);
      return;
    }
    status.networking_enabled = dbus::get_bool(manager, "NetworkingEnabled");
    status.wifi_enabled = dbus::get_bool(manager, "WirelessEnabled");
    status.wifi_hardware_enabled = dbus::get_bool(manager, "WirelessHardwareEnabled");
    status.connectivity = static_cast<Connectivity>(dbus::get_int(manager, "Connectivity"));
    status.primary = kind_of(dbus::get_string(manager, "PrimaryConnectionType"));

    const std::string primary = dbus::get_string(manager, "PrimaryConnection");
    if (primary.size() > 1) {
      const auto active = get_all(bus, primary, active_interface);
      status.connection = dbus::get_string(active ? &*active : nullptr, "Id");
      const std::string ip4 = dbus::get_string(active ? &*active : nullptr, "Ip4Config");
      if (ip4.size() > 1) {
        const auto config = get_all(bus, ip4, "org.freedesktop.NetworkManager.IP4Config");
        const Value* addresses = config ? config->find("AddressData") : nullptr;
        if (addresses != nullptr && addresses->is("aa{sv}") && addresses->size() > 0) {
          const Value* first = addresses->at(0);
          status.address = dbus::get_string(first, "address");
          if (const auto prefix = dbus::get_int(first, "prefix", -1); prefix >= 0)
            status.address += "/" + std::to_string(prefix);
        }
      }
    }

    std::string active_ap;
    const auto devices = call(bus, nm_path, nm_name, "GetDevices");
    for (const auto& path : devices ? devices->string_list() : std::vector<std::string>{}) {
      const auto device_value = get_all(bus, path, device_interface);
      const Value* device = device_value ? &*device_value : nullptr;
      const auto type = dbus::get_int(device, "DeviceType");
      const auto state = dbus::get_int(device, "State");
      if (type == device_ethernet && state == device_activated) status.wired = true;
      if (type != device_wifi || !next.wifi_device.empty()) continue;
      status.has_wifi = true;
      next.wifi_device = path;
      const std::string active = dbus::get_string(device, "ActiveConnection");
      if (active.size() > 1) next.wifi_active_connection = active;
      const auto wireless_value = get_all(bus, path, wireless_interface);
      const Value* wireless = wireless_value ? &*wireless_value : nullptr;
      active_ap = dbus::get_string(wireless, "ActiveAccessPoint");
      for (const auto& ap_path : dbus::get_strings(wireless, "AccessPoints")) {
        const auto ap_value = get_all(bus, ap_path, ap_interface);
        if (!ap_value) continue;
        const Value* ap = &*ap_value;
        AccessPoint point;
        const Value* ssid = ap->find("Ssid");
        point.ssid = ssid == nullptr ? std::string() : trimmed_bytes(ssid->str());
        if (point.ssid.empty()) continue;
        point.bssid = dbus::get_string(ap, "HwAddress");
        point.strength = static_cast<std::uint8_t>(dbus::get_int(ap, "Strength"));
        point.frequency = static_cast<std::uint32_t>(dbus::get_int(ap, "Frequency"));
        point.secured = (dbus::get_int(ap, "Flags") & 1) != 0 || dbus::get_int(ap, "WpaFlags") != 0 ||
                        dbus::get_int(ap, "RsnFlags") != 0;
        point.active = ap_path == active_ap;
        // Keep the strongest AP per SSID, but always the active one.
        auto existing = std::ranges::find(next.access_points, point.ssid, &AccessPoint::ssid);
        if (existing == next.access_points.end()) {
          next.ap_paths[point.ssid] = ap_path;
          next.access_points.push_back(std::move(point));
        } else if (point.active || (!existing->active && point.strength > existing->strength)) {
          next.ap_paths[point.ssid] = ap_path;
          *existing = std::move(point);
        }
      }
    }

    next.saved = saved_connections(bus);
    for (auto& point : next.access_points) {
      point.saved = next.saved.contains(point.ssid);
      if (point.active) {
        status.ssid = point.ssid;
        status.strength = point.strength;
      }
    }
    std::ranges::sort(next.access_points, [](const AccessPoint& a, const AccessPoint& b) {
      if (a.active != b.active) return a.active;
      return a.strength > b.strength;
    });
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

dbus::Connection* bus() { return service().monitor().connection(); }

bool done(bool ok) {
  if (ok) service().monitor().schedule_refresh();
  return ok;
}

}  // namespace

Status status() { return service().snapshot().status; }
std::vector<AccessPoint> access_points() { return service().snapshot().access_points; }

std::string icon_name(const Status& status) {
  if (!status.available || !status.networking_enabled) return "network-offline-symbolic";
  switch (status.primary) {
    case Kind::wired: return "network-wired-symbolic";
    case Kind::vpn: return "network-vpn-symbolic";
    case Kind::wifi: {
      const char* level = status.strength >= 80 ? "excellent"
                          : status.strength >= 55 ? "good"
                          : status.strength >= 30 ? "ok"
                          : status.strength >= 5  ? "weak"
                                                  : "none";
      return std::string("network-wireless-signal-") + level + "-symbolic";
    }
    case Kind::other: return "network-transmit-receive-symbolic";
    case Kind::none: break;
  }
  if (status.has_wifi && !status.wifi_enabled) return "network-wireless-disabled-symbolic";
  return status.has_wifi ? "network-wireless-offline-symbolic" : "network-wired-disconnected-symbolic";
}

bool set_wifi_enabled(bool enabled) {
  return done(invoke(bus(), nm_path, "org.freedesktop.DBus.Properties", "Set",
                     {Value::string(nm_name), Value::string("WirelessEnabled"),
                      Value::variant(Value::boolean(enabled))}));
}

bool scan() {
  const Snapshot snapshot = service().snapshot();
  if (snapshot.wifi_device.empty()) return false;
  return invoke(bus(), snapshot.wifi_device, wireless_interface, "RequestScan", {Value::dict("s", "v")});
}

bool connect(std::string_view ssid_view, std::string_view password) {
  const Snapshot snapshot = service().snapshot();
  if (snapshot.wifi_device.empty()) return false;
  const std::string ssid(ssid_view);
  const auto ap = snapshot.ap_paths.find(ssid);
  const std::string specific = ap != snapshot.ap_paths.end() ? ap->second : "/";
  if (const auto saved = snapshot.saved.find(ssid); saved != snapshot.saved.end() && password.empty())
    return done(invoke(bus(), nm_path, nm_name, "ActivateConnection",
                       {Value::object_path(saved->second), Value::object_path(snapshot.wifi_device),
                        Value::object_path(specific)},
                       10000));

  std::vector<std::pair<Value, Value>> settings;
  settings.emplace_back(Value::string("802-11-wireless"),
                        Value::dict("s", "v", {{Value::string("ssid"), Value::variant(Value::bytes(ssid))}}));
  if (!password.empty())
    settings.emplace_back(Value::string("802-11-wireless-security"),
                          Value::dict("s", "v",
                                      {{Value::string("key-mgmt"), Value::variant(Value::string("wpa-psk"))},
                                       {Value::string("psk"), Value::variant(Value::string(password))}}));
  return done(invoke(bus(), nm_path, nm_name, "AddAndActivateConnection",
                     {Value::dict("s", "a{sv}", std::move(settings)), Value::object_path(snapshot.wifi_device),
                      Value::object_path(specific)},
                     10000));
}

bool disconnect_wifi() {
  const Snapshot snapshot = service().snapshot();
  if (snapshot.wifi_active_connection.empty()) return false;
  return done(invoke(bus(), nm_path, nm_name, "DeactivateConnection",
                     {Value::object_path(snapshot.wifi_active_connection)}));
}

int event_fd() { return service().monitor().fd(); }
bool drain_events() { return service().monitor().drain(); }

}  // namespace vw::network
