// Exercise the actual GTK open/save panels, including UTF-8 paths and cancel.
// Run under a graphical session or: xvfb-run -a piet_linux_dialog_smoke
#include "piet/shell.h"
#include <gtk/gtk.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

struct Choice {
  std::string path;
  bool cancel;
  unsigned attempts = 0;
};

static gboolean choose(gpointer data) {
  auto& choice = *static_cast<Choice*>(data);
  if (++choice.attempts > 100) {
    std::cerr << "GTK file chooser did not complete\n";
    std::exit(1);
  }
  GList* windows = gtk_window_list_toplevels();
  GtkWidget* dialog = nullptr;
  for (GList* p = windows; p; p = p->next) {
    if (GTK_IS_FILE_CHOOSER(p->data)) dialog = GTK_WIDGET(p->data);
  }
  g_list_free(windows);
  if (!dialog) return G_SOURCE_CONTINUE;
  auto* chooser = GTK_FILE_CHOOSER(dialog);
  if (choice.cancel) {
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    return G_SOURCE_REMOVE;
  }
  if (choice.attempts == 1) {
    const auto path = std::filesystem::path(choice.path);
    gtk_file_chooser_set_current_folder(chooser, path.parent_path().c_str());
    if (gtk_file_chooser_get_action(chooser) == GTK_FILE_CHOOSER_ACTION_SAVE)
      gtk_file_chooser_set_current_name(chooser, path.filename().c_str());
    else
      gtk_file_chooser_select_filename(chooser, path.c_str());
    return G_SOURCE_CONTINUE;
  }
  char* selected = gtk_file_chooser_get_filename(chooser);
  const bool ready = selected && choice.path == selected;
  g_free(selected);
  if (!ready) return G_SOURCE_CONTINUE;
  gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
  return G_SOURCE_REMOVE;
}

int main() {
  gchar* scratch = g_dir_make_tmp("piet-dialog-XXXXXX", nullptr);
  if (!scratch) return 1;
  const std::filesystem::path folder(scratch);
  g_free(scratch);
  piet::Shell shell(false);
  Choice open{(folder / "한글 data.csv").string(), false};
  std::ofstream(open.path) << "x\n1\n";
  g_timeout_add(100, choose, &open);
  const bool opened = shell.openFileDialog("dta,csv,hbin") == open.path;
  Choice cancel{"", true};
  g_timeout_add(100, choose, &cancel);
  const bool openCancelled = shell.openFileDialog("").empty();
  Choice save{(folder / "저장 result.do").string(), false};
  g_timeout_add(100, choose, &save);
  const bool saved = shell.saveFileDialog("example.do", "do") == save.path;
  cancel.attempts = 0;
  g_timeout_add(100, choose, &cancel);
  const bool saveCancelled = shell.saveFileDialog("example.do", "do").empty();
  std::filesystem::remove_all(folder);
  if (!(opened && openCancelled && saved && saveCancelled)) {
    std::cerr << "GTK dialogs: FAIL\n";
    return 1;
  }
  std::cout << "GTK dialogs: OK (open/save UTF-8 paths, open/save cancellation)\n";
}
