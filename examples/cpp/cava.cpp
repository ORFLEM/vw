// An audio visualizer from cava: rounded bars inside a rounded card. The bars
// bleed past every edge on purpose (the outer bars hang off the sides, all of
// them run below the bottom, loud ones past the top), so the card visibly
// clips them to its rounded shape, corners included, every frame.
//
//   cmake --build build && ./build/vw-example-cava
//
// cava runs with raw ASCII output into a pipe, so the widget redraws when a
// frame arrives and never polls.
#include <vw/vw.hpp>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr int bar_count = 28;
constexpr int max_value = 1000;

// cava's config: raw "v;v;v;…\n" frames on stdout, values 0..max_value.
std::filesystem::path write_config() {
  const auto path = std::filesystem::temp_directory_path() / ("vw-cava-" + std::to_string(getpid()) + ".conf");
  std::ofstream(path) << "[general]\n"
                      << "bars = " << bar_count << "\n"
                      << "framerate = 60\n"
                      << "[output]\n"
                      << "method = raw\n"
                      << "raw_target = /dev/stdout\n"
                      << "data_format = ascii\n"
                      << "ascii_max_range = " << max_value << "\n"
                      << "bar_delimiter = 59\n"
                      << "frame_delimiter = 10\n";
  return path;
}

struct Cava {
  pid_t pid = -1;
  int fd = -1;
  int quiet_writer = -1;
  bool missing = false;
  std::string pending;
  std::vector<float> bars = std::vector<float>(bar_count, 0.0F);  // 0..1

  void start(const std::filesystem::path& config) {
    int pipe_fds[2];
    if (pipe2(pipe_fds, O_CLOEXEC) != 0) {
      missing = true;
      return;
    }
    pid = fork();
    if (pid == 0) {
      dup2(pipe_fds[1], STDOUT_FILENO);
      execlp("cava", "cava", "-p", config.c_str(), static_cast<char*>(nullptr));
      _exit(127);
    }
    close(pipe_fds[1]);
    fd = pipe_fds[0];
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  }

  // Reads what cava wrote; true when the widget should redraw.
  bool drain() {
    char buffer[4096];
    for (;;) {
      const ssize_t count = read(fd, buffer, sizeof(buffer));
      if (count > 0) {
        pending.append(buffer, static_cast<std::size_t>(count));
        continue;
      }
      if (count == 0) {
        // cava exited, or is not installed. Swap the pipe for one that never
        // becomes readable, so the event loop stops waking for it.
        missing = true;
        int quiet[2];
        if (pipe2(quiet, O_CLOEXEC) == 0) {
          dup2(quiet[0], fd);
          close(quiet[0]);
          quiet_writer = quiet[1];
        }
        return true;
      }
      break;
    }
    // Only the newest complete frame matters.
    const auto end_of_frame = pending.rfind('\n');
    if (end_of_frame == std::string::npos) return false;
    const auto previous = pending.rfind('\n', end_of_frame == 0 ? 0 : end_of_frame - 1);
    const std::size_t start = previous == std::string::npos || previous == end_of_frame ? 0 : previous + 1;
    const std::string frame = pending.substr(start, end_of_frame - start);
    pending.erase(0, end_of_frame + 1);
    std::size_t index = 0;
    for (const char* at = frame.data(); at < frame.data() + frame.size() && index < bars.size();) {
      int value = 0;
      const auto [end, error] = std::from_chars(at, frame.data() + frame.size(), value);
      if (error != std::errc{}) break;
      bars[index++] = std::clamp(static_cast<float>(value) / max_value, 0.0F, 1.0F);
      at = end + 1;  // past the ';'
    }
    return index > 0;
  }

  ~Cava() {
    if (pid > 0) {
      kill(pid, SIGTERM);
      waitpid(pid, nullptr, 0);
    }
    if (fd >= 0) close(fd);
    if (quiet_writer >= 0) close(quiet_writer);
  }
};

}  // namespace

int main() {
  const auto config = write_config();
  Cava cava;
  cava.start(config);

  vw::App app([&](vw::Ui& ui) {
    constexpr float width = 420;
    constexpr float height = 120;
    constexpr float radius = 28;
    constexpr float gap = 5;
    // How far the bars' box reaches past the card on each side.
    constexpr float bleed = 18;
    const float bar_width = (width + 2 * bleed - gap * (bar_count - 1)) / bar_count;

    const vw::Widget card = ui.rectangle({.width = width, .height = height, .radius = radius,
                                          .color = vw::rgba(0x151a1ee6), .border_width = 1,
                                          .border_color = vw::rgba(0xffffff14)});
    vw::Widget content;
    if (cava.missing) {
      content = ui.text({.text = "cava is not running (is it installed?)", .font = "sans", .size = 14,
                         .color = vw::rgb(0xa5acb3)});
    } else {
      std::vector<vw::Widget> bars;
      for (int index = 0; index < bar_count; ++index) {
        // From below the card's bottom edge to up to 1.4x its height.
        const float level = cava.bars[static_cast<std::size_t>(index)];
        const float bar_height = bleed + std::max(bar_width, level * height * 1.4F);
        const float hue = static_cast<float>(index) / (bar_count - 1);
        bars.push_back(ui.rectangle({.width = bar_width, .height = bar_height, .radius = bar_width / 2,
                                     .color = vw::Color{0.65F + 0.25F * hue, 0.80F - 0.25F * hue, 0.95F, 1.0F},
                                     .color2 = vw::Color{0.40F + 0.20F * hue, 0.55F, 0.90F - 0.20F * hue, 1.0F},
                                     .gradient = vw::Gradient::vertical}));
      }
      // A box larger than the card, centred on it: the stack below cuts
      // what lies outside.
      content = ui.row(bars, {.gap = gap, .width = width + 2 * bleed, .height = height + 2 * bleed,
                              .align = vw::Align::end});
    }
    // The card clips everything on it to its rounded shape.
    ui.stack({card, content}, {.width = width, .height = height, .radius = radius});

    ui.surface({.anchor = vw::anchor::bottom, .layer = vw::Layer::bottom, .margin = {0, 0, 48, 0},
                .surface_namespace = "vw-cava", .input_regions = std::vector<std::array<std::int32_t, 4>>{}});
  });
  if (cava.fd >= 0) app.on_event(cava.fd, [&] { return cava.drain(); });
  const int result = app.run();
  std::filesystem::remove(config);
  return result;
}
