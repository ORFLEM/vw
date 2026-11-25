#include "vw/notifications.hpp"

#include "vw/icon.hpp"

#include "dbus.hpp"
#include "desktop_entry.hpp"

#include <cairo.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <sys/eventfd.h>
#include <unistd.h>

#ifndef VW_VERSION
#define VW_VERSION "unknown"
#endif

namespace vw::notifications {
namespace {

constexpr const char* bus_name = "org.freedesktop.Notifications";
constexpr const char* object_path = "/org/freedesktop/Notifications";
constexpr const char* interface_xml = R"XML(
  <method name="GetCapabilities"><arg type="as" direction="out"/></method>
  <method name="GetServerInformation">
    <arg type="s" direction="out"/><arg type="s" direction="out"/>
    <arg type="s" direction="out"/><arg type="s" direction="out"/>
  </method>
  <method name="Notify">
    <arg name="app_name" type="s" direction="in"/><arg name="replaces_id" type="u" direction="in"/>
    <arg name="app_icon" type="s" direction="in"/><arg name="summary" type="s" direction="in"/>
    <arg name="body" type="s" direction="in"/><arg name="actions" type="as" direction="in"/>
    <arg name="hints" type="a{sv}" direction="in"/><arg name="expire_timeout" type="i" direction="in"/>
    <arg name="id" type="u" direction="out"/>
  </method>
  <method name="CloseNotification"><arg name="id" type="u" direction="in"/></method>
  <signal name="NotificationClosed"><arg name="id" type="u"/><arg name="reason" type="u"/></signal>
  <signal name="ActionInvoked"><arg name="id" type="u"/><arg name="action_key" type="s"/></signal>
)XML";

// Spec close reasons.
constexpr std::uint32_t expired = 1;
constexpr std::uint32_t dismissed = 2;
constexpr std::uint32_t withdrawn = 3;
constexpr std::uint32_t evicted = 4;

constexpr int default_timeout_ms = 5000;
constexpr std::size_t history_limit = 256;
constexpr std::size_t open_limit = 256;

using Clock = std::chrono::steady_clock;
using dbus::Message;
using dbus::Value;

// --- Hints ---------------------------------------------------------------------

class Hints {
 public:
  explicit Hints(const Value& dictionary) : dictionary_(dictionary) {}

  const Value* get(const char* key) const { return dictionary_.find(key); }
  // The first of `keys` present as a string.
  std::string string(std::initializer_list<const char*> keys) const {
    for (const char* key : keys)
      if (const Value* value = get(key); value != nullptr && value->is("s")) return value->text;
    return {};
  }
  bool boolean(const char* key) const {
    const Value* value = get(key);
    return value != nullptr && value->flag();
  }
  // Senders disagree on integer types (urgency is a byte, value an int).
  std::optional<std::int64_t> integer(const char* key) const {
    const Value* value = get(key);
    return value == nullptr ? std::nullopt : value->integer();
  }

 private:
  const Value& dictionary_;
};

// --- Icons and images ----------------------------------------------------------

std::string decode_uri_path(std::string_view uri) {
  std::string path;
  for (std::size_t i = 0; i < uri.size(); ++i) {
    if (uri[i] == '%' && i + 2 < uri.size() && std::isxdigit(static_cast<unsigned char>(uri[i + 1])) != 0 &&
        std::isxdigit(static_cast<unsigned char>(uri[i + 2])) != 0) {
      path.push_back(static_cast<char>(std::stoi(std::string(uri.substr(i + 1, 2)), nullptr, 16)));
      i += 2;
    } else {
      path.push_back(uri[i]);
    }
  }
  return path;
}

// A path, a file:// URI or a theme name; empty for a missing file.
std::string icon_source(const std::string& value) {
  std::string result = value;
  if (value.starts_with("file://")) {
    std::string_view rest = std::string_view(value).substr(7);
    // file://host/path: only the local host.
    if (!rest.starts_with("/")) {
      const auto slash = rest.find('/');
      rest = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash);
    }
    result = decode_uri_path(rest);
  }
  std::error_code error;
  if (result.starts_with('/') && !std::filesystem::exists(result, error)) return {};
  return result;
}

std::string desktop_icon(const std::string& entry) {
  if (entry.empty()) return {};
  const std::string id = entry.ends_with(".desktop") ? entry : entry + ".desktop";
  const auto application = applications::find(id);
  return application ? application->icon : std::string();
}

// Only the daemon holding the bus name writes here, so anything found on
// first use was left by an earlier run.
const std::filesystem::path& image_directory() {
  static const std::filesystem::path directory = [] {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    const std::filesystem::path base = runtime != nullptr && *runtime != '\0'
        ? std::filesystem::path(runtime)
        : std::filesystem::temp_directory_path() / ("vw-" + std::to_string(getuid()));
    std::filesystem::path path = base / "vw" / "notifications";
    std::error_code error;
    std::filesystem::remove_all(path, error);
    std::filesystem::create_directories(path, error);
    return path;
  }();
  return directory;
}

// Writes an image-data hint, (iiibiiay): width, height, rowstride, has alpha,
// bits per sample, channels, pixels. The PNG's path, or empty when malformed.
std::string write_image(const Value& value) {
  if (!value.is("(iiibiiay)")) return {};
  const auto field = [&](std::size_t index) { return static_cast<std::int32_t>(value.items[index].bits); };
  const int width = field(0), height = field(1), rowstride = field(2), bits = field(4), channels = field(5);
  const bool has_alpha = value.items[3].flag();
  const std::string& data = value.items[6].text;
  const std::size_t size = data.size();
  const auto* pixels = reinterpret_cast<const std::uint8_t*>(data.data());
  constexpr int limit = 4096;
  if (width <= 0 || height <= 0 || width > limit || height > limit || bits != 8 || channels != (has_alpha ? 4 : 3))
    return {};
  const auto row = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(channels);
  if (rowstride < 0 || static_cast<std::uint64_t>(rowstride) < row ||
      static_cast<std::uint64_t>(rowstride) * static_cast<std::uint64_t>(height - 1) + row > size)
    return {};

  cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(surface);
    return {};
  }
  cairo_surface_flush(surface);
  std::uint8_t* target = cairo_image_surface_get_data(surface);
  const int stride = cairo_image_surface_get_stride(surface);
  for (int y = 0; y < height; ++y) {
    const std::uint8_t* source = pixels + static_cast<std::ptrdiff_t>(y) * rowstride;
    auto* line = reinterpret_cast<std::uint32_t*>(target + static_cast<std::ptrdiff_t>(y) * stride);
    for (int x = 0; x < width; ++x, source += channels) {
      const std::uint32_t alpha = has_alpha ? source[3] : 255U;
      const auto scale = [alpha](std::uint32_t channel) { return (channel * alpha + 127U) / 255U; };
      line[x] = alpha << 24U | scale(source[0]) << 16U | scale(source[1]) << 8U | scale(source[2]);
    }
  }
  cairo_surface_mark_dirty(surface);
  static std::uint64_t next = 0;  // D-Bus thread only
  const auto path = image_directory() / std::format("{}-{}.png", getpid(), ++next);
  const cairo_status_t status = cairo_surface_write_to_png(surface, path.c_str());
  cairo_surface_destroy(surface);
  return status == CAIRO_STATUS_SUCCESS ? path.string() : "";
}

void remove_file(const std::string& path) {
  if (path.empty()) return;
  std::error_code error;
  std::filesystem::remove(path, error);
}

// --- Model -----------------------------------------------------------------------

struct Record : Notification {
  Clock::time_point expires{};
  // Paths or theme names; resolved into icon and image by readers.
  std::string icon_source;
  std::string image_source;
  // The PNG written for image-data, removed with the record.
  std::string owned_image;
  // Synchronous / stack tag: a later notification with the same tag from
  // the same app replaces this one in place.
  std::string tag;
  // In the history (see the header).
  bool kept = true;
};

enum class View : std::size_t { open, popups, history };

struct Signal {
  const char* name;
  std::uint32_t id;
  std::uint32_t reason;  // NotificationClosed
  std::string key;       // ActionInvoked
};

class Service {
 public:
  Service() : event_(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {
    if (event_ < 0) throw std::runtime_error("could not create notification event fd");
  }
  ~Service() {
    if (thread_.joinable()) {
      thread_.request_stop();
      loop_.quit();
      thread_.join();
    }
    for (const Record& record : records_) remove_file(record.owned_image);
    close(event_);
  }
  Service(const Service&) = delete;
  Service& operator=(const Service&) = delete;

  StartResult start() {
    std::unique_lock lock(mutex_);
    if (!started_) {
      started_ = true;
      thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
    }
    ready_condition_.wait(lock, [this] { return ready_; });
    return {running_, error_};
  }
  bool running() {
    std::lock_guard lock(mutex_);
    return running_;
  }
  int fd() const { return event_; }
  bool drain() {
    std::uint64_t count = 0;
    bool changed = false;
    while (read(event_, &count, sizeof(count)) > 0) changed = true;
    return changed;
  }
  std::uint64_t generation() {
    std::lock_guard lock(mutex_);
    return generation_;
  }

  // Rebuilt only when the model changed since the last call for this view.
  std::vector<Notification> snapshot(View view) {
    std::lock_guard cache_lock(cache_mutex_);
    Cache& cache = caches_[static_cast<std::size_t>(view)];
    std::vector<Record> matching;
    {
      std::lock_guard lock(mutex_);
      if (cache.generation == generation_) return cache.items;
      cache.generation = generation_;
      for (auto record = records_.rbegin(); record != records_.rend(); ++record) {
        const bool wanted = view == View::open     ? !record->closed
                            : view == View::popups ? record->popup && !record->closed
                                                   : record->kept;
        if (wanted) matching.push_back(*record);
      }
    }
    // Theme lookups happen here, on the reader's thread, once per source.
    if (resolved_.size() > 1024) resolved_.clear();
    const auto resolve = [this](const std::string& source) -> std::string {
      if (source.empty()) return {};
      auto found = resolved_.find(source);
      if (found == resolved_.end()) found = resolved_.emplace(source, resolve_icon(source).string()).first;
      return found->second;
    };
    cache.items.clear();
    cache.items.reserve(matching.size());
    for (Record& record : matching) {
      record.icon = resolve(record.icon_source);
      record.image = resolve(record.image_source);
      cache.items.push_back(std::move(static_cast<Notification&>(record)));
    }
    return cache.items;
  }

  bool dismiss(std::uint32_t id) {
    std::vector<Signal> signals;
    bool found = false;
    {
      std::lock_guard lock(mutex_);
      if (Record* record = find(id)) {
        found = true;
        record->kept = false;
        record->popup = false;
        close_locked(*record, dismissed, signals);
        prune_locked();
        changed_locked();
      }
    }
    emit(signals);
    return found;
  }
  void hide_popup(std::uint32_t id) {
    std::lock_guard lock(mutex_);
    if (Record* record = find(id); record != nullptr && record->popup) {
      record->popup = false;
      changed_locked();
    }
  }
  void hide_popups() {
    std::lock_guard lock(mutex_);
    hide_popups_locked();
  }
  bool invoke(std::uint32_t id, const std::string& key) {
    std::vector<Signal> signals;
    {
      std::lock_guard lock(mutex_);
      Record* record = find(id);
      if (record == nullptr || record->closed || std::ranges::find(record->actions, key, &Action::key) == record->actions.end())
        return false;
      signals.push_back({"ActionInvoked", id, 0, key});
      if (!record->resident) {
        record->kept = false;
        record->popup = false;
        close_locked(*record, dismissed, signals);
        prune_locked();
      }
      changed_locked();
    }
    emit(signals);
    return true;
  }
  bool activate(std::uint32_t id) {
    std::string key;
    {
      std::lock_guard lock(mutex_);
      const Record* record = find(id);
      if (record == nullptr || record->closed) return false;
      if (std::ranges::find(record->actions, "default", &Action::key) != record->actions.end()) key = "default";
      else if (record->actions.size() == 1) key = record->actions.front().key;
      else return false;
    }
    return invoke(id, key);
  }
  void clear() {
    std::vector<Signal> signals;
    {
      std::lock_guard lock(mutex_);
      for (Record& record : records_) {
        record.kept = false;
        record.popup = false;
        close_locked(record, dismissed, signals);
      }
      prune_locked();
      changed_locked();
    }
    emit(signals);
  }
  bool dnd() {
    std::lock_guard lock(mutex_);
    return dnd_;
  }
  void set_dnd(bool enabled) {
    std::lock_guard lock(mutex_);
    if (dnd_ == enabled) return;
    dnd_ = enabled;
    if (enabled) hide_popups_locked();
    changed_locked();
  }

 private:
  struct Cache {
    std::uint64_t generation = ~0ULL;
    std::vector<Notification> items;
  };

  // --- Model, all under mutex_ ---

  Record* find(std::uint32_t id) {
    const auto found = std::ranges::find(records_, id, &Notification::id);
    return found == records_.end() ? nullptr : &*found;
  }
  void changed_locked() {
    ++generation_;
    const std::uint64_t one = 1;
    const auto written = write(event_, &one, sizeof(one));
    (void)written;
  }
  void hide_popups_locked() {
    bool any = false;
    for (Record& record : records_) any = std::exchange(record.popup, false) || any;
    if (any) changed_locked();
  }
  // Queues NotificationClosed; expiry keeps a record in the history.
  void close_locked(Record& record, std::uint32_t reason, std::vector<Signal>& signals) {
    if (record.closed) return;
    record.closed = true;
    record.close_reason = reason;
    record.popup = false;
    if (reason != expired) record.kept = false;
    signals.push_back({"NotificationClosed", record.id, reason, {}});
  }
  // Closed records outside the history are gone, and the history is capped.
  void prune_locked() {
    std::size_t kept = std::ranges::count_if(records_, [](const Record& r) { return r.kept; });
    for (auto record = records_.begin(); kept > history_limit && record != records_.end(); ++record)
      if (record->kept) {
        record->kept = false;
        --kept;
      }
    std::erase_if(records_, [](const Record& record) {
      if (!record.closed || record.kept) return false;
      remove_file(record.owned_image);
      return true;
    });
  }

  // --- D-Bus thread ---

  void emit(const std::vector<Signal>& signals) {
    if (signals.empty()) return;
    std::lock_guard connection_lock(connection_mutex_);
    if (connection_ == nullptr) return;
    for (const Signal& signal : signals) {
      std::vector<Value> body{Value::uint32(signal.id)};
      if (signal.key.empty() && std::string_view(signal.name) == "NotificationClosed")
        body.push_back(Value::uint32(signal.reason));
      else
        body.push_back(Value::string(signal.key));
      connection_->emit(object_path, bus_name, signal.name, std::move(body));
    }
  }

  std::uint32_t notify(const Message& call) {
    if (call.signature() != "susssasa{sv}i") throw std::invalid_argument("Notify takes (susssasa{sv}i)");
    const std::string& app = call.body[0].text;
    const auto replaces = static_cast<std::uint32_t>(call.body[1].bits);
    const std::string& icon = call.body[2].text;
    const std::string& summary = call.body[3].text;
    const std::string& body = call.body[4].text;
    const Value& actions = call.body[5];
    const Hints hints(call.body[6]);
    const auto timeout = static_cast<std::int32_t>(call.body[7].bits);

    Record record;
    record.app_name = app;
    record.app_icon = icon;
    record.summary = summary;
    record.body = body;
    record.image_path = hints.string({"image-path", "image_path"});
    record.desktop_entry = hints.string({"desktop-entry"});
    record.category = hints.string({"category"});
    record.tag = hints.string({"x-canonical-private-synchronous", "x-dunst-stack-tag", "synchronous", "private-synchronous"});
    record.resident = hints.boolean("resident");
    record.transient = hints.boolean("transient");
    record.action_icons = hints.boolean("action-icons");
    if (const auto urgency = hints.integer("urgency")) record.urgency = static_cast<std::uint8_t>(std::clamp<std::int64_t>(*urgency, 0, 2));
    if (const auto value = hints.integer("value")) record.value = static_cast<int>(std::clamp<std::int64_t>(*value, 0, 100));

    record.icon_source = icon_source(record.app_icon);
    if (record.icon_source.empty()) record.icon_source = desktop_icon(record.desktop_entry);
    for (const char* key : {"image-data", "image_data", "icon_data"})
      if (record.owned_image.empty())
        if (const Value* data = hints.get(key)) record.owned_image = write_image(*data);
    record.image_source = record.owned_image.empty() ? icon_source(record.image_path) : record.owned_image;

    const std::size_t count = actions.size();
    if (count % 2 != 0) {
      remove_file(record.owned_image);
      throw std::invalid_argument("notification actions must be key/label pairs");
    }
    for (std::size_t index = 0; index < count; index += 2)
      record.actions.push_back({actions.items[index].text, actions.items[index + 1].text});

    record.expire_timeout = timeout < 0 ? (record.urgency == 2 ? 0 : default_timeout_ms) : timeout;
    record.expires = Clock::now() + std::chrono::milliseconds(record.expire_timeout);
    record.timestamp =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    record.kept = !record.transient;

    std::vector<Signal> signals;
    std::uint32_t id = 0;
    {
      std::lock_guard lock(mutex_);
      record.popup = !dnd_ || record.urgency == 2;
      // What this one replaces: replaces_id, else an open one with its tag.
      Record* previous = replaces != 0 ? find(replaces) : nullptr;
      if (previous == nullptr && !record.tag.empty())
        for (Record& candidate : records_)
          if (!candidate.closed && candidate.tag == record.tag && candidate.app_name == record.app_name)
            previous = &candidate;
      if (previous != nullptr) {
        record.id = previous->id;
        remove_file(previous->owned_image);
        std::erase_if(records_, [id = record.id](const Record& r) { return r.id == id; });
      } else if (replaces != 0) {
        // The spec returns replaces_id even when it names nothing (any more).
        record.id = replaces;
      } else {
        do record.id = next_id_++;
        while (record.id == 0 || find(record.id) != nullptr);
      }
      id = record.id;
      records_.push_back(std::move(record));
      // Too many open: the oldest goes.
      if (static_cast<std::size_t>(std::ranges::count_if(records_, [](const Record& r) { return !r.closed; })) > open_limit)
        for (Record& oldest : records_)
          if (!oldest.closed) {
            close_locked(oldest, evicted, signals);
            break;
          }
      prune_locked();
      changed_locked();
    }
    emit(signals);
    reset_expiry();
    return id;
  }

  // One timer for the earliest deadline among open records.
  void reset_expiry() {
    if (expiry_ != 0) {
      loop_.cancel(expiry_);
      expiry_ = 0;
    }
    auto next = Clock::time_point::max();
    {
      std::lock_guard lock(mutex_);
      for (const Record& record : records_)
        if (!record.closed && record.expire_timeout > 0) next = std::min(next, record.expires);
    }
    if (next == Clock::time_point::max()) return;
    const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(next - Clock::now()).count();
    expiry_ = loop_.after(std::chrono::milliseconds(std::max<std::int64_t>(delay + 1, 1)), [this] {
      expiry_ = 0;
      std::vector<Signal> signals;
      {
        std::lock_guard lock(mutex_);
        const auto now = Clock::now();
        for (Record& record : records_)
          if (!record.closed && record.expire_timeout > 0 && record.expires <= now)
            close_locked(record, expired, signals);
        if (!signals.empty()) {
          prune_locked();
          changed_locked();
        }
      }
      emit(signals);
      reset_expiry();
    });
  }

  void close_by_sender(std::uint32_t id) {
    std::vector<Signal> signals;
    {
      std::lock_guard lock(mutex_);
      if (Record* record = find(id); record != nullptr && !record->closed) {
        close_locked(*record, withdrawn, signals);
        prune_locked();
        changed_locked();
      }
    }
    emit(signals);
  }

  dbus::Reply method_call(const Message& call) {
    const std::string_view name(call.member);
    if (name == "GetCapabilities")
      return {{Value::strings({"actions", "body", "body-markup", "icon-static", "persistence"})}, {}, {}};
    if (name == "GetServerInformation")
      return {{Value::string("vw"), Value::string("vw"), Value::string(VW_VERSION), Value::string("1.2")}, {}, {}};
    if (name == "Notify") return {{Value::uint32(notify(call))}, {}, {}};
    if (name == "CloseNotification") {
      const Value* id = call.arg(0);
      if (id == nullptr || !id->is("u")) throw std::invalid_argument("CloseNotification takes (u)");
      close_by_sender(static_cast<std::uint32_t>(id->bits));
      return {};
    }
    return dbus::Reply::error("org.freedesktop.DBus.Error.UnknownMethod", "unknown notification method");
  }

  // A private connection, so closing it drops the name with nothing else.
  void run(const std::stop_token& stop) {
    bool owned = false;
    std::unique_ptr<dbus::Connection> connection;
    try {
      connection = std::make_unique<dbus::Connection>(dbus::Bus::session, loop_);
      connection->export_object(object_path, {bus_name, interface_xml,
                                              [this](const Message& call) { return method_call(call); }, {}});
      // DO_NOT_QUEUE: fail at once when another daemon has the name.
      const std::uint32_t result = connection->request_name(bus_name, 4U);
      if (result == 0) throw std::runtime_error("could not acquire notification name");
      if (result != 1 && result != 4)
        throw std::runtime_error("another notification daemon already owns org.freedesktop.Notifications");
      owned = true;
      connection->on_closed([this] {
        {
          std::lock_guard lock(mutex_);
          running_ = false;
          error_ = "notification D-Bus connection closed";
          changed_locked();
        }
        loop_.quit();
      });
      {
        std::lock_guard connection_lock(connection_mutex_);
        connection_ = connection.get();
      }
      {
        std::lock_guard lock(mutex_);
        running_ = true;
        ready_ = true;
        changed_locked();
      }
      ready_condition_.notify_all();
      if (!stop.stop_requested()) loop_.run();
    } catch (const std::exception& error) {
      std::lock_guard lock(mutex_);
      error_ = error.what();
    }
    if (expiry_ != 0) {
      loop_.cancel(expiry_);
      expiry_ = 0;
    }
    {
      std::lock_guard connection_lock(connection_mutex_);
      connection_ = nullptr;
    }
    {
      std::lock_guard lock(mutex_);
      running_ = false;
    }
    if (connection != nullptr) {
      if (owned && connection->connected()) connection->release_name(bus_name);
      connection->unexport_object(object_path);
      connection.reset();
    }
    {
      std::lock_guard lock(mutex_);
      ready_ = true;
      changed_locked();
    }
    ready_condition_.notify_all();
  }

  const int event_;
  dbus::Loop loop_;
  std::uint64_t expiry_ = 0;  // D-Bus thread only
  std::jthread thread_;

  std::mutex mutex_;
  std::condition_variable ready_condition_;
  std::mutex connection_mutex_;
  dbus::Connection* connection_ = nullptr;
  std::vector<Record> records_;  // oldest first
  std::uint32_t next_id_ = 1;
  std::uint64_t generation_ = 0;
  std::string error_;
  bool started_ = false;
  bool ready_ = false;
  bool running_ = false;
  bool dnd_ = false;

  // Reader side.
  std::mutex cache_mutex_;
  std::array<Cache, 3> caches_;
  std::unordered_map<std::string, std::string> resolved_;
};

Service& service() {
  static Service instance;
  return instance;
}

}  // namespace

StartResult start() { return service().start(); }
bool running() { return service().running(); }
std::vector<Notification> list() { return service().snapshot(View::open); }
std::vector<Notification> popups() { return service().snapshot(View::popups); }
std::vector<Notification> history() { return service().snapshot(View::history); }
std::uint64_t generation() { return service().generation(); }
bool dismiss(std::uint32_t id) { return service().dismiss(id); }
void hide_popup(std::uint32_t id) { service().hide_popup(id); }
void hide_popups() { service().hide_popups(); }
bool invoke(std::uint32_t id, const std::string& action_key) { return service().invoke(id, action_key); }
bool activate(std::uint32_t id) { return service().activate(id); }
void clear() { service().clear(); }
bool do_not_disturb() { return service().dnd(); }
void set_do_not_disturb(bool enabled) { service().set_dnd(enabled); }
int event_fd() { return service().fd(); }
bool drain_events() { return service().drain(); }

}  // namespace vw::notifications
