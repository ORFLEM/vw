#pragma once

// vw's own D-Bus: the wire format, SASL EXTERNAL authentication, method
// calls (blocking from any thread, or with a callback), signal matching,
// name ownership and exported objects, plus the small event loop the system
// services run on. Internal; not installed.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vw::dbus {

// A D-Bus value of one complete type. Numbers keep their bits in `bits`
// (doubles too); s, o and g keep their text, and so does ay; arrays,
// structs, dict entries and variants keep their parts in `items` (a variant
// holds exactly one).
struct Value {
  std::string type;
  std::uint64_t bits = 0;
  std::string text;
  std::vector<Value> items;

  static Value boolean(bool value);
  static Value byte(std::uint8_t value);
  static Value int16(std::int16_t value);
  static Value uint16(std::uint16_t value);
  static Value int32(std::int32_t value);
  static Value uint32(std::uint32_t value);
  static Value int64(std::int64_t value);
  static Value uint64(std::uint64_t value);
  static Value number(double value);
  static Value string(std::string_view value);
  static Value object_path(std::string_view value);
  static Value signature(std::string_view value);
  static Value bytes(std::string_view value);
  static Value variant(Value inner);
  static Value array(std::string element_type, std::vector<Value> elements = {});
  static Value strings(const std::vector<std::string>& values, std::string_view element_type = "s");
  static Value structure(std::vector<Value> fields);
  // a{<key><value>} from key/value pairs.
  static Value dict(std::string_view key_type, std::string_view value_type,
                    std::vector<std::pair<Value, Value>> entries = {});

  bool is(std::string_view signature) const { return type == signature; }
  // The inside of a variant, or this value.
  const Value& unwrap() const;
  // s, o, g and ay; empty otherwise.
  std::string_view str() const;
  // Any integer type.
  std::optional<std::int64_t> integer() const;
  bool flag(bool fallback = false) const;
  double real(double fallback = 0) const;
  std::size_t size() const { return items.size(); }
  const Value* at(std::size_t index) const { return index < items.size() ? &items[index] : nullptr; }
  // The value stored under key in a dictionary with string keys, a variant
  // unwrapped; null when missing.
  const Value* find(std::string_view key) const;
  // as / ao contents.
  std::vector<std::string> string_list() const;
};

// Dictionary helpers that tolerate a null dictionary.
std::string get_string(const Value* dictionary, std::string_view key);
bool get_bool(const Value* dictionary, std::string_view key, bool fallback = false);
double get_double(const Value* dictionary, std::string_view key, double fallback = 0);
std::int64_t get_int(const Value* dictionary, std::string_view key, std::int64_t fallback = 0);
std::vector<std::string> get_strings(const Value* dictionary, std::string_view key);

bool valid_object_path(std::string_view path);

struct Message {
  enum class Type : std::uint8_t { invalid = 0, call = 1, reply = 2, error = 3, signal = 4 };
  static constexpr std::uint8_t no_reply_expected = 1;

  Type type = Type::invalid;
  std::uint8_t flags = 0;
  std::uint32_t serial = 0;
  std::uint32_t reply_serial = 0;
  std::string path;
  std::string interface;
  std::string member;
  std::string error_name;
  std::string destination;
  std::string sender;
  std::vector<Value> body;

  static Message method_call(std::string destination, std::string path, std::string interface,
                             std::string member, std::vector<Value> body = {});
  static Message signal(std::string path, std::string interface, std::string member,
                        std::vector<Value> body = {});

  const Value* arg(std::size_t index) const { return index < body.size() ? &body[index] : nullptr; }
  // The concatenated signature of the body.
  std::string signature() const;
};

// Tasks and timers on one thread.
class Loop {
 public:
  using Task = std::function<void()>;
  using Clock = std::chrono::steady_clock;

  void post(Task task);
  // Runs task after delay; the id cancels it.
  std::uint64_t after(std::chrono::milliseconds delay, Task task);
  void cancel(std::uint64_t timer);
  // Runs until quit(). The thread calling it becomes the loop thread.
  void run();
  void quit();
  bool in_loop() const { return running_thread_ == std::this_thread::get_id(); }

 private:
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Task> tasks_;
  std::multimap<Clock::time_point, std::pair<std::uint64_t, Task>> timers_;
  std::uint64_t next_timer_ = 1;
  bool quit_ = false;
  std::thread::id running_thread_;
};

enum class Bus { session, system };

// A signal filter, sent to the bus as a match rule and checked locally.
// sender may be a well-known name; the connection follows its owner.
struct Match {
  std::string sender;
  std::string interface;
  std::string member;
  std::string path;
  std::string arg0;
  std::string arg0_namespace;

  std::string rule() const;
};

// A method reply body, or an error.
struct Reply {
  std::vector<Value> body;
  std::string error_name;
  std::string error_message;

  static Reply error(std::string name, std::string message) { return {{}, std::move(name), std::move(message)}; }
};

// One interface on an exported object. Properties are read-only.
struct Exported {
  std::string interface;
  // <method>, <property> and <signal> elements, for Introspect.
  std::string introspection;
  std::function<Reply(const Message& call)> call;
  std::function<std::vector<std::pair<std::string, Value>>()> properties;
};

class Connection {
 public:
  using Callback = std::function<void(const Message* reply, const std::string& error)>;

  // Connects, authenticates and registers with the bus. Incoming signals,
  // method calls and asynchronous replies run on `loop`. Throws on failure.
  Connection(Bus bus, Loop& loop);
  ~Connection();
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  const std::string& unique_name() const { return unique_name_; }
  bool connected() const;

  // Blocks until the reply, from any thread (the loop keeps running). Null
  // on an error reply, timeout or lost connection; error receives why.
  std::optional<Message> call(Message message, int timeout_ms = 1000, std::string* error = nullptr);
  // The reply's first body value, or null.
  std::optional<Value> call_value(Message message, int timeout_ms = 1000);
  // Callback runs on the loop. Without one, no reply is asked for.
  void call_async(Message message, int timeout_ms = 1000, Callback callback = {});
  // Signals and replies.
  void send(Message message);
  void emit(std::string path, std::string interface, std::string member, std::vector<Value> body = {});

  std::uint64_t subscribe(Match match, std::function<void(const Message&)> handler);
  void unsubscribe(std::uint64_t id);
  // appeared(owner) when name gets an owner (also at once when it has one),
  // vanished() when it loses it. Both run on the loop.
  std::uint64_t watch_name(std::string name, std::function<void(const std::string& owner)> appeared,
                           std::function<void()> vanished);
  void unwatch_name(std::uint64_t id);
  // org.freedesktop.DBus.RequestName; the bus's answer (1 primary owner,
  // 2 queued, 3 exists, 4 already owner) or 0 on failure.
  std::uint32_t request_name(std::string_view name, std::uint32_t flags);
  void release_name(std::string_view name);

  void export_object(std::string path, Exported object);
  void unexport_object(const std::string& path);
  // Runs on the loop when the connection drops.
  void on_closed(std::function<void()> handler);

 private:
  struct Pending {
    bool done = false;
    std::optional<Message> reply;
    std::string error;
    Callback callback;
    std::uint64_t timer = 0;
  };
  struct Subscription {
    Match match;
    std::function<void(const Message&)> handler;
  };

  void write(Message& message);
  void reader();
  void incoming(Message message);
  void dispatch_signal(const Message& message);
  void dispatch_call(const Message& message);
  bool matches(const Match& match, const Message& message);
  void add_match(const std::string& rule);
  void remove_match(const std::string& rule);
  void follow_owner(const std::string& name);
  void fail_pending(const std::string& reason);

  Loop& loop_;
  int socket_ = -1;
  std::string unique_name_;
  std::thread reader_;
  std::mutex write_mutex_;
  mutable std::mutex mutex_;
  std::condition_variable replies_;
  std::uint32_t next_serial_ = 1;
  bool closed_ = false;
  std::unordered_map<std::uint32_t, std::shared_ptr<Pending>> pending_;
  std::map<std::uint64_t, Subscription> subscriptions_;
  std::uint64_t next_subscription_ = 1;
  // Owners of the well-known names that subscriptions name as sender.
  std::map<std::string, std::string> owners_;
  std::map<std::string, int> followed_;
  std::map<std::string, std::vector<Exported>> objects_;
  std::vector<std::function<void()>> closed_handlers_;
  // Shared with tasks posted to the loop, which may outlive the connection.
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

// A loop on its own thread with one bus connection that keeps a service's
// state fresh. Signals from a watched name, and the name appearing or
// vanishing, schedule a debounced refresh on that thread; after every
// refresh the event fd becomes readable so a vw::App rebuilds.
class Monitor {
 public:
  using Refresh = std::function<void(Connection*)>;
  using Setup = std::function<void(Connection*)>;

  // setup runs once on the monitor thread before the first refresh, e.g. to
  // own names or export objects.
  Monitor(Bus bus, Refresh refresh, Setup setup = {}, unsigned debounce_ms = 60);
  ~Monitor();
  Monitor(const Monitor&) = delete;
  Monitor& operator=(const Monitor&) = delete;

  // Refresh on every signal from name. Only before start().
  void watch(std::string name);
  // Starts the thread and waits for the first refresh. Idempotent.
  void start();
  // The bus connection, or null when the bus is unreachable.
  Connection* connection();
  Loop& loop() { return loop_; }
  int fd() const { return event_; }
  bool drain();
  void wake();
  void schedule_refresh();
  // Runs task on the monitor thread (immediately when already on it).
  void invoke(std::function<void()> task);

 private:
  void run();

  Bus bus_;
  Refresh refresh_;
  Setup setup_;
  unsigned debounce_ms_;
  int event_ = -1;
  Loop loop_;
  std::unique_ptr<Connection> connection_;
  std::vector<std::string> names_;
  bool pending_ = false;
  std::mutex mutex_;
  std::condition_variable ready_condition_;
  bool started_ = false;
  bool ready_ = false;
  std::thread thread_;
};

}  // namespace vw::dbus
