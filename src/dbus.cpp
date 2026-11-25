#include "dbus.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <span>
#include <stdexcept>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace vw::dbus {
namespace {

constexpr std::size_t max_message = 128U << 20U;
constexpr int max_depth = 64;
constexpr const char* bus_name = "org.freedesktop.DBus";
constexpr const char* bus_path = "/org/freedesktop/DBus";

[[noreturn]] void malformed(const char* what) { throw std::runtime_error(std::string("malformed D-Bus data: ") + what); }

// The end of the complete type starting at pos.
std::size_t type_end(std::string_view signature, std::size_t pos, int depth = 0) {
  if (pos >= signature.size() || depth > max_depth) malformed("signature");
  const char c = signature[pos];
  if (c == 'a') return type_end(signature, pos + 1, depth + 1);
  if (c == '(' || c == '{') {
    const char close = c == '(' ? ')' : '}';
    std::size_t p = pos + 1;
    while (p < signature.size() && signature[p] != close) p = type_end(signature, p, depth + 1);
    if (p >= signature.size()) malformed("signature");
    return p + 1;
  }
  if (std::string_view("ybnqiuxtdsoghv").find(c) == std::string_view::npos) malformed("signature");
  return pos + 1;
}

std::size_t alignment(char c) {
  switch (c) {
    case 'y': case 'g': case 'v': return 1;
    case 'n': case 'q': return 2;
    case 'x': case 't': case 'd': case '(': case '{': return 8;
    default: return 4;
  }
}

class Writer {
 public:
  std::vector<std::uint8_t> data;

  void pad(std::size_t to) {
    while (data.size() % to != 0) data.push_back(0);
  }
  void u8(std::uint8_t v) { data.push_back(v); }
  void u16(std::uint16_t v) {
    pad(2);
    data.push_back(static_cast<std::uint8_t>(v));
    data.push_back(static_cast<std::uint8_t>(v >> 8U));
  }
  void u32(std::uint32_t v) {
    pad(4);
    for (int i = 0; i < 4; ++i) data.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
  }
  void u64(std::uint64_t v) {
    pad(8);
    for (int i = 0; i < 8; ++i) data.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
  }
  void text(std::string_view s) {
    u32(static_cast<std::uint32_t>(s.size()));
    data.insert(data.end(), s.begin(), s.end());
    data.push_back(0);
  }
  void signature(std::string_view s) {
    if (s.size() > 255) malformed("signature too long");
    u8(static_cast<std::uint8_t>(s.size()));
    data.insert(data.end(), s.begin(), s.end());
    data.push_back(0);
  }
  void value(const Value& v, int depth = 0) {
    if (v.type.empty() || depth > max_depth) malformed("value");
    switch (v.type[0]) {
      case 'y': u8(static_cast<std::uint8_t>(v.bits)); break;
      case 'b': u32(v.bits != 0 ? 1 : 0); break;
      case 'n': case 'q': u16(static_cast<std::uint16_t>(v.bits)); break;
      case 'i': case 'u': case 'h': u32(static_cast<std::uint32_t>(v.bits)); break;
      case 'x': case 't': case 'd': u64(v.bits); break;
      case 's': case 'o': text(v.text); break;
      case 'g': signature(v.text); break;
      case 'v':
        if (v.items.size() != 1) malformed("variant");
        signature(v.items[0].type);
        value(v.items[0], depth + 1);
        break;
      case '(': case '{':
        pad(8);
        for (const Value& item : v.items) value(item, depth + 1);
        break;
      case 'a': {
        pad(4);
        const std::size_t at = data.size();
        u32(0);
        pad(alignment(v.type[1]));
        const std::size_t start = data.size();
        if (v.type == "ay" && v.items.empty()) {
          data.insert(data.end(), v.text.begin(), v.text.end());
        } else {
          for (const Value& item : v.items) value(item, depth + 1);
        }
        const auto length = static_cast<std::uint32_t>(data.size() - start);
        for (int i = 0; i < 4; ++i) data[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(length >> (8 * i));
        break;
      }
      default: malformed("type");
    }
  }
};

class Parser {
 public:
  Parser(std::span<const std::uint8_t> data, bool little) : data_(data), little_(little) {}
  std::size_t pos = 0;

  void align(std::size_t to) {
    pos = (pos + to - 1) / to * to;
    if (pos > data_.size()) malformed("truncated");
  }
  const std::uint8_t* take(std::size_t n) {
    if (n > data_.size() - pos) malformed("truncated");
    const std::uint8_t* p = data_.data() + pos;
    pos += n;
    return p;
  }
  std::uint64_t number(std::size_t bytes) {
    align(bytes);
    const std::uint8_t* p = take(bytes);
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < bytes; ++i) {
      const std::size_t shift = little_ ? i : bytes - 1 - i;
      v |= static_cast<std::uint64_t>(p[i]) << (8 * shift);
    }
    return v;
  }
  std::string text() {
    const auto length = static_cast<std::size_t>(number(4));
    const std::uint8_t* p = take(length + 1);
    return std::string(reinterpret_cast<const char*>(p), length);
  }
  std::string signature() {
    const std::size_t length = *take(1);
    const std::uint8_t* p = take(length + 1);
    return std::string(reinterpret_cast<const char*>(p), length);
  }
  Value value(std::string_view type, int depth = 0) {
    if (type.empty() || depth > max_depth) malformed("value");
    Value v;
    v.type = std::string(type);
    switch (type[0]) {
      case 'y': v.bits = *take(1); break;
      case 'b': v.bits = number(4) != 0 ? 1 : 0; break;
      case 'n': v.bits = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int16_t>(number(2)))); break;
      case 'q': v.bits = number(2); break;
      case 'i': v.bits = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(number(4)))); break;
      case 'u': case 'h': v.bits = number(4); break;
      case 'x': case 't': case 'd': v.bits = number(8); break;
      case 's': case 'o': v.text = text(); break;
      case 'g': v.text = signature(); break;
      case 'v': {
        const std::string inner = signature();
        if (type_end(inner, 0) != inner.size()) malformed("variant signature");
        v.items.push_back(value(inner, depth + 1));
        break;
      }
      case '(': case '{': {
        align(8);
        std::size_t p = 1;
        while (p < type.size() - 1) {
          const std::size_t end = type_end(type, p);
          v.items.push_back(value(type.substr(p, end - p), depth + 1));
          p = end;
        }
        break;
      }
      case 'a': {
        const auto length = static_cast<std::size_t>(number(4));
        if (length > max_message) malformed("array length");
        const std::string_view element = type.substr(1);
        align(alignment(element[0]));
        const std::size_t end = pos + length;
        if (end > data_.size()) malformed("truncated");
        if (element == "y") {
          v.text.assign(reinterpret_cast<const char*>(take(length)), length);
          break;
        }
        while (pos < end) v.items.push_back(value(element, depth + 1));
        if (pos != end) malformed("array length");
        break;
      }
      default: malformed("type");
    }
    return v;
  }

 private:
  std::span<const std::uint8_t> data_;
  bool little_;
};

std::vector<std::uint8_t> serialize(const Message& message) {
  Writer body;
  for (const Value& value : message.body) body.value(value);
  Writer header;
  header.u8('l');
  header.u8(static_cast<std::uint8_t>(message.type));
  header.u8(message.flags);
  header.u8(1);
  header.u32(static_cast<std::uint32_t>(body.data.size()));
  header.u32(message.serial);
  std::vector<Value> fields;
  const auto field = [&](std::uint8_t code, Value value) {
    fields.push_back(Value::structure({Value::byte(code), Value::variant(std::move(value))}));
  };
  if (!message.path.empty()) field(1, Value::object_path(message.path));
  if (!message.interface.empty()) field(2, Value::string(message.interface));
  if (!message.member.empty()) field(3, Value::string(message.member));
  if (!message.error_name.empty()) field(4, Value::string(message.error_name));
  if (message.reply_serial != 0) field(5, Value::uint32(message.reply_serial));
  if (!message.destination.empty()) field(6, Value::string(message.destination));
  if (!message.body.empty()) field(8, Value::signature(message.signature()));
  header.value(Value::array("(yv)", std::move(fields)));
  header.pad(8);
  header.data.insert(header.data.end(), body.data.begin(), body.data.end());
  return std::move(header.data);
}

Message parse(std::span<const std::uint8_t> data) {
  if (data.size() < 16 || (data[0] != 'l' && data[0] != 'B')) malformed("header");
  Parser parser(data, data[0] == 'l');
  parser.pos = 1;
  Message message;
  message.type = static_cast<Message::Type>(*parser.take(1));
  message.flags = *parser.take(1);
  parser.take(1);
  const auto body_length = static_cast<std::size_t>(parser.number(4));
  message.serial = static_cast<std::uint32_t>(parser.number(4));
  const Value fields = parser.value("a(yv)");
  std::string signature;
  for (const Value& field : fields.items) {
    const auto code = field.items[0].bits;
    const Value& value = field.items[1].items[0];
    switch (code) {
      case 1: message.path = value.text; break;
      case 2: message.interface = value.text; break;
      case 3: message.member = value.text; break;
      case 4: message.error_name = value.text; break;
      case 5: message.reply_serial = static_cast<std::uint32_t>(value.bits); break;
      case 6: message.destination = value.text; break;
      case 7: message.sender = value.text; break;
      case 8: signature = value.text; break;
      default: break;
    }
  }
  parser.align(8);
  if (data.size() - parser.pos != body_length) malformed("body length");
  std::size_t p = 0;
  while (p < signature.size()) {
    const std::size_t end = type_end(signature, p);
    message.body.push_back(parser.value(std::string_view(signature).substr(p, end - p)));
    p = end;
  }
  return message;
}

bool write_all(int fd, const void* data, std::size_t size) {
  const auto* p = static_cast<const std::uint8_t*>(data);
  while (size > 0) {
    const ssize_t written = ::send(fd, p, size, MSG_NOSIGNAL);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) return false;
    p += written;
    size -= static_cast<std::size_t>(written);
  }
  return true;
}

bool read_all(int fd, void* data, std::size_t size) {
  auto* p = static_cast<std::uint8_t*>(data);
  while (size > 0) {
    const ssize_t got = ::recv(fd, p, size, 0);
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) return false;
    p += got;
    size -= static_cast<std::size_t>(got);
  }
  return true;
}

std::string unescape_address(std::string_view value) {
  std::string out;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '%' && i + 2 < value.size()) {
      out.push_back(static_cast<char>(std::stoi(std::string(value.substr(i + 1, 2)), nullptr, 16)));
      i += 2;
    } else {
      out.push_back(value[i]);
    }
  }
  return out;
}

// unix:path=... or unix:abstract=..., the first of a ;-separated list that
// connects.
int connect_address(std::string_view addresses) {
  std::size_t start = 0;
  while (start <= addresses.size()) {
    std::size_t end = addresses.find(';', start);
    if (end == std::string_view::npos) end = addresses.size();
    const std::string_view address = addresses.substr(start, end - start);
    start = end + 1;
    if (!address.starts_with("unix:")) continue;
    std::string path;
    bool abstract = false;
    std::size_t p = 5;
    while (p < address.size()) {
      std::size_t comma = address.find(',', p);
      if (comma == std::string_view::npos) comma = address.size();
      const std::string_view pair = address.substr(p, comma - p);
      p = comma + 1;
      const std::size_t equals = pair.find('=');
      if (equals == std::string_view::npos) continue;
      const std::string_view key = pair.substr(0, equals);
      if (key == "path" || key == "abstract") {
        path = unescape_address(pair.substr(equals + 1));
        abstract = key == "abstract";
      }
    }
    if (path.empty()) continue;
    sockaddr_un socket_address{};
    socket_address.sun_family = AF_UNIX;
    const std::size_t offset = abstract ? 1 : 0;
    if (path.size() + offset >= sizeof(socket_address.sun_path)) continue;
    std::memcpy(socket_address.sun_path + offset, path.data(), path.size());
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) continue;
    const auto length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + offset + path.size() + (abstract ? 0 : 1));
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&socket_address), length) == 0) return fd;
    ::close(fd);
  }
  return -1;
}

std::string bus_address(Bus bus) {
  if (bus == Bus::system) {
    const char* address = std::getenv("DBUS_SYSTEM_BUS_ADDRESS");
    return address != nullptr && *address != '\0' ? address : "unix:path=/var/run/dbus/system_bus_socket";
  }
  if (const char* address = std::getenv("DBUS_SESSION_BUS_ADDRESS"); address != nullptr && *address != '\0')
    return address;
  if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime != nullptr && *runtime != '\0')
    return std::string("unix:path=") + runtime + "/bus";
  throw std::runtime_error("no session D-Bus address (DBUS_SESSION_BUS_ADDRESS is unset)");
}

std::string read_line(int fd) {
  std::string line;
  char c = 0;
  while (line.size() < 4096) {
    if (!read_all(fd, &c, 1)) throw std::runtime_error("D-Bus authentication failed: connection closed");
    if (c == '\n') break;
    if (c != '\r') line.push_back(c);
  }
  return line;
}

void authenticate(int fd) {
  const std::string uid = std::to_string(getuid());
  std::string hex;
  for (const char c : uid) {
    constexpr const char* digits = "0123456789abcdef";
    hex.push_back(digits[(static_cast<unsigned char>(c) >> 4U) & 15U]);
    hex.push_back(digits[static_cast<unsigned char>(c) & 15U]);
  }
  const std::string request = std::string(1, '\0') + "AUTH EXTERNAL " + hex + "\r\n";
  if (!write_all(fd, request.data(), request.size())) throw std::runtime_error("D-Bus authentication failed");
  const std::string answer = read_line(fd);
  if (!answer.starts_with("OK")) throw std::runtime_error("D-Bus authentication was refused: " + answer);
  constexpr std::string_view begin = "BEGIN\r\n";
  if (!write_all(fd, begin.data(), begin.size())) throw std::runtime_error("D-Bus authentication failed");
}

std::string error_text(const Message& message) {
  std::string text = message.error_name;
  if (const Value* first = message.arg(0); first != nullptr && first->is("s")) text += ": " + first->text;
  return text;
}

}  // namespace

// ---------------------------------------------------------------------------
// Value

Value Value::boolean(bool value) { return {"b", value ? 1U : 0U, {}, {}}; }
Value Value::byte(std::uint8_t value) { return {"y", value, {}, {}}; }
Value Value::int16(std::int16_t value) { return {"n", static_cast<std::uint64_t>(static_cast<std::int64_t>(value)), {}, {}}; }
Value Value::uint16(std::uint16_t value) { return {"q", value, {}, {}}; }
Value Value::int32(std::int32_t value) { return {"i", static_cast<std::uint64_t>(static_cast<std::int64_t>(value)), {}, {}}; }
Value Value::uint32(std::uint32_t value) { return {"u", value, {}, {}}; }
Value Value::int64(std::int64_t value) { return {"x", static_cast<std::uint64_t>(value), {}, {}}; }
Value Value::uint64(std::uint64_t value) { return {"t", value, {}, {}}; }
Value Value::number(double value) { return {"d", std::bit_cast<std::uint64_t>(value), {}, {}}; }
Value Value::string(std::string_view value) { return {"s", 0, std::string(value), {}}; }
Value Value::object_path(std::string_view value) { return {"o", 0, std::string(value), {}}; }
Value Value::signature(std::string_view value) { return {"g", 0, std::string(value), {}}; }
Value Value::bytes(std::string_view value) { return {"ay", 0, std::string(value), {}}; }
Value Value::variant(Value inner) { return {"v", 0, {}, {std::move(inner)}}; }
Value Value::array(std::string element_type, std::vector<Value> elements) {
  return {"a" + element_type, 0, {}, std::move(elements)};
}
Value Value::strings(const std::vector<std::string>& values, std::string_view element_type) {
  std::vector<Value> items;
  for (const std::string& value : values) items.push_back({std::string(element_type), 0, value, {}});
  return array(std::string(element_type), std::move(items));
}
Value Value::structure(std::vector<Value> fields) {
  std::string type = "(";
  for (const Value& field : fields) type += field.type;
  type += ")";
  return {std::move(type), 0, {}, std::move(fields)};
}
Value Value::dict(std::string_view key_type, std::string_view value_type,
                  std::vector<std::pair<Value, Value>> entries) {
  const std::string entry_type = "{" + std::string(key_type) + std::string(value_type) + "}";
  std::vector<Value> items;
  for (auto& [key, value] : entries) items.push_back({entry_type, 0, {}, {std::move(key), std::move(value)}});
  return array(entry_type, std::move(items));
}

const Value& Value::unwrap() const {
  const Value* value = this;
  for (int depth = 0; value->type == "v" && value->items.size() == 1 && depth < max_depth; ++depth)
    value = &value->items[0];
  return *value;
}

std::string_view Value::str() const {
  if (type == "s" || type == "o" || type == "g" || type == "ay") return text;
  return {};
}

std::optional<std::int64_t> Value::integer() const {
  if (type.size() != 1) return std::nullopt;
  switch (type[0]) {
    case 'y': case 'q': case 'u': case 't': case 'n': case 'i': case 'x': case 'h':
      return static_cast<std::int64_t>(bits);
    default: return std::nullopt;
  }
}

bool Value::flag(bool fallback) const { return type == "b" ? bits != 0 : fallback; }

double Value::real(double fallback) const {
  if (type == "d") return std::bit_cast<double>(bits);
  if (const auto value = integer()) return static_cast<double>(*value);
  return fallback;
}

const Value* Value::find(std::string_view key) const {
  const Value& self = unwrap();
  if (self.type.size() < 4 || self.type[0] != 'a' || self.type[1] != '{') return nullptr;
  for (const Value& entry : self.items)
    if (entry.items.size() == 2 && entry.items[0].text == key) return &entry.items[1].unwrap();
  return nullptr;
}

std::vector<std::string> Value::string_list() const {
  const Value& self = unwrap();
  std::vector<std::string> result;
  if (self.type != "as" && self.type != "ao" && self.type != "ag") return result;
  for (const Value& item : self.items) result.push_back(item.text);
  return result;
}

std::string get_string(const Value* dictionary, std::string_view key) {
  const Value* value = dictionary == nullptr ? nullptr : dictionary->find(key);
  return value == nullptr ? std::string() : std::string(value->str());
}

bool get_bool(const Value* dictionary, std::string_view key, bool fallback) {
  const Value* value = dictionary == nullptr ? nullptr : dictionary->find(key);
  return value == nullptr ? fallback : value->flag(fallback);
}

double get_double(const Value* dictionary, std::string_view key, double fallback) {
  const Value* value = dictionary == nullptr ? nullptr : dictionary->find(key);
  return value == nullptr ? fallback : value->real(fallback);
}

std::int64_t get_int(const Value* dictionary, std::string_view key, std::int64_t fallback) {
  const Value* value = dictionary == nullptr ? nullptr : dictionary->find(key);
  return value == nullptr ? fallback : value->integer().value_or(fallback);
}

std::vector<std::string> get_strings(const Value* dictionary, std::string_view key) {
  const Value* value = dictionary == nullptr ? nullptr : dictionary->find(key);
  return value == nullptr ? std::vector<std::string>{} : value->string_list();
}

bool valid_object_path(std::string_view path) {
  if (path.empty() || path[0] != '/') return false;
  if (path.size() == 1) return true;
  if (path.back() == '/') return false;
  char previous = '/';
  for (std::size_t i = 1; i < path.size(); ++i) {
    const char c = path[i];
    if (c == '/') {
      if (previous == '/') return false;
    } else if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '_') {
      return false;
    }
    previous = c;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Message

Message Message::method_call(std::string destination, std::string path, std::string interface,
                             std::string member, std::vector<Value> body) {
  Message message;
  message.type = Type::call;
  message.destination = std::move(destination);
  message.path = std::move(path);
  message.interface = std::move(interface);
  message.member = std::move(member);
  message.body = std::move(body);
  return message;
}

Message Message::signal(std::string path, std::string interface, std::string member, std::vector<Value> body) {
  Message message;
  message.type = Type::signal;
  message.path = std::move(path);
  message.interface = std::move(interface);
  message.member = std::move(member);
  message.body = std::move(body);
  return message;
}

std::string Message::signature() const {
  std::string result;
  for (const Value& value : body) result += value.type;
  return result;
}

// ---------------------------------------------------------------------------
// Loop

void Loop::post(Task task) {
  {
    std::lock_guard lock(mutex_);
    tasks_.push_back(std::move(task));
  }
  wake_.notify_one();
}

std::uint64_t Loop::after(std::chrono::milliseconds delay, Task task) {
  std::uint64_t id = 0;
  {
    std::lock_guard lock(mutex_);
    id = next_timer_++;
    timers_.emplace(Clock::now() + delay, std::pair{id, std::move(task)});
  }
  wake_.notify_one();
  return id;
}

void Loop::cancel(std::uint64_t timer) {
  std::lock_guard lock(mutex_);
  for (auto it = timers_.begin(); it != timers_.end(); ++it)
    if (it->second.first == timer) {
      timers_.erase(it);
      return;
    }
}

void Loop::run() {
  running_thread_ = std::this_thread::get_id();
  std::unique_lock lock(mutex_);
  while (!quit_) {
    if (!tasks_.empty()) {
      Task task = std::move(tasks_.front());
      tasks_.pop_front();
      lock.unlock();
      task();
      lock.lock();
      continue;
    }
    if (!timers_.empty() && timers_.begin()->first <= Clock::now()) {
      Task task = std::move(timers_.begin()->second.second);
      timers_.erase(timers_.begin());
      lock.unlock();
      task();
      lock.lock();
      continue;
    }
    if (timers_.empty()) wake_.wait(lock);
    else wake_.wait_until(lock, timers_.begin()->first);
  }
  quit_ = false;
  running_thread_ = {};
}

void Loop::quit() {
  {
    std::lock_guard lock(mutex_);
    quit_ = true;
  }
  wake_.notify_one();
}

// ---------------------------------------------------------------------------
// Match

std::string Match::rule() const {
  std::string rule = "type='signal'";
  const auto add = [&rule](const char* key, const std::string& value) {
    if (!value.empty()) rule += std::string(",") + key + "='" + value + "'";
  };
  add("sender", sender);
  add("interface", interface);
  add("member", member);
  add("path", path);
  add("arg0", arg0);
  add("arg0namespace", arg0_namespace);
  return rule;
}

// ---------------------------------------------------------------------------
// Connection

Connection::Connection(Bus bus, Loop& loop) : loop_(loop) {
  socket_ = connect_address(bus_address(bus));
  if (socket_ < 0) throw std::runtime_error(bus == Bus::system ? "could not connect to the system D-Bus"
                                                               : "could not connect to the session D-Bus");
  try {
    authenticate(socket_);
  } catch (...) {
    ::close(socket_);
    throw;
  }
  reader_ = std::thread([this] { reader(); });
  std::string error;
  const auto reply = call(Message::method_call(bus_name, bus_path, bus_name, "Hello"), 5000, &error);
  if (!reply || reply->body.empty()) {
    ::shutdown(socket_, SHUT_RDWR);
    reader_.join();
    ::close(socket_);
    throw std::runtime_error("D-Bus Hello failed: " + error);
  }
  unique_name_ = reply->body[0].text;
}

Connection::~Connection() {
  *alive_ = false;
  ::shutdown(socket_, SHUT_RDWR);
  if (reader_.joinable()) reader_.join();
  ::close(socket_);
}

bool Connection::connected() const {
  std::lock_guard lock(mutex_);
  return !closed_;
}

void Connection::write(Message& message) {
  std::lock_guard lock(write_mutex_);
  if (message.serial == 0) message.serial = next_serial_++;
  if (next_serial_ == 0) next_serial_ = 1;
  const std::vector<std::uint8_t> data = serialize(message);
  if (!write_all(socket_, data.data(), data.size())) ::shutdown(socket_, SHUT_RDWR);
}

std::optional<Message> Connection::call(Message message, int timeout_ms, std::string* error) {
  message.type = Message::Type::call;
  message.flags &= static_cast<std::uint8_t>(~Message::no_reply_expected);
  auto pending = std::make_shared<Pending>();
  {
    std::lock_guard write_lock(write_mutex_);
    message.serial = next_serial_++;
    if (next_serial_ == 0) next_serial_ = 1;
  }
  {
    std::lock_guard lock(mutex_);
    if (closed_) {
      if (error != nullptr) *error = "D-Bus connection closed";
      return std::nullopt;
    }
    pending_[message.serial] = pending;
  }
  write(message);
  std::unique_lock lock(mutex_);
  const bool answered = replies_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return pending->done; });
  pending_.erase(message.serial);
  if (!answered) {
    if (error != nullptr) *error = "D-Bus call timed out: " + message.member;
    return std::nullopt;
  }
  if (error != nullptr) *error = pending->error;
  return std::move(pending->reply);
}

std::optional<Value> Connection::call_value(Message message, int timeout_ms) {
  auto reply = call(std::move(message), timeout_ms);
  if (!reply || reply->body.empty()) return std::nullopt;
  return std::move(reply->body[0]);
}

void Connection::call_async(Message message, int timeout_ms, Callback callback) {
  message.type = Message::Type::call;
  if (!callback) {
    message.flags |= Message::no_reply_expected;
    write(message);
    return;
  }
  auto pending = std::make_shared<Pending>();
  pending->callback = std::move(callback);
  {
    std::lock_guard write_lock(write_mutex_);
    message.serial = next_serial_++;
    if (next_serial_ == 0) next_serial_ = 1;
  }
  const std::uint32_t serial = message.serial;
  {
    std::lock_guard lock(mutex_);
    if (closed_) {
      loop_.post([pending] { pending->callback(nullptr, "D-Bus connection closed"); });
      return;
    }
    pending_[serial] = pending;
    pending->timer = loop_.after(std::chrono::milliseconds(timeout_ms), [this, serial, alive = alive_] {
      if (!*alive) return;
      std::shared_ptr<Pending> expired;
      {
        std::lock_guard timer_lock(mutex_);
        const auto found = pending_.find(serial);
        if (found == pending_.end()) return;
        expired = found->second;
        pending_.erase(found);
      }
      expired->callback(nullptr, "D-Bus call timed out");
    });
  }
  write(message);
}

void Connection::send(Message message) { write(message); }

void Connection::emit(std::string path, std::string interface, std::string member, std::vector<Value> body) {
  Message message = Message::signal(std::move(path), std::move(interface), std::move(member), std::move(body));
  write(message);
}

void Connection::reader() {
  std::vector<std::uint8_t> buffer;
  for (;;) {
    std::array<std::uint8_t, 16> head{};
    if (!read_all(socket_, head.data(), head.size())) break;
    const bool little = head[0] == 'l';
    if (head[0] != 'l' && head[0] != 'B') break;
    const auto field = [&](std::size_t at) {
      std::uint32_t v = 0;
      for (std::size_t i = 0; i < 4; ++i)
        v |= static_cast<std::uint32_t>(head[at + i]) << (8 * (little ? i : 3 - i));
      return static_cast<std::size_t>(v);
    };
    const std::size_t body = field(4), fields = field(12);
    const std::size_t header = (16 + fields + 7) / 8 * 8;
    if (body > max_message || fields > max_message) break;
    buffer.assign(header + body, 0);
    std::memcpy(buffer.data(), head.data(), head.size());
    if (!read_all(socket_, buffer.data() + head.size(), buffer.size() - head.size())) break;
    try {
      incoming(parse(buffer));
    } catch (const std::exception&) {
      // A message we cannot read is dropped; the stream stays in step.
    }
  }
  fail_pending("D-Bus connection closed");
}

void Connection::fail_pending(const std::string& reason) {
  std::vector<std::shared_ptr<Pending>> callbacks;
  std::vector<std::function<void()>> closed;
  {
    std::lock_guard lock(mutex_);
    closed_ = true;
    for (auto& [serial, pending] : pending_) {
      if (pending->callback) {
        callbacks.push_back(pending);
      } else {
        pending->done = true;
        pending->error = reason;
      }
    }
    std::erase_if(pending_, [](const auto& entry) { return entry.second->callback != nullptr; });
    closed = closed_handlers_;
  }
  replies_.notify_all();
  for (auto& pending : callbacks) {
    loop_.cancel(pending->timer);
    loop_.post([pending, reason] { pending->callback(nullptr, reason); });
  }
  for (auto& handler : closed) loop_.post([handler, alive = alive_] { if (*alive) handler(); });
}

void Connection::incoming(Message message) {
  if (message.type == Message::Type::reply || message.type == Message::Type::error) {
    std::shared_ptr<Pending> pending;
    {
      std::lock_guard lock(mutex_);
      const auto found = pending_.find(message.reply_serial);
      if (found == pending_.end()) return;
      pending = found->second;
      if (pending->callback) pending_.erase(found);
      if (!pending->callback) {
        pending->done = true;
        if (message.type == Message::Type::error) pending->error = error_text(message);
        else pending->reply = std::move(message);
      }
    }
    if (!pending->callback) {
      replies_.notify_all();
      return;
    }
    loop_.cancel(pending->timer);
    auto shared = std::make_shared<Message>(std::move(message));
    loop_.post([pending, shared] {
      if (shared->type == Message::Type::error) pending->callback(nullptr, error_text(*shared));
      else pending->callback(shared.get(), {});
    });
    return;
  }
  if (message.type == Message::Type::signal) {
    if (message.sender == bus_name && message.member == "NameOwnerChanged" && message.body.size() == 3) {
      std::lock_guard lock(mutex_);
      if (followed_.contains(message.body[0].text)) owners_[message.body[0].text] = message.body[2].text;
    }
    auto shared = std::make_shared<Message>(std::move(message));
    loop_.post([this, shared, alive = alive_] {
      if (*alive) dispatch_signal(*shared);
    });
    return;
  }
  if (message.type == Message::Type::call) {
    auto shared = std::make_shared<Message>(std::move(message));
    loop_.post([this, shared, alive = alive_] {
      if (*alive) dispatch_call(*shared);
    });
  }
}

bool Connection::matches(const Match& match, const Message& message) {
  if (!match.interface.empty() && match.interface != message.interface) return false;
  if (!match.member.empty() && match.member != message.member) return false;
  if (!match.path.empty() && match.path != message.path) return false;
  if (!match.arg0.empty() || !match.arg0_namespace.empty()) {
    const Value* first = message.arg(0);
    if (first == nullptr || !first->is("s")) return false;
    if (!match.arg0.empty() && first->text != match.arg0) return false;
    if (!match.arg0_namespace.empty() && first->text != match.arg0_namespace &&
        !first->text.starts_with(match.arg0_namespace + "."))
      return false;
  }
  if (!match.sender.empty() && match.sender != message.sender) {
    const auto owner = owners_.find(match.sender);
    if (owner == owners_.end() || owner->second != message.sender) return false;
  }
  return true;
}

void Connection::dispatch_signal(const Message& message) {
  std::vector<std::function<void(const Message&)>> handlers;
  {
    std::lock_guard lock(mutex_);
    for (const auto& [id, subscription] : subscriptions_)
      if (matches(subscription.match, message)) handlers.push_back(subscription.handler);
  }
  for (const auto& handler : handlers) handler(message);
}

void Connection::dispatch_call(const Message& message) {
  const bool wants_reply = (message.flags & Message::no_reply_expected) == 0;
  const auto respond = [&](Reply reply) {
    if (!wants_reply) return;
    Message answer;
    answer.reply_serial = message.serial;
    answer.destination = message.sender;
    if (!reply.error_name.empty()) {
      answer.type = Message::Type::error;
      answer.error_name = std::move(reply.error_name);
      answer.body.push_back(Value::string(reply.error_message));
    } else {
      answer.type = Message::Type::reply;
      answer.body = std::move(reply.body);
    }
    write(answer);
  };
  if (message.interface == "org.freedesktop.DBus.Peer") {
    if (message.member == "Ping") return respond({});
    if (message.member == "GetMachineId") {
      std::ifstream file("/etc/machine-id");
      std::string id;
      file >> id;
      return respond({{Value::string(id)}, {}, {}});
    }
  }
  std::vector<Exported> interfaces;
  {
    std::lock_guard lock(mutex_);
    const auto found = objects_.find(message.path);
    if (found != objects_.end()) interfaces = found->second;
  }
  if (interfaces.empty())
    return respond(Reply::error("org.freedesktop.DBus.Error.UnknownObject", "no object at " + message.path));
  try {
    if (message.interface == "org.freedesktop.DBus.Introspectable" && message.member == "Introspect") {
      std::string xml =
          "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
          "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n<node>\n"
          "<interface name=\"org.freedesktop.DBus.Introspectable\"><method name=\"Introspect\">"
          "<arg type=\"s\" direction=\"out\"/></method></interface>\n"
          "<interface name=\"org.freedesktop.DBus.Properties\"><method name=\"Get\"><arg type=\"s\" direction=\"in\"/>"
          "<arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"out\"/></method><method name=\"GetAll\">"
          "<arg type=\"s\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"out\"/></method></interface>\n";
      for (const Exported& object : interfaces)
        xml += "<interface name=\"" + object.interface + "\">" + object.introspection + "</interface>\n";
      xml += "</node>\n";
      return respond({{Value::string(xml)}, {}, {}});
    }
    if (message.interface == "org.freedesktop.DBus.Properties") {
      const Value* interface_name = message.arg(0);
      const auto exported = std::ranges::find_if(interfaces, [&](const Exported& object) {
        return interface_name != nullptr && object.interface == interface_name->text;
      });
      if (exported == interfaces.end())
        return respond(Reply::error("org.freedesktop.DBus.Error.UnknownInterface", "unknown interface"));
      const auto properties = exported->properties ? exported->properties() : decltype(exported->properties()){};
      if (message.member == "GetAll") {
        std::vector<std::pair<Value, Value>> entries;
        for (const auto& [name, value] : properties) entries.emplace_back(Value::string(name), Value::variant(value));
        return respond({{Value::dict("s", "v", std::move(entries))}, {}, {}});
      }
      if (message.member == "Get") {
        const Value* property = message.arg(1);
        for (const auto& [name, value] : properties)
          if (property != nullptr && name == property->text) return respond({{Value::variant(value)}, {}, {}});
        return respond(Reply::error("org.freedesktop.DBus.Error.UnknownProperty", "unknown property"));
      }
      if (message.member == "Set")
        return respond(Reply::error("org.freedesktop.DBus.Error.PropertyReadOnly", "properties are read-only"));
    }
    for (const Exported& object : interfaces)
      if ((message.interface.empty() || message.interface == object.interface) && object.call)
        return respond(object.call(message));
    respond(Reply::error("org.freedesktop.DBus.Error.UnknownMethod", "unknown method " + message.member));
  } catch (const std::exception& error) {
    respond(Reply::error("org.freedesktop.DBus.Error.InvalidArgs", error.what()));
  }
}

void Connection::add_match(const std::string& rule) {
  call_async(Message::method_call(bus_name, bus_path, bus_name, "AddMatch", {Value::string(rule)}));
}

void Connection::remove_match(const std::string& rule) {
  call_async(Message::method_call(bus_name, bus_path, bus_name, "RemoveMatch", {Value::string(rule)}));
}

void Connection::follow_owner(const std::string& name) {
  {
    std::lock_guard lock(mutex_);
    if (followed_[name]++ > 0) return;
  }
  Match changes{bus_name, bus_name, "NameOwnerChanged", bus_path, name, {}};
  add_match(changes.rule());
  const auto owner = call_value(Message::method_call(bus_name, bus_path, bus_name, "GetNameOwner", {Value::string(name)}));
  std::lock_guard lock(mutex_);
  if (owner && !owners_.contains(name)) owners_[name] = owner->text;
}

std::uint64_t Connection::subscribe(Match match, std::function<void(const Message&)> handler) {
  std::uint64_t id = 0;
  const std::string rule = match.rule();
  const std::string sender = match.sender;
  {
    std::lock_guard lock(mutex_);
    id = next_subscription_++;
    subscriptions_.emplace(id, Subscription{std::move(match), std::move(handler)});
  }
  add_match(rule);
  if (!sender.empty() && sender[0] != ':' && sender != bus_name) follow_owner(sender);
  return id;
}

void Connection::unsubscribe(std::uint64_t id) {
  std::string rule;
  {
    std::lock_guard lock(mutex_);
    const auto found = subscriptions_.find(id);
    if (found == subscriptions_.end()) return;
    rule = found->second.match.rule();
    subscriptions_.erase(found);
  }
  remove_match(rule);
}

std::uint64_t Connection::watch_name(std::string name, std::function<void(const std::string&)> appeared,
                                     std::function<void()> vanished) {
  const std::uint64_t id = subscribe(Match{bus_name, bus_name, "NameOwnerChanged", bus_path, name, {}},
                                     [appeared, vanished](const Message& message) {
                                       const Value* owner = message.arg(2);
                                       if (owner == nullptr) return;
                                       if (owner->text.empty()) {
                                         if (vanished) vanished();
                                       } else if (appeared) {
                                         appeared(owner->text);
                                       }
                                     });
  call_async(Message::method_call(bus_name, bus_path, bus_name, "GetNameOwner", {Value::string(name)}), 2000,
             [appeared, vanished](const Message* reply, const std::string&) {
               if (reply != nullptr && !reply->body.empty() && !reply->body[0].text.empty()) {
                 if (appeared) appeared(reply->body[0].text);
               } else if (vanished) {
                 vanished();
               }
             });
  return id;
}

void Connection::unwatch_name(std::uint64_t id) { unsubscribe(id); }

std::uint32_t Connection::request_name(std::string_view name, std::uint32_t flags) {
  const auto reply = call_value(
      Message::method_call(bus_name, bus_path, bus_name, "RequestName", {Value::string(name), Value::uint32(flags)}),
      2000);
  return reply ? static_cast<std::uint32_t>(reply->bits) : 0;
}

void Connection::release_name(std::string_view name) {
  (void)call(Message::method_call(bus_name, bus_path, bus_name, "ReleaseName", {Value::string(name)}), 1000);
}

void Connection::export_object(std::string path, Exported object) {
  std::lock_guard lock(mutex_);
  objects_[std::move(path)].push_back(std::move(object));
}

void Connection::unexport_object(const std::string& path) {
  std::lock_guard lock(mutex_);
  objects_.erase(path);
}

void Connection::on_closed(std::function<void()> handler) {
  bool already = false;
  {
    std::lock_guard lock(mutex_);
    already = closed_;
    if (!already) closed_handlers_.push_back(handler);
  }
  if (already) loop_.post(std::move(handler));
}

// ---------------------------------------------------------------------------
// Monitor

Monitor::Monitor(Bus bus, Refresh refresh, Setup setup, unsigned debounce_ms)
    : bus_(bus), refresh_(std::move(refresh)), setup_(std::move(setup)), debounce_ms_(debounce_ms),
      event_(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {
  if (event_ < 0) throw std::runtime_error("could not create service event fd");
}

Monitor::~Monitor() {
  if (thread_.joinable()) {
    loop_.quit();
    thread_.join();
  }
  connection_.reset();
  close(event_);
}

void Monitor::watch(std::string name) { names_.push_back(std::move(name)); }

void Monitor::start() {
  std::unique_lock lock(mutex_);
  if (!started_) {
    started_ = true;
    thread_ = std::thread([this] { run(); });
  }
  ready_condition_.wait(lock, [this] { return ready_; });
}

Connection* Monitor::connection() {
  std::lock_guard lock(mutex_);
  return connection_.get();
}

bool Monitor::drain() {
  std::uint64_t count = 0;
  bool changed = false;
  while (read(event_, &count, sizeof(count)) > 0) changed = true;
  return changed;
}

void Monitor::wake() {
  const std::uint64_t one = 1;
  const auto result = write(event_, &one, sizeof(one));
  (void)result;
}

void Monitor::invoke(std::function<void()> task) {
  if (connection() == nullptr) return;  // no bus, no loop
  if (loop_.in_loop()) task();
  else loop_.post(std::move(task));
}

void Monitor::schedule_refresh() {
  invoke([this] {
    if (pending_) return;
    pending_ = true;
    loop_.after(std::chrono::milliseconds(debounce_ms_), [this] {
      pending_ = false;
      refresh_(connection_.get());
      wake();
    });
  });
}

void Monitor::run() {
  std::unique_ptr<Connection> connection;
  try {
    connection = std::make_unique<Connection>(bus_, loop_);
  } catch (const std::exception&) {
    // No bus: the service reports itself unavailable.
  }
  Connection* bus = connection.get();
  {
    std::lock_guard lock(mutex_);
    connection_ = std::move(connection);
  }
  if (bus == nullptr) {
    {
      std::lock_guard lock(mutex_);
      ready_ = true;
    }
    ready_condition_.notify_all();
    wake();
    return;
  }
  loop_.post([this, bus] {
    for (const auto& name : names_) {
      bus->subscribe(Match{name, {}, {}, {}, {}, {}}, [this](const Message&) { schedule_refresh(); });
      bus->subscribe(Match{"org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
                           "/org/freedesktop/DBus", name, {}},
                     [this](const Message&) { schedule_refresh(); });
    }
    if (setup_) setup_(bus);
    refresh_(bus);
    {
      std::lock_guard lock(mutex_);
      ready_ = true;
    }
    ready_condition_.notify_all();
    wake();
  });
  loop_.run();
}

}  // namespace vw::dbus
