#include "skate3_linux_file_picker.h"

#include <cerrno>
#include <cstdlib>
#include <string>
#include <vector>

#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gtk/gtk.h>

#include <rex/logging.h>

extern char** environ;

namespace skate3 {

namespace {

// GTK is only initialised when the first dialog is requested. Nothing else in
// the process initialises it: the SDK used to call gtk_init_check from its own
// GTK windowing code, but since its windowing moved to SDL3 that call is gone,
// and any GTK widget created without it aborts with "Can't create a
// GtkStyleContext without a display connection" (KI-102). Under Wayland the
// libdecor GTK plugin initialises GTK as a side effect, which hid the problem.
// gtk_init_check is idempotent, so running after that plugin is harmless.
bool EnsureGtkInitialised() {
  static const bool initialised = []() {
    // Keep GTK from calling setlocale(LC_ALL, ""), which would change the
    // numeric locale of the whole process; the SDK did the same before.
    gtk_disable_setlocale();
    const bool ok = gtk_init_check(nullptr, nullptr) != FALSE;
    if (!ok) {
      REXLOG_WARN("GTK could not open a display; the built-in file chooser is unavailable");
    }
    return ok;
  }();
  return initialised;
}

std::filesystem::path PickWithGtk(const LinuxFilePickerRequest& request) {
  GtkWidget* dialog = gtk_file_chooser_dialog_new(
      request.title.c_str(), nullptr, GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel",
      GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, nullptr);
  if (!dialog) {
    return {};
  }

  if (!request.filter_patterns.empty()) {
    GtkFileFilter* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, request.filter_name.c_str());
    for (const auto& pattern : request.filter_patterns) {
      gtk_file_filter_add_pattern(filter, pattern.c_str());
    }
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
  }

  GtkFileFilter* all_filter = gtk_file_filter_new();
  gtk_file_filter_set_name(all_filter, "All files");
  gtk_file_filter_add_pattern(all_filter, "*");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), all_filter);

  std::filesystem::path result;
  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
    char* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    if (filename) {
      result = filename;
      g_free(filename);
    }
  }

  gtk_widget_destroy(dialog);
  while (gtk_events_pending()) {
    gtk_main_iteration_do(FALSE);
  }
  return result;
}

enum class ExternalResult {
  kUnavailable,  // the program could not be started
  kCancelled,    // it ran but chose nothing (cancel, or it failed to show)
  kPicked,       // `output` holds the chosen path
};

// Runs argv[0] (searched on PATH) with the arguments exactly as given, with no
// shell involved, and captures its standard output.
ExternalResult RunExternalPicker(const std::vector<std::string>& args, std::string& output) {
  output.clear();
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) != 0) {
    return ExternalResult::kUnavailable;
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);

  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (const auto& arg : args) {
    argv.push_back(const_cast<char*>(arg.c_str()));
  }
  argv.push_back(nullptr);

  pid_t pid = 0;
  const int spawn_error = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(fds[1]);
  if (spawn_error != 0) {
    close(fds[0]);
    return ExternalResult::kUnavailable;
  }

  char buffer[4096];
  constexpr size_t kMaxOutput = 1 << 16;
  while (output.size() < kMaxOutput) {
    const ssize_t count = read(fds[0], buffer, sizeof(buffer));
    if (count > 0) {
      output.append(buffer, static_cast<size_t>(count));
    } else if (count < 0 && errno == EINTR) {
      continue;
    } else {
      break;
    }
  }
  close(fds[0]);

  int status = 0;
  pid_t waited;
  do {
    waited = waitpid(pid, &status, 0);
  } while (waited < 0 && errno == EINTR);
  const bool exited_ok = waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;

  // Only the first line is a path; both tools print one selection per line.
  const auto newline = output.find('\n');
  if (newline != std::string::npos) {
    output.resize(newline);
  }
  while (!output.empty() && output.back() == '\r') {
    output.pop_back();
  }
  if (!exited_ok || output.empty()) {
    return ExternalResult::kCancelled;
  }
  return ExternalResult::kPicked;
}

std::string JoinPatterns(const std::vector<std::string>& patterns) {
  std::string joined;
  for (const auto& pattern : patterns) {
    if (!joined.empty()) {
      joined += ' ';
    }
    joined += pattern;
  }
  return joined;
}

// Returns false if neither zenity nor kdialog could be started.
bool PickWithExternalTool(const LinuxFilePickerRequest& request, std::filesystem::path& result) {
  std::string output;

  std::vector<std::string> zenity = {"zenity", "--file-selection", "--title=" + request.title};
  if (!request.filter_patterns.empty()) {
    zenity.push_back("--file-filter=" + request.filter_name + " | " +
                     JoinPatterns(request.filter_patterns));
  }
  zenity.push_back("--file-filter=All files | *");
  switch (RunExternalPicker(zenity, output)) {
    case ExternalResult::kPicked:
      result = output;
      return true;
    case ExternalResult::kCancelled:
      return true;
    case ExternalResult::kUnavailable:
      break;
  }

  const char* home = std::getenv("HOME");
  std::string filter;
  if (!request.filter_patterns.empty()) {
    filter = JoinPatterns(request.filter_patterns) + "|" + request.filter_name + "\n";
  }
  filter += "*|All files";
  std::vector<std::string> kdialog = {"kdialog", "--title", request.title, "--getopenfilename",
                                      (home && *home) ? home : ".", filter};
  switch (RunExternalPicker(kdialog, output)) {
    case ExternalResult::kPicked:
      result = output;
      return true;
    case ExternalResult::kCancelled:
      return true;
    case ExternalResult::kUnavailable:
      break;
  }
  return false;
}

}  // namespace

std::filesystem::path PickFileLinux(const LinuxFilePickerRequest& request) {
  if (EnsureGtkInitialised()) {
    return PickWithGtk(request);
  }

  REXLOG_WARN("Trying zenity or kdialog as the file chooser instead");
  std::filesystem::path result;
  if (PickWithExternalTool(request, result)) {
    return result;
  }

  REXLOG_ERROR(
      "No file chooser could be shown (GTK has no display, and neither zenity nor kdialog is "
      "installed). Install zenity (for example `sudo apt install zenity`), or skip the dialog by "
      "launching with {}=/full/path/to/file set in the environment.",
      request.env_var);
  return {};
}

}  // namespace skate3
