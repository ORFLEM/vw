// Installed applications from freedesktop desktop entries: discovery,
// localized fields, menu visibility, search and launching.
#include "vw/applications.hpp"
#include "vw/icon.hpp"
#include "vw/process.hpp"

#include "dbus.hpp"
#include "desktop_entry.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace vw::applications {
namespace {

namespace fs = std::filesystem;

std::vector<std::string> split(std::string_view text, char separator) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (start <= text.size()) {
    std::size_t end = text.find(separator, start);
    if (end == std::string_view::npos) end = text.size();
    if (end > start) parts.emplace_back(text.substr(start, end - start));
    start = end + 1;
  }
  return parts;
}

std::string_view trim(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) value.remove_suffix(1);
  return value;
}

std::string lowercase(std::string_view value) {
  std::string result(value);
  std::ranges::transform(result, result.begin(),
                         [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
  return result;
}

// $XDG_DATA_HOME, then $XDG_DATA_DIRS: earlier directories win.
std::vector<fs::path> data_directories() {
  std::vector<fs::path> directories;
  const char* home = std::getenv("HOME");
  if (const char* data_home = std::getenv("XDG_DATA_HOME"); data_home != nullptr && *data_home == '/')
    directories.emplace_back(data_home);
  else if (home != nullptr && *home != '\0')
    directories.emplace_back(fs::path(home) / ".local/share");
  const char* data_dirs = std::getenv("XDG_DATA_DIRS");
  for (const std::string& directory :
       split(data_dirs != nullptr && *data_dirs != '\0' ? data_dirs : "/usr/local/share:/usr/share", ':'))
    if (!directory.empty() && directory.front() == '/') directories.emplace_back(directory);
  return directories;
}

// Locale variants to try for localized keys, most specific first
// (lang_COUNTRY@MODIFIER, lang_COUNTRY, lang@MODIFIER, lang).
std::vector<std::string> locale_variants() {
  std::vector<std::string> locales;
  if (const char* language = std::getenv("LANGUAGE"); language != nullptr && *language != '\0')
    locales = split(language, ':');
  for (const char* name : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
    const char* value = std::getenv(name);
    if (value != nullptr && *value != '\0') {
      locales.emplace_back(value);
      break;
    }
  }
  std::vector<std::string> variants;
  for (const std::string& locale : locales) {
    if (locale == "C" || locale == "POSIX") continue;
    std::string_view rest(locale);
    std::string modifier;
    if (const auto at = rest.find('@'); at != std::string_view::npos) {
      modifier = std::string(rest.substr(at));
      rest = rest.substr(0, at);
    }
    if (const auto dot = rest.find('.'); dot != std::string_view::npos) rest = rest.substr(0, dot);
    const std::string language_country(rest);
    const std::string language(rest.substr(0, rest.find('_')));
    for (const std::string& variant : {language_country + modifier, language_country, language + modifier, language})
      if (!variant.empty() && std::ranges::find(variants, variant) == variants.end()) variants.push_back(variant);
  }
  return variants;
}

// The [Desktop Entry] group's keys, values unescaped.
struct DesktopFile {
  std::map<std::string, std::string, std::less<>> keys;

  std::string get(std::string_view key) const {
    const auto found = keys.find(key);
    return found == keys.end() ? std::string() : found->second;
  }
  std::string localized(std::string_view key) const {
    static const std::vector<std::string> variants = locale_variants();
    for (const std::string& variant : variants) {
      const auto found = keys.find(std::string(key) + "[" + variant + "]");
      if (found != keys.end()) return found->second;
    }
    return get(key);
  }
  bool flag(std::string_view key) const { return get(key) == "true"; }
  // ;-separated lists, with \; for a literal semicolon.
  std::vector<std::string> list(std::string_view key, bool localize = false) const {
    const std::string raw = localize ? localized(key) : get(key);
    std::vector<std::string> items;
    std::string current;
    for (std::size_t i = 0; i < raw.size(); ++i) {
      if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == ';') {
        current.push_back(';');
        ++i;
      } else if (raw[i] == ';') {
        if (!current.empty()) items.push_back(current);
        current.clear();
      } else {
        current.push_back(raw[i]);
      }
    }
    if (!current.empty()) items.push_back(current);
    return items;
  }
};

std::string unescape(std::string_view value) {
  std::string out;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] != '\\' || i + 1 >= value.size()) {
      out.push_back(value[i]);
      continue;
    }
    switch (value[++i]) {
      case 's': out.push_back(' '); break;
      case 'n': out.push_back('\n'); break;
      case 't': out.push_back('\t'); break;
      case 'r': out.push_back('\r'); break;
      case '\\': out.push_back('\\'); break;
      default:
        // Kept for list splitting (\;) and Exec quoting.
        out.push_back('\\');
        out.push_back(value[i]);
    }
  }
  return out;
}

std::optional<DesktopFile> read_desktop_file(const fs::path& path) {
  std::ifstream input(path);
  if (!input) return std::nullopt;
  DesktopFile file;
  bool in_entry = false;
  std::string line;
  while (std::getline(input, line)) {
    const std::string_view text = trim(line);
    if (text.empty() || text.front() == '#') continue;
    if (text.front() == '[') {
      in_entry = text == "[Desktop Entry]";
      continue;
    }
    if (!in_entry) continue;
    const auto equals = text.find('=');
    if (equals == std::string_view::npos) continue;
    const std::string key(trim(text.substr(0, equals)));
    if (!file.keys.contains(key)) file.keys.emplace(key, unescape(trim(text.substr(equals + 1))));
  }
  return file;
}

bool on_path(const std::string& program) {
  if (program.empty()) return false;
  if (program.find('/') != std::string::npos) return access(program.c_str(), X_OK) == 0;
  const char* path = std::getenv("PATH");
  for (const std::string& directory : split(path != nullptr ? path : "/usr/local/bin:/usr/bin:/bin", ':'))
    if (access((fs::path(directory) / program).c_str(), X_OK) == 0) return true;
  return false;
}

// Exec split into arguments, with quoting (double quotes; \" \` \$ \\ inside).
std::vector<std::string> exec_arguments(std::string_view exec) {
  std::vector<std::string> arguments;
  std::string current;
  bool quoted = false, started = false;
  for (std::size_t i = 0; i < exec.size(); ++i) {
    const char c = exec[i];
    if (quoted) {
      if (c == '\\' && i + 1 < exec.size() && std::string_view("\"`$\\").find(exec[i + 1]) != std::string_view::npos) {
        current.push_back(exec[++i]);
      } else if (c == '"') {
        quoted = false;
      } else {
        current.push_back(c);
      }
    } else if (c == '"') {
      quoted = true;
      started = true;
    } else if (c == ' ' || c == '\t') {
      if (started) arguments.push_back(std::move(current));
      current.clear();
      started = false;
    } else {
      current.push_back(c);
      started = true;
    }
  }
  if (started) arguments.push_back(std::move(current));
  return arguments;
}

struct Installed {
  Application application;
  DesktopFile file;
  bool shown = true;
};

bool shown_in_menus(const DesktopFile& file) {
  if (file.flag("NoDisplay")) return false;
  const char* current = std::getenv("XDG_CURRENT_DESKTOP");
  const std::vector<std::string> desktops = split(current != nullptr ? current : "", ':');
  const auto any = [&](const std::vector<std::string>& names) {
    return std::ranges::any_of(names, [&](const std::string& name) { return std::ranges::find(desktops, name) != desktops.end(); });
  };
  const auto only = file.list("OnlyShowIn");
  if (!only.empty() && !any(only)) return false;
  return !any(file.list("NotShowIn"));
}

// The Icon key as an icon name or path: theme names lose an image
// extension, as the icon theme spec asks.
std::string icon_of(const DesktopFile& file) {
  std::string icon = file.localized("Icon");
  if (icon.empty() || icon.front() == '/') return icon;
  for (const char* extension : {".png", ".svg", ".xpm"})
    if (icon.ends_with(extension)) icon.resize(icon.size() - std::string_view(extension).size());
  return icon;
}

Installed describe(const std::string& id, const fs::path& path, DesktopFile file) {
  Installed installed;
  Application& result = installed.application;
  result.id = id;
  result.name = file.localized("Name");
  const std::string full_name = file.localized("X-GNOME-FullName");
  result.display_name = full_name.empty() ? result.name : full_name;
  result.generic_name = file.localized("GenericName");
  result.description = file.localized("Comment");
  result.commandline = file.get("Exec");
  const auto arguments = exec_arguments(result.commandline);
  result.executable = arguments.empty() ? std::string() : arguments.front();
  result.filename = path.string();
  result.startup_wm_class = file.get("StartupWMClass");
  result.icon = icon_of(file);
  result.terminal = file.flag("Terminal");
  result.categories = file.list("Categories");
  result.keywords = file.list("Keywords", true);
  installed.shown = shown_in_menus(file);
  installed.file = std::move(file);
  return installed;
}

// Every installed application by id, rescanned when an applications
// directory changes.
class Catalog {
 public:
  std::vector<Installed> all() {
    std::lock_guard lock(mutex_);
    refresh();
    std::vector<Installed> result;
    result.reserve(entries_.size());
    for (const auto& [id, installed] : entries_) result.push_back(installed);
    return result;
  }
  std::optional<Installed> find(const std::string& id) {
    std::lock_guard lock(mutex_);
    refresh();
    const auto found = entries_.find(id);
    if (found == entries_.end()) return std::nullopt;
    return found->second;
  }

 private:
  // Directory modification times, cheap to compare.
  std::vector<std::pair<fs::path, fs::file_time_type>> stamp() const {
    std::vector<std::pair<fs::path, fs::file_time_type>> result;
    for (const fs::path& directory : data_directories()) {
      std::error_code error;
      const fs::path applications = directory / "applications";
      for (auto it = fs::recursive_directory_iterator(applications, fs::directory_options::follow_directory_symlink, error);
           !error && it != fs::recursive_directory_iterator(); it.increment(error))
        if (it->is_directory(error)) result.emplace_back(it->path(), fs::last_write_time(it->path(), error));
      result.emplace_back(applications, fs::last_write_time(applications, error));
    }
    return result;
  }

  void refresh() {
    auto current = stamp();
    if (scanned_ && current == stamp_) return;
    stamp_ = std::move(current);
    scanned_ = true;
    entries_.clear();
    std::set<std::string> claimed;
    for (const fs::path& directory : data_directories()) {
      const fs::path root = directory / "applications";
      std::error_code error;
      std::vector<fs::path> files;
      for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::follow_directory_symlink, error);
           !error && it != fs::recursive_directory_iterator(); it.increment(error))
        if (it->path().extension() == ".desktop") files.push_back(it->path());
      std::ranges::sort(files);
      for (const fs::path& path : files) {
        // The desktop file id: the path below applications/, '/' as '-'.
        std::string id = path.lexically_relative(root).string();
        std::ranges::replace(id, '/', '-');
        if (!claimed.insert(id).second) continue;
        auto file = read_desktop_file(path);
        if (!file || file->get("Type") != "Application" || file->flag("Hidden")) continue;
        if (const std::string try_exec = file->get("TryExec"); !try_exec.empty() && !on_path(try_exec)) continue;
        if (file->get("Exec").empty() && !file->flag("DBusActivatable")) continue;
        entries_.emplace(id, describe(id, path, std::move(*file)));
      }
    }
  }

  std::mutex mutex_;
  bool scanned_ = false;
  std::vector<std::pair<fs::path, fs::file_time_type>> stamp_;
  std::map<std::string, Installed> entries_;
};

Catalog& catalog() {
  static Catalog instance;
  return instance;
}

// Lower ranks sort first: exact name, name prefix, word prefix, name
// substring, then any other searchable field or keyword.
std::optional<int> match_rank(const Application& app, std::string_view needle) {
  if (needle.empty()) return 0;
  int best = std::numeric_limits<int>::max();
  for (const std::string* value : {&app.display_name, &app.name}) {
    if (value->empty()) continue;
    const std::string name = lowercase(*value);
    const std::size_t found = name.find(needle);
    if (found == std::string::npos) continue;
    if (name.size() == needle.size()) best = std::min(best, 0);
    else if (found == 0) best = std::min(best, 1);
    else if (!std::isalnum(static_cast<unsigned char>(name[found - 1]))) best = std::min(best, 2);
    else best = std::min(best, 3);
  }
  if (best != std::numeric_limits<int>::max()) return best;
  for (const std::string* value : {&app.generic_name, &app.description, &app.executable, &app.id})
    if (lowercase(*value).contains(needle)) return 4;
  for (const std::string& keyword : app.keywords)
    if (lowercase(keyword).contains(needle)) return 4;
  return std::nullopt;
}

Application with_icon(Application application) {
  application.icon_path = resolve_icon(application.icon, 48).string();
  return application;
}

// The argument list for an entry's Exec, with field codes expanded for a
// launch without files.
std::vector<std::string> command_line(const Installed& installed) {
  const DesktopFile& file = installed.file;
  std::vector<std::string> arguments;
  for (const std::string& argument : exec_arguments(file.get("Exec"))) {
    if (argument == "%i") {
      if (const std::string icon = file.get("Icon"); !icon.empty()) {
        arguments.emplace_back("--icon");
        arguments.push_back(icon);
      }
      continue;
    }
    std::string expanded;
    bool only_code = argument.size() == 2 && argument[0] == '%';
    for (std::size_t i = 0; i < argument.size(); ++i) {
      if (argument[i] != '%' || i + 1 >= argument.size()) {
        expanded.push_back(argument[i]);
        continue;
      }
      switch (argument[++i]) {
        case '%': expanded.push_back('%'); only_code = false; break;
        case 'c': expanded += installed.application.name; break;
        case 'k': expanded += installed.application.filename; break;
        default: break;  // %f %F %u %U and the deprecated codes: no files to pass
      }
    }
    if (!(only_code && expanded.empty())) arguments.push_back(std::move(expanded));
  }
  return arguments;
}

// A terminal to run Terminal=true entries in: $TERMINAL, then the list GLib
// tries, with the flag each one takes before a command.
std::vector<std::string> terminal_prefix() {
  if (const char* terminal = std::getenv("TERMINAL"); terminal != nullptr && on_path(terminal))
    return {terminal, "-e"};
  static const std::vector<std::pair<std::string, std::string>> terminals{
      {"xdg-terminal-exec", ""}, {"kgx", "-e"}, {"gnome-terminal", "--"}, {"mate-terminal", "-x"},
      {"xfce4-terminal", "-x"}, {"tilix", "-e"}, {"konsole", "-e"}, {"nxterm", "-e"},
      {"color-xterm", "-e"}, {"rxvt", "-e"}, {"dtterm", "-e"}, {"xterm", "-e"}};
  for (const auto& [program, flag] : terminals)
    if (on_path(program)) return flag.empty() ? std::vector<std::string>{program} : std::vector<std::string>{program, flag};
  throw std::runtime_error("no terminal emulator found for a Terminal=true application");
}

// org.freedesktop.Application.Activate on the entry's bus name.
bool activate_over_dbus(const std::string& id) {
  std::string name = id.ends_with(".desktop") ? id.substr(0, id.size() - 8) : id;
  std::string path = "/" + name;
  std::ranges::replace(path, '.', '/');
  std::ranges::replace(path, '-', '_');
  try {
    dbus::Loop loop;
    dbus::Connection bus(dbus::Bus::session, loop);
    return bus.call(dbus::Message::method_call(name, path, "org.freedesktop.Application", "Activate",
                                               {dbus::Value::dict("s", "v")}),
                    5000)
        .has_value();
  } catch (const std::exception&) {
    return false;
  }
}

}  // namespace

std::optional<Application> find(std::string_view id) {
  auto installed = catalog().find(std::string(id));
  if (!installed) return std::nullopt;
  return with_icon(std::move(installed->application));
}

std::vector<Application> search(std::string_view query, std::size_t limit) {
  const std::string needle = lowercase(query);
  std::vector<Installed> apps = catalog().all();
  std::erase_if(apps, [](const Installed& app) { return !app.shown; });
  std::ranges::stable_sort(apps, {}, [](const Installed& app) { return app.application.display_name; });
  std::vector<std::pair<int, const Application*>> matches;
  for (const Installed& app : apps)
    if (const auto rank = match_rank(app.application, needle)) matches.emplace_back(*rank, &app.application);
  // apps is sorted by display name, so a stable sort keeps that order per rank.
  std::ranges::stable_sort(matches, {}, &std::pair<int, const Application*>::first);
  if (limit > 0 && matches.size() > limit) matches.resize(limit);
  std::vector<Application> result;
  result.reserve(matches.size());
  for (const auto& [rank, app] : matches) result.push_back(with_icon(*app));
  return result;
}

void launch(std::string_view id) {
  const std::string name(id);
  const auto installed = catalog().find(name);
  if (!installed) throw std::runtime_error("desktop application '" + name + "' was not found");
  if (installed->file.flag("DBusActivatable") && activate_over_dbus(name)) return;
  std::vector<std::string> arguments = command_line(*installed);
  if (arguments.empty()) throw std::runtime_error("desktop application '" + name + "' has no command");
  if (installed->application.terminal) {
    std::vector<std::string> wrapped = terminal_prefix();
    wrapped.insert(wrapped.end(), arguments.begin(), arguments.end());
    arguments = std::move(wrapped);
  }
  // Path= is the working directory; the shell changes into it and execs.
  if (const std::string directory = installed->file.get("Path"); !directory.empty()) {
    std::vector<std::string> wrapped{"/bin/sh", "-c", "cd \"$0\" && exec \"$@\"", directory};
    wrapped.insert(wrapped.end(), arguments.begin(), arguments.end());
    arguments = std::move(wrapped);
  }
  if (!on_path(arguments.front())) throw std::runtime_error("could not find " + arguments.front());
  spawn(arguments);
}

}  // namespace vw::applications
