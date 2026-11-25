#include "vw/mpris.hpp"

#include "dbus.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace vw::mpris {
namespace {

using dbus::Message;
using dbus::Value;

constexpr const char* prefix = "org.mpris.MediaPlayer2.";
constexpr const char* path = "/org/mpris/MediaPlayer2";
constexpr const char* player_interface = "org.mpris.MediaPlayer2.Player";
constexpr const char* properties_interface = "org.freedesktop.DBus.Properties";

struct Snapshot {
  std::vector<Player> players;
  std::string active_service;
  std::chrono::steady_clock::time_point fetched{};
};

double time_property(const Value* dictionary, const char* key) {
  const Value* value = dictionary == nullptr ? nullptr : dictionary->find(key);
  if (value != nullptr && value->is("x")) return static_cast<double>(static_cast<std::int64_t>(value->bits)) / 1000000.0;
  return 0;
}

// Paths become file:// URIs; anything with a scheme stays as it is.
std::string artwork_url(std::string raw) {
  if (raw.empty() || raw.front() != '/') return raw;
  std::string uri = "file://";
  for (const unsigned char c : raw) {
    if (std::isalnum(c) != 0 || std::string_view("/-._~!$&'()*+,;=:@").find(static_cast<char>(c)) != std::string_view::npos) {
      uri.push_back(static_cast<char>(c));
    } else {
      constexpr const char* digits = "0123456789ABCDEF";
      uri += '%';
      uri += digits[c >> 4U];
      uri += digits[c & 15U];
    }
  }
  return uri;
}

Player fetch_player(dbus::Connection* bus, const std::string& service) {
  Player player;
  player.service = service;
  player.name = service.substr(std::char_traits<char>::length(prefix));
  const auto reply = bus->call_value(
      Message::method_call(service, path, properties_interface, "GetAll", {Value::string(player_interface)}));
  if (!reply) return player;
  const Value* properties = &*reply;
  player.playback_status = dbus::get_string(properties, "PlaybackStatus");
  if (player.playback_status.empty()) player.playback_status = "Stopped";
  player.position = time_property(properties, "Position");
  player.can_go_previous = dbus::get_bool(properties, "CanGoPrevious");
  player.can_play = dbus::get_bool(properties, "CanPlay");
  player.can_pause = dbus::get_bool(properties, "CanPause");
  player.can_go_next = dbus::get_bool(properties, "CanGoNext");

  const Value* metadata = properties->find("Metadata");
  player.title = dbus::get_string(metadata, "xesam:title");
  for (const std::string& artist : dbus::get_strings(metadata, "xesam:artist")) {
    if (!player.artist.empty()) player.artist += ", ";
    player.artist += artist;
  }
  player.album = dbus::get_string(metadata, "xesam:album");
  player.art_url = artwork_url(dbus::get_string(metadata, "mpris:artUrl"));
  for (const char* key : {"xesam:lyrics", "xesam:lyric", "xesam:asText"}) {
    if (player.lyrics.empty()) player.lyrics = dbus::get_string(metadata, key);
  }
  player.track_id = dbus::get_string(metadata, "mpris:trackid");
  player.length = time_property(metadata, "mpris:length");
  return player;
}

Snapshot fetch_snapshot(dbus::Connection* bus, const std::string& previous) {
  Snapshot result;
  if (const auto names = bus->call_value(Message::method_call("org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                              "org.freedesktop.DBus", "ListNames")))
    for (const std::string& name : names->string_list())
      if (name.starts_with(prefix)) result.players.push_back(fetch_player(bus, name));
  std::ranges::sort(result.players, {}, &Player::service);
  std::string playing;
  for (const auto& player : result.players) {
    if (player.playback_status != "Playing") continue;
    if (!player.art_url.empty()) {
      result.active_service = player.service;
      break;
    }
    if (playing.empty()) playing = player.service;
  }
  if (result.active_service.empty() && !playing.empty()) {
    const auto selected = std::ranges::find(result.players, playing, &Player::service);
    if (selected != result.players.end() && !selected->title.empty()) {
      for (const auto& player : result.players) {
        if (player.title == selected->title && !player.art_url.empty()) {
          result.active_service = player.service;
          break;
        }
      }
    }
  }
  if (result.active_service.empty()) {
    if (!playing.empty()) result.active_service = playing;
    else if (std::ranges::find(result.players, previous, &Player::service) != result.players.end())
      result.active_service = previous;
    else if (!result.players.empty()) result.active_service = result.players.front().service;
  }
  result.fetched = std::chrono::steady_clock::now();
  return result;
}

// Player state lives on a dbus::Monitor thread and is refetched only when a
// player signals (PropertiesChanged, Seeked) or a player name comes or goes;
// position in between is extrapolated. Readers never touch the bus.
class MprisService {
 public:
  MprisService()
      : monitor_(dbus::Bus::session, [this](dbus::Connection* bus) { refresh(bus); },
                 [this](dbus::Connection* bus) { subscribe(bus); }) {}

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
    std::string previous;
    {
      std::lock_guard lock(mutex_);
      previous = snapshot_.active_service;
    }
    Snapshot next = fetch_snapshot(bus, previous);
    std::lock_guard lock(mutex_);
    snapshot_ = std::move(next);
  }

  // Player names are dynamic, so match by path and interface rather than by
  // sender (dbus::Monitor::watch only takes fixed names).
  void subscribe(dbus::Connection* bus) {
    const auto on_signal = [this](const Message&) { monitor_.schedule_refresh(); };
    bus->subscribe({{}, properties_interface, "PropertiesChanged", path, player_interface, {}}, on_signal);
    bus->subscribe({{}, player_interface, "Seeked", path, {}, {}}, on_signal);
    bus->subscribe({"org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged", "/org/freedesktop/DBus", {},
                    "org.mpris.MediaPlayer2"},
                   on_signal);
  }

  std::mutex mutex_;
  Snapshot snapshot_;
  dbus::Monitor monitor_;
};

MprisService& service() {
  static MprisService instance;
  return instance;
}

const Player* active_player(const Snapshot& snapshot) {
  const auto found = std::ranges::find(snapshot.players, snapshot.active_service,
                                       &Player::service);
  return found == snapshot.players.end() ? nullptr : &*found;
}

double position_now(const Snapshot& snapshot, const Player& player) {
  double position = player.position;
  if (player.playback_status == "Playing")
    position += std::chrono::duration<double>(
        std::chrono::steady_clock::now() - snapshot.fetched).count();
  return player.length > 0 ? std::clamp(position, 0.0, player.length)
                           : std::max(0.0, position);
}

Player current(const Snapshot& snapshot, Player player) {
  player.position = position_now(snapshot, player);
  return player;
}

bool control(const char* method, bool (*allowed)(const Player&)) {
  const auto snapshot = service().snapshot();
  const Player* player = active_player(snapshot);
  if (player == nullptr || !allowed(*player)) return false;
  dbus::Connection* bus = service().monitor().connection();
  const bool success = bus != nullptr && bus->call(Message::method_call(player->service, path, player_interface, method)).has_value();
  service().monitor().schedule_refresh();
  return success;
}

}  // namespace

std::optional<Player> active() {
  const Snapshot snapshot = service().snapshot();
  const Player* player = active_player(snapshot);
  if (player == nullptr) return std::nullopt;
  return current(snapshot, *player);
}

std::vector<Player> players() {
  const Snapshot snapshot = service().snapshot();
  std::vector<Player> result;
  result.reserve(snapshot.players.size());
  for (const Player& player : snapshot.players) result.push_back(current(snapshot, player));
  return result;
}

bool available() { return active_player(service().snapshot()) != nullptr; }

bool previous() {
  return control("Previous", [](const Player& player) { return player.can_go_previous; });
}

bool play_pause() {
  return control("PlayPause", [](const Player& player) { return player.can_play || player.can_pause; });
}

bool next() {
  return control("Next", [](const Player& player) { return player.can_go_next; });
}

bool set_position(double seconds) {
  if (!std::isfinite(seconds)) throw std::invalid_argument("position must be finite");
  const auto snapshot = service().snapshot();
  const Player* player = active_player(snapshot);
  if (player == nullptr || player->length <= 0 || !dbus::valid_object_path(player->track_id)) return false;
  const double clamped = std::clamp(seconds, 0.0, player->length);
  dbus::Connection* bus = service().monitor().connection();
  const bool success =
      bus != nullptr &&
      bus->call(Message::method_call(player->service, path, player_interface, "SetPosition",
                                     {Value::object_path(player->track_id),
                                      Value::int64(static_cast<std::int64_t>(clamped * 1000000.0))}))
          .has_value();
  service().monitor().schedule_refresh();
  return success;
}

int event_fd() { return service().monitor().fd(); }
bool drain_events() { return service().monitor().drain(); }

}  // namespace vw::mpris
