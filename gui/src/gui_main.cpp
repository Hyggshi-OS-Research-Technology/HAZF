#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <gio/gio.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <string>
#include <unordered_set>
#include <algorithm>
#include <thread>
#include <mutex>

#include <zstd.h>
#include "hazf/format.hpp"

namespace fs = std::filesystem;
using namespace hazf;

// ---------------------------------------------------------------------------
// App State
// ---------------------------------------------------------------------------
struct AppState {
    GtkApplication *app = nullptr;
    GtkWindow *window = nullptr;
    GtkStack *stack = nullptr;
    GtkStackSwitcher *switcher = nullptr;

    // Create page
    GtkWidget *create_drop_area = nullptr;
    GtkWidget *create_file_listbox = nullptr;
    GtkWidget *create_archive_entry = nullptr; // GtkEntry for archive path (with button)
    GtkWidget *create_archive_choose_btn = nullptr;
    GtkWidget *create_create_btn = nullptr;
    GtkWidget *create_clear_btn = nullptr;
    GtkWidget *create_progress = nullptr;
    GtkWidget *create_status = nullptr;
    GtkWidget *create_count_label = nullptr;
    std::vector<std::string> create_files; // list of added files (absolute or relative)
    std::string create_archive_path;

    // Browse page
    GtkWidget *browse_archive_entry = nullptr;
    GtkWidget *browse_choose_btn = nullptr;
    GtkWidget *browse_open_btn = nullptr;
    GtkWidget *browse_columnview = nullptr;
    GtkWidget *browse_list_container = nullptr; // scroll
    GtkWidget *browse_status = nullptr;
    GtkWidget *browse_search_entry = nullptr;
    GtkStringList *browse_store = nullptr; // we will use custom, but simple ListBox for now
    GtkWidget *browse_listbox = nullptr;
    std::string browse_archive_path;
    std::vector<std::tuple<std::string, uint64_t, uint64_t, uint32_t>> browse_entries;

    // Extract page
    GtkWidget *extract_archive_entry = nullptr;
    GtkWidget *extract_archive_btn = nullptr;
    GtkWidget *extract_dest_entry = nullptr;
    GtkWidget *extract_dest_btn = nullptr;
    GtkWidget *extract_btn = nullptr;
    GtkWidget *extract_progress = nullptr;
    GtkWidget *extract_status = nullptr;
    std::string extract_archive_path;
    std::string extract_dest_path;

    // Info / Test page
    GtkWidget *info_archive_entry = nullptr;
    GtkWidget *info_choose_btn = nullptr;
    GtkWidget *info_test_btn = nullptr;
    GtkWidget *info_box = nullptr; // container for info labels
    GtkWidget *info_status = nullptr;
    GtkWidget *info_textview = nullptr;
    GtkTextBuffer *info_buffer = nullptr;
    std::string info_archive_path;
};

static AppState *g_state = nullptr;

// ---------------------------------------------------------------------------
// CSS
// ---------------------------------------------------------------------------
static const char *kCss = R"CSS(
window { background: linear-gradient(180deg, #fafafa 0%, #f0f0f0 100%); }
headerbar { background: linear-gradient(180deg, #ffffff 0%, #f6f6f6 100%); border-bottom: 1px solid alpha(#000, 0.08); box-shadow: 0 1px 3px alpha(#000,0.08); }
.stack-switcher button { border-radius: 999px; padding: 6px 16px; transition: all 200ms cubic-bezier(0.25,0.46,0.45,0.94); }
.stack-switcher button:checked { background: #3584e4; color: white; box-shadow: 0 2px 8px alpha(#3584e4,0.35); }
.drop-area { border: 2.5px dashed alpha(@borders,0.55); border-radius: 16px; background: alpha(white,0.7); transition: all 250ms cubic-bezier(0.25,0.46,0.45,0.94); box-shadow: 0 2px 12px alpha(black,0.04); }
.drop-area.drag-over { border-color: #3584e4; background: alpha(#3584e4,0.08); box-shadow: 0 8px 24px alpha(#3584e4,0.22), inset 0 0 0 1px alpha(#3584e4,0.15); }
.drop-icon { color: alpha(@window_fg_color,0.55); transition: all 250ms ease; }
.drop-area.drag-over .drop-icon { color: #3584e4; }
.drop-title { font-size: 15px; font-weight: 600; }
.drop-subtitle { font-size: 13px; color: alpha(@window_fg_color,0.65); }
.file-row { border-radius: 10px; padding: 8px 10px; margin: 2px 0; transition: all 180ms ease; border: 1px solid transparent; }
.file-row:hover { background: alpha(#3584e4,0.08); border-color: alpha(#3584e4,0.12); box-shadow: 0 2px 8px alpha(black,0.04); }
.card { background: white; border-radius: 12px; border: 1px solid alpha(@borders,0.5); box-shadow: 0 2px 12px alpha(black,0.06); padding: 16px; }
.card:hover { box-shadow: 0 4px 20px alpha(black,0.09); }
button.pill { border-radius: 999px; padding: 8px 20px; font-weight: 600; transition: all 200ms cubic-bezier(0.25,0.46,0.45,0.94); }
button.suggested-action { background: linear-gradient(180deg, #3584e4 0%, #1c71d8 100%); color: white; box-shadow: 0 2px 10px alpha(#3584e4,0.35); }
button.suggested-action:hover { background: linear-gradient(180deg, #4891e7 0%, #1c71d8 100%); box-shadow: 0 4px 16px alpha(#3584e4,0.45); }
progressbar trough { border-radius: 999px; background: alpha(@borders,0.35); min-height: 6px; }
progressbar progress { border-radius: 999px; background: linear-gradient(90deg, #3584e4 0%, #62a0ea 100%); }
.status-ok { background: alpha(#26a269,0.12); color: #26a269; border-radius: 999px; padding: 2px 8px; font-size: 11px; font-weight: 600; }
.status-fail { background: alpha(#c01c28,0.12); color: #c01c28; border-radius: 999px; padding: 2px 8px; font-size: 11px; font-weight: 600; }
)CSS";

// ---------------------------------------------------------------------------
// Helpers: icon for file
// ---------------------------------------------------------------------------
static const char* icon_for_path(const std::string& path) {
    auto lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.ends_with(".txt") || lower.ends_with(".md") || lower.ends_with(".log")) return "text-x-generic-symbolic";
    if (lower.ends_with(".png") || lower.ends_with(".jpg") || lower.ends_with(".jpeg") || lower.ends_with(".svg") || lower.ends_with(".webp")) return "image-x-generic-symbolic";
    if (lower.ends_with(".mp3") || lower.ends_with(".wav") || lower.ends_with(".ogg") || lower.ends_with(".flac")) return "audio-x-generic-symbolic";
    if (lower.ends_with(".mp4") || lower.ends_with(".mkv") || lower.ends_with(".avi")) return "video-x-generic-symbolic";
    if (lower.ends_with(".pdf")) return "x-office-document-symbolic";
    if (lower.ends_with(".zip") || lower.ends_with(".hazf") || lower.ends_with(".tar") || lower.ends_with(".gz")) return "package-x-generic-symbolic";
    if (lower.find('/') != std::string::npos) {
        // check if looks like folder? generic
        if (lower.ends_with("/")) return "folder-symbolic";
    }
    return "text-x-generic-symbolic";
}

static std::string format_size(uint64_t b) {
    if (b < 1024) return std::to_string(b) + " B";
    if (b < 1024*1024) {
        char buf[32]; snprintf(buf,sizeof(buf),"%.1f KiB", b/1024.0); return buf;
    }
    if (b < 1024ULL*1024*1024) {
        char buf[32]; snprintf(buf,sizeof(buf),"%.1f MiB", b/(1024.0*1024)); return buf;
    }
    char buf[32]; snprintf(buf,sizeof(buf),"%.2f GiB", b/(1024.0*1024*1024)); return buf;
}

// ---------------------------------------------------------------------------
// HAZF helpers for GUI (duplicate of CLI core but adapted)
// ---------------------------------------------------------------------------
struct GuiCollected { fs::path fsPath; std::string stored; };

static std::vector<GuiCollected> gui_collect(const std::vector<std::string>& inputs, std::string &err) {
    std::vector<GuiCollected> out;
    std::unordered_set<std::string> seen;
    try {
        for (auto &raw : inputs) {
            fs::path p(raw);
            if (!fs::exists(p)) throw std::runtime_error("Not found: " + raw);
            if (fs::is_regular_file(p)) {
                std::string stored;
                if (p.is_absolute()) stored = p.filename().generic_string();
                else {
                    stored = normalize_input_path(p);
                    if (stored.empty()) stored = p.filename().generic_string();
                    stored = normalize_stored_path_string(stored);
                }
                if (stored.empty()) stored = p.filename().generic_string();
                stored = normalize_stored_path_string(stored);
                if (!is_valid_stored_path(stored)) throw std::runtime_error("Invalid path: " + stored);
                if (seen.count(stored)) throw std::runtime_error("Duplicate: " + stored);
                seen.insert(stored);
                out.push_back({p, stored});
            } else if (fs::is_directory(p)) {
                std::string base;
                if (p.is_absolute()) {
                    base = p.filename().generic_string();
                    if (base=="." || base.empty()) base="";
                    else base = normalize_stored_path_string(base);
                } else base = normalize_input_path(p);
                for (auto it = fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied); it != fs::recursive_directory_iterator(); ++it) {
                    if (!it->is_regular_file()) continue;
                    fs::path fp = it->path();
                    fs::path rel = fs::relative(fp, p);
                    std::string rs = normalize_stored_path_string(rel.generic_string());
                    if (rs.empty()) continue;
                    std::string stored = base.empty()? rs : base + "/" + rs;
                    stored = normalize_stored_path_string(stored);
                    if (!is_valid_stored_path(stored)) throw std::runtime_error("Invalid: " + stored);
                    if (seen.count(stored)) throw std::runtime_error("Duplicate: " + stored);
                    seen.insert(stored);
                    out.push_back({fp, stored});
                }
            }
        }
        if (out.size() > kMaxEntries) throw std::runtime_error("Too many entries");
        std::sort(out.begin(), out.end(), [](auto &a, auto &b){return a.stored < b.stored;});
    } catch (std::exception &e) { err = e.what(); return {}; }
    return out;
}

// ---------------------------------------------------------------------------
// UI helpers
// ---------------------------------------------------------------------------
static void show_toast(GtkWindow *win, const char *msg, int timeout_ms=3000) {
    // Simple info bar via dialog for now; in GTK4 we could use AdwToast
    // We'll set status label instead; this helper shows a transient dialog
    GtkWidget *dlg = gtk_message_dialog_new(win, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "%s", msg);
    gtk_window_set_title(GTK_WINDOW(dlg), "HAZF");
    // auto close after timeout? just show and user closes
    g_timeout_add(timeout_ms, [](gpointer d)->gboolean { gtk_window_destroy(GTK_WINDOW(d)); return G_SOURCE_REMOVE; }, dlg);
    gtk_window_present(GTK_WINDOW(dlg));
}

static void set_status(GtkWidget *label, const std::string &text, bool is_error=false) {
    gtk_label_set_text(GTK_LABEL(label), text.c_str());
    if (is_error) {
        gtk_widget_add_css_class(label, "status-fail");
        gtk_widget_remove_css_class(label, "status-ok");
    } else {
        gtk_widget_remove_css_class(label, "status-fail");
        // gtk_widget_add_css_class(label, "status-ok");
    }
}

// Update file listbox for create page
static void refresh_create_list(AppState *st) {
    // clear
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(st->create_file_listbox)) != nullptr) {
        gtk_list_box_remove(GTK_LIST_BOX(st->create_file_listbox), child);
    }
    for (size_t i=0;i<st->create_files.size();++i) {
        auto &p = st->create_files[i];
        fs::path fp(p);
        std::string name = fp.filename().string();
        if (name.empty()) name = p;
        std::string dir = fp.parent_path().string();
        uint64_t sz = 0;
        std::string szStr = "?";
        try { sz = fs::is_regular_file(fp) ? fs::file_size(fp) : 0; szStr = format_size(sz); } catch(...) {}
        bool isDir = fs::is_directory(fp);

        GtkWidget *row = gtk_list_box_row_new();
        gtk_widget_add_css_class(row, "file-row");
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_top(box, 4); gtk_widget_set_margin_bottom(box, 4);
        gtk_widget_set_margin_start(box, 6); gtk_widget_set_margin_end(box, 6);

        const char* iconName = isDir ? "folder-symbolic" : icon_for_path(p);
        GtkWidget *icon = gtk_image_new_from_icon_name(iconName);
        gtk_widget_add_css_class(icon, "file-icon");
        gtk_image_set_pixel_size(GTK_IMAGE(icon), 20);
        gtk_box_append(GTK_BOX(box), icon);

        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
        gtk_widget_set_hexpand(vbox, TRUE);
        GtkWidget *nameLabel = gtk_label_new(name.c_str());
        gtk_widget_add_css_class(nameLabel, "file-name");
        gtk_label_set_xalign(GTK_LABEL(nameLabel), 0);
        gtk_label_set_ellipsize(GTK_LABEL(nameLabel), PANGO_ELLIPSIZE_MIDDLE);
        gtk_box_append(GTK_BOX(vbox), nameLabel);
        std::string meta = dir + "  •  " + szStr;
        if (isDir) {
            // count files inside
            size_t cnt=0; for(auto it=fs::recursive_directory_iterator(fp, fs::directory_options::skip_permission_denied); it!=fs::recursive_directory_iterator(); ++it) if(it->is_regular_file()) cnt++;
            meta = dir + "  •  " + std::to_string(cnt) + " files";
        }
        GtkWidget *metaLabel = gtk_label_new(meta.c_str());
        gtk_widget_add_css_class(metaLabel, "file-meta");
        gtk_label_set_xalign(GTK_LABEL(metaLabel), 0);
        gtk_label_set_ellipsize(GTK_LABEL(metaLabel), PANGO_ELLIPSIZE_MIDDLE);
        gtk_box_append(GTK_BOX(vbox), metaLabel);
        gtk_box_append(GTK_BOX(box), vbox);

        // Remove button
        GtkWidget *removeBtn = gtk_button_new_from_icon_name("window-close-symbolic");
        gtk_widget_add_css_class(removeBtn, "flat");
        gtk_widget_add_css_class(removeBtn, "icon-button");
        gtk_widget_set_valign(removeBtn, GTK_ALIGN_CENTER);
        size_t idx = i;
        g_signal_connect(removeBtn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data){
            AppState *s = (AppState*)data;
            // find which row? we captured idx but after removal indices shift; instead find via pointer
            // We'll handle by re-searching: remove by index captured at creation time is stale after removals
            // So we store index pointer and search
        }), st);
        // Instead use closure with index capture via lambda needing careful
        // We'll do a hack: store index as widget data and lookup file path
        g_object_set_data(G_OBJECT(removeBtn), "idx", GINT_TO_POINTER((int)idx));
        g_signal_connect(removeBtn, "clicked", G_CALLBACK(+[](GtkButton *btn, gpointer data){
            AppState *s = (AppState*)data;
            int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "idx"));
            if (idx >=0 && (size_t)idx < s->create_files.size()) {
                s->create_files.erase(s->create_files.begin()+idx);
                refresh_create_list(s);
                std::string t = std::to_string(s->create_files.size()) + " files selected";
                gtk_label_set_text(GTK_LABEL(s->create_count_label), t.c_str());
                gtk_widget_set_sensitive(s->create_create_btn, !s->create_files.empty() && !s->create_archive_path.empty());
            }
        }), st);
        gtk_box_append(GTK_BOX(box), removeBtn);

        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
        gtk_list_box_append(GTK_LIST_BOX(st->create_file_listbox), row);
    }
    std::string t = std::to_string(st->create_files.size()) + " files selected";
    if (st->create_files.empty()) t = "No files selected — drag & drop or click Add";
    gtk_label_set_text(GTK_LABEL(st->create_count_label), t.c_str());
}

// ---------------------------------------------------------------------------
// File choosers (GTK 4.10 FileDialog or fallback)
// ---------------------------------------------------------------------------
static void choose_archive_save(AppState *st, GtkWindow *parent, std::function<void(std::string)> cb) {
#if GTK_CHECK_VERSION(4,10,0)
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Choose HAZF Archive");
    gtk_file_dialog_set_accept_label(dlg, "Select");
    GtkFileFilter *f = gtk_file_filter_new();
    gtk_file_filter_set_name(f, "HAZF Archive (*.hazf)");
    gtk_file_filter_add_pattern(f, "*.hazf");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, f);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    gtk_file_dialog_set_default_filter(dlg, f);
    gtk_file_dialog_save(dlg, parent, nullptr, [](GObject *obj, GAsyncResult *res, gpointer data){
        auto *func = (std::function<void(std::string)>*)data;
        GError *err=nullptr;
        GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(obj), res, &err);
        if (file) {
            char *path = g_file_get_path(file);
            if (path) { (*func)(std::string(path)); g_free(path); }
            g_object_unref(file);
        }
        delete func;
    }, new std::function<void(std::string)>(cb));
#else
    GtkWidget *dlg = gtk_file_chooser_native_new("Choose HAZF Archive", parent, GTK_FILE_CHOOSER_ACTION_SAVE, "_Save", "_Cancel");
    GtkFileFilter *f = gtk_file_filter_new();
    gtk_file_filter_set_name(f, "HAZF Archive (*.hazf)");
    gtk_file_filter_add_pattern(f, "*.hazf");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), f);
    g_signal_connect(dlg, "response", G_CALLBACK(+[](GtkNativeDialog *d, int resp, gpointer data){
        auto *func = (std::function<void(std::string)>*)data;
        if (resp == GTK_RESPONSE_ACCEPT) {
            char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));
            if (path) { (*func)(std::string(path)); g_free(path); }
        }
        g_object_unref(d);
        delete func;
    }), new std::function<void(std::string)>(cb));
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(dlg));
#endif
}

static void choose_archive_open(AppState *st, GtkWindow *parent, std::function<void(std::string)> cb) {
#if GTK_CHECK_VERSION(4,10,0)
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Open HAZF Archive");
    GtkFileFilter *f = gtk_file_filter_new();
    gtk_file_filter_set_name(f, "HAZF Archive (*.hazf)");
    gtk_file_filter_add_pattern(f, "*.hazf");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, f);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    gtk_file_dialog_open(dlg, parent, nullptr, [](GObject *obj, GAsyncResult *res, gpointer data){
        auto *func = (std::function<void(std::string)>*)data;
        GError *err=nullptr;
        GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(obj), res, &err);
        if (file) {
            char *path = g_file_get_path(file);
            if (path) { (*func)(std::string(path)); g_free(path); }
            g_object_unref(file);
        }
        delete func;
    }, new std::function<void(std::string)>(cb));
#else
    GtkWidget *dlg = gtk_file_chooser_native_new("Open HAZF Archive", parent, GTK_FILE_CHOOSER_ACTION_OPEN, "_Open", "_Cancel");
    GtkFileFilter *f = gtk_file_filter_new();
    gtk_file_filter_set_name(f, "HAZF Archive (*.hazf)");
    gtk_file_filter_add_pattern(f, "*.hazf");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), f);
    g_signal_connect(dlg, "response", G_CALLBACK(+[](GtkNativeDialog *d, int resp, gpointer data){
        auto *func = (std::function<void(std::string)>*)data;
        if (resp == GTK_RESPONSE_ACCEPT) {
            char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));
            if (path) { (*func)(std::string(path)); g_free(path); }
        }
        g_object_unref(d);
        delete func;
    }), new std::function<void(std::string)>(cb));
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(dlg));
#endif
}

static void choose_files_open(AppState *st, GtkWindow *parent, std::function<void(std::vector<std::string>)> cb) {
#if GTK_CHECK_VERSION(4,10,0)
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Add Files or Folders");
    // allow multiple? GtkFileDialog doesn't multi; fallback to native for multi
    // Use native for multi
    GtkWidget *native = gtk_file_chooser_native_new("Add Files or Folders", parent, GTK_FILE_CHOOSER_ACTION_OPEN, "_Add", "_Cancel");
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(native), TRUE);
    g_signal_connect(native, "response", G_CALLBACK(+[](GtkNativeDialog *d, int resp, gpointer data){
        auto *func = (std::function<void(std::vector<std::string>)>*)data;
        if (resp==GTK_RESPONSE_ACCEPT) {
            GSList *files = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(d));
            std::vector<std::string> out;
            for (GSList *l=files;l;l=l->next) { out.emplace_back((char*)l->data); g_free(l->data); }
            g_slist_free(files);
            (*func)(out);
        }
        g_object_unref(d);
        delete func;
    }), new std::function<void(std::vector<std::string>)>(cb));
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(native));
#else
    GtkWidget *dlg = gtk_file_chooser_native_new("Add Files or Folders", parent, GTK_FILE_CHOOSER_ACTION_OPEN, "_Add", "_Cancel");
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dlg), TRUE);
    g_signal_connect(dlg, "response", G_CALLBACK(+[](GtkNativeDialog *d, int resp, gpointer data){
        auto *func = (std::function<void(std::vector<std::string>)>*)data;
        if (resp==GTK_RESPONSE_ACCEPT) {
            GSList *files = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(d));
            std::vector<std::string> out;
            for (GSList *l=files;l;l=l->next) { out.emplace_back((char*)l->data); g_free(l->data); }
            g_slist_free(files);
            (*func)(out);
        }
        g_object_unref(d);
        delete func;
    }), new std::function<void(std::vector<std::string>)>(cb));
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(dlg));
#endif
}

static void choose_folder(AppState *st, GtkWindow *parent, std::function<void(std::string)> cb) {
#if GTK_CHECK_VERSION(4,10,0)
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Choose Folder");
    gtk_file_dialog_select_folder(dlg, parent, nullptr, [](GObject *obj, GAsyncResult *res, gpointer data){
        auto *func=(std::function<void(std::string)>*)data;
        GError *err=nullptr;
        GFile *file=gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(obj),res,&err);
        if(file){ char *p=g_file_get_path(file); if(p){(*func)(std::string(p)); g_free(p);} g_object_unref(file);}
        delete func;
    }, new std::function<void(std::string)>(cb));
#else
    GtkWidget *dlg = gtk_file_chooser_native_new("Choose Folder", parent, GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "_Select", "_Cancel");
    g_signal_connect(dlg, "response", G_CALLBACK(+[](GtkNativeDialog *d,int resp,gpointer data){
        auto *func=(std::function<void(std::string)>*)data;
        if(resp==GTK_RESPONSE_ACCEPT){ char *p=gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d)); if(p){(*func)(std::string(p)); g_free(p);} }
        g_object_unref(d); delete func;
    }), new std::function<void(std::string)>(cb));
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(dlg));
#endif
}

// ---------------------------------------------------------------------------
// Create operation (threaded)
// ---------------------------------------------------------------------------
struct CreateJob {
    AppState *st;
    std::vector<std::string> files;
    std::string archive;
    std::string error;
    bool success=false;
};

static gboolean create_done(gpointer data) {
    CreateJob *job = (CreateJob*)data;
    AppState *st = job->st;
    gtk_widget_set_sensitive(st->create_create_btn, TRUE);
    gtk_widget_set_sensitive(st->create_clear_btn, TRUE);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(st->create_progress), job->success?1.0:0.0);
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(st->create_progress), FALSE);
    if (job->success) {
        set_status(st->create_status, "✓ Archive created successfully: " + job->archive);
        // also show dialog
        GtkWidget *dlg = gtk_message_dialog_new(st->window, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "Archive created:\n%s", job->archive.c_str());
        gtk_window_present(GTK_WINDOW(dlg));
        g_signal_connect(dlg, "response", G_CALLBACK(+[](GtkDialog *d, int, gpointer){ gtk_window_destroy(GTK_WINDOW(d)); }), nullptr);
    } else {
        set_status(st->create_status, "✗ Failed: " + job->error, true);
        GtkWidget *dlg = gtk_message_dialog_new(st->window, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Failed to create archive:\n%s", job->error.c_str());
        gtk_window_present(GTK_WINDOW(dlg));
        g_signal_connect(dlg, "response", G_CALLBACK(+[](GtkDialog *d, int, gpointer){ gtk_window_destroy(GTK_WINDOW(d)); }), nullptr);
    }
    delete job;
    return G_SOURCE_REMOVE;
}

static gpointer create_thread(gpointer data) {
    CreateJob *job = (CreateJob*)data;
    std::string err;
    auto collected = gui_collect(job->files, err);
    if (!err.empty()) { job->error = err; g_idle_add(create_done, job); return nullptr; }
    if (collected.empty()) { job->error="No regular files found"; g_idle_add(create_done, job); return nullptr; }

    struct Prep { std::string path; std::vector<uint8_t> comp; uint64_t orig; uint32_t crc; };
    std::vector<Prep> preps;
    preps.reserve(collected.size());
    for (auto &ce: collected) {
        uint64_t fsize;
        try { fsize = fs::file_size(ce.fsPath);} catch(std::exception &e){ job->error = e.what(); g_idle_add(create_done,job); return nullptr; }
        if (fsize>kMaxFileSize){ job->error="File too large: " + ce.fsPath.string(); g_idle_add(create_done,job); return nullptr; }
        std::vector<uint8_t> data;
        data.resize((size_t)fsize);
        if (fsize>0){
            std::ifstream in(ce.fsPath, std::ios::binary);
            if(!in){ job->error="Cannot read: " + ce.fsPath.string(); g_idle_add(create_done,job); return nullptr; }
            in.read((char*)data.data(), fsize);
        }
        uint32_t crc = data.empty()?0:crc32(data.data(), data.size());
        size_t bound = ZSTD_compressBound(data.size());
        std::vector<uint8_t> comp(bound);
        size_t cSize = ZSTD_compress(comp.data(), bound, data.data(), data.size(), 3);
        if (ZSTD_isError(cSize)){ job->error = ZSTD_getErrorName(cSize); g_idle_add(create_done,job); return nullptr; }
        comp.resize(cSize);
        preps.push_back({ce.stored, std::move(comp), fsize, crc});
    }
    // write
    std::ofstream out(job->archive, std::ios::binary|std::ios::trunc);
    if(!out){ job->error="Cannot write archive"; g_idle_add(create_done,job); return nullptr; }
    ArchiveHeader ah; ah.version=kVersion; ah.reserved=0; ah.entryCount=(uint32_t)preps.size();
    auto ahB = ah.serialize();
    out.write((char*)ahB.data(), ahB.size());
    for (auto &p: preps){
        EntryHeader eh; eh.pathLen=(uint16_t)p.path.size(); eh.reserved=0; eh.originalSize=p.orig; eh.compressedSize=p.comp.size(); eh.crc32=p.crc;
        auto ehB = eh.serialize();
        out.write((char*)ehB.data(), ehB.size());
        out.write(p.path.data(), p.path.size());
        if(!p.comp.empty()) out.write((char*)p.comp.data(), p.comp.size());
    }
    out.close();
    job->success=true;
    g_idle_add(create_done, job);
    return nullptr;
}

// ---------------------------------------------------------------------------
// Browse / List
// ---------------------------------------------------------------------------
static void refresh_browse_list(AppState *st) {
    while (GtkWidget *c = gtk_widget_get_first_child(st->browse_listbox)) gtk_list_box_remove(GTK_LIST_BOX(st->browse_listbox), c);
    std::string filter;
    if (st->browse_search_entry) filter = gtk_editable_get_text(GTK_EDITABLE(st->browse_search_entry));
    std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);
    for (auto &e: st->browse_entries) {
        auto &path = std::get<0>(e);
        uint64_t orig = std::get<1>(e);
        uint64_t comp = std::get<2>(e);
        uint32_t crc = std::get<3>(e);
        if (!filter.empty()) {
            std::string low=path; std::transform(low.begin(), low.end(), low.begin(), ::tolower);
            if (low.find(filter)==std::string::npos) continue;
        }
        GtkWidget *row = gtk_list_box_row_new();
        gtk_widget_add_css_class(row, "file-row");
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_widget_set_margin_top(box,6); gtk_widget_set_margin_bottom(box,6);
        gtk_widget_set_margin_start(box,8); gtk_widget_set_margin_end(box,8);
        GtkWidget *icon = gtk_image_new_from_icon_name(icon_for_path(path));
        gtk_image_set_pixel_size(GTK_IMAGE(icon), 18);
        gtk_widget_add_css_class(icon, "file-icon");
        gtk_box_append(GTK_BOX(box), icon);
        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL,2);
        gtk_widget_set_hexpand(vbox, TRUE);
        GtkWidget *name = gtk_label_new(path.c_str());
        gtk_widget_add_css_class(name, "file-name");
        gtk_label_set_xalign(GTK_LABEL(name),0);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_MIDDLE);
        gtk_box_append(GTK_BOX(vbox), name);
        char meta[128];
        snprintf(meta,sizeof(meta), "%s  •  compressed %s  •  CRC %08x", format_size(orig).c_str(), format_size(comp).c_str(), crc);
        GtkWidget *metaL = gtk_label_new(meta);
        gtk_widget_add_css_class(metaL, "file-meta");
        gtk_label_set_xalign(GTK_LABEL(metaL),0);
        gtk_box_append(GTK_BOX(vbox), metaL);
        gtk_box_append(GTK_BOX(box), vbox);
        // size badge
        GtkWidget *badge = gtk_label_new(format_size(orig).c_str());
        gtk_widget_add_css_class(badge, "archive-badge");
        gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(box), badge);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
        gtk_list_box_append(GTK_LIST_BOX(st->browse_listbox), row);
    }
    if (st->browse_entries.empty()) {
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *label = gtk_label_new("No entries. Open a .hazf archive.");
        gtk_widget_add_css_class(label, "file-meta");
        gtk_widget_set_margin_top(label, 20); gtk_widget_set_margin_bottom(label,20);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
        gtk_list_box_append(GTK_LIST_BOX(st->browse_listbox), row);
    }
}

static gboolean browse_done(gpointer data) {
    AppState *st = (AppState*)data;
    gtk_widget_set_sensitive(st->browse_open_btn, TRUE);
    if (st->browse_entries.empty() && !st->browse_archive_path.empty()) {
        // Check if file exists but empty? Could be error handled elsewhere
    }
    refresh_browse_list(st);
    set_status(st->browse_status, "Loaded " + std::to_string(st->browse_entries.size()) + " entries from " + fs::path(st->browse_archive_path).filename().string());
    return G_SOURCE_REMOVE;
}

struct BrowseJob { AppState *st; std::string archive; std::string error; };

static gpointer browse_thread(gpointer data) {
    BrowseJob *job=(BrowseJob*)data;
    AppState *st=job->st;
    std::ifstream in;
    uint64_t fsize=0;
    ArchiveHeader ah;
    try {
        if(!fs::exists(job->archive)) throw std::runtime_error("Not found");
        fsize=fs::file_size(job->archive);
        in.open(job->archive, std::ios::binary);
        std::array<uint8_t,12> buf; read_exact(in, buf.data(),12);
        ah=ArchiveHeader::deserialize(buf.data());
        if(ah.version!=kVersion) throw std::runtime_error("Unsupported version");
    } catch(std::exception &e){
        std::string msg=e.what();
        g_idle_add([](gpointer d)->gboolean{ AppState *s=(AppState*)d; set_status(s->browse_status, "Failed to open archive", true); return G_SOURCE_REMOVE; }, st);
        delete job; return nullptr;
    }
    std::vector<std::tuple<std::string,uint64_t,uint64_t,uint32_t>> entries;
    for(uint32_t i=0;i<ah.entryCount;++i){
        std::array<uint8_t,24> ehB; read_exact(in, ehB.data(),24);
        EntryHeader eh=EntryHeader::deserialize(ehB.data());
        std::string path; path.resize(eh.pathLen);
        if(eh.pathLen) read_exact(in, (uint8_t*)path.data(), eh.pathLen);
        // skip payload
        if(eh.compressedSize) in.seekg(eh.compressedSize, std::ios::cur);
        entries.emplace_back(path, eh.originalSize, eh.compressedSize, eh.crc32);
    }
    st->browse_entries = std::move(entries);
    g_idle_add(browse_done, st);
    delete job;
    return nullptr;
}

static void do_browse_open(AppState *st) {
    if(st->browse_archive_path.empty()){ set_status(st->browse_status,"Choose an archive first",true); return; }
    gtk_widget_set_sensitive(st->browse_open_btn, FALSE);
    set_status(st->browse_status, "Loading...");
    BrowseJob *job=new BrowseJob{st, st->browse_archive_path};
    g_thread_new("browse", browse_thread, job);
}

// ---------------------------------------------------------------------------
// Extract
// ---------------------------------------------------------------------------
struct ExtractJob { AppState *st; std::string archive; std::string dest; std::string error; bool success=false; int extracted=0; };

static gboolean extract_done(gpointer data){
    ExtractJob *job=(ExtractJob*)data;
    AppState *st=job->st;
    gtk_widget_set_sensitive(st->extract_btn, TRUE);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(st->extract_progress), job->success?1.0:0.0);
    if(job->success) set_status(st->extract_status, "✓ Extracted " + std::to_string(job->extracted) + " files to " + job->dest);
    else set_status(st->extract_status, "✗ " + job->error, true);
    if(job->success){
        GtkWidget *dlg=gtk_message_dialog_new(st->window, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "Extracted %d files to\n%s", job->extracted, job->dest.c_str());
        gtk_window_present(GTK_WINDOW(dlg));
        g_signal_connect(dlg,"response",G_CALLBACK(+[](GtkDialog*d,int,gpointer){gtk_window_destroy(GTK_WINDOW(d));}),nullptr);
    } else if(!job->error.empty()){
        GtkWidget *dlg=gtk_message_dialog_new(st->window, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Extract failed:\n%s", job->error.c_str());
        gtk_window_present(GTK_WINDOW(dlg));
        g_signal_connect(dlg,"response",G_CALLBACK(+[](GtkDialog*d,int,gpointer){gtk_window_destroy(GTK_WINDOW(d));}),nullptr);
    }
    delete job;
    return G_SOURCE_REMOVE;
}

static gpointer extract_thread(gpointer data){
    ExtractJob *job=(ExtractJob*)data;
    // Use same logic as CLI extract but simplified
    std::ifstream in;
    uint64_t fsize=0;
    ArchiveHeader ah;
    try{
        if(!fs::exists(job->archive)) throw std::runtime_error("Archive not found");
        fsize=fs::file_size(job->archive);
        in.open(job->archive,std::ios::binary);
        std::array<uint8_t,12> buf; read_exact(in,buf.data(),12);
        ah=ArchiveHeader::deserialize(buf.data());
    }catch(std::exception &e){ job->error=e.what(); g_idle_add(extract_done,job); return nullptr; }
    if(!fs::exists(job->dest)) fs::create_directories(job->dest);
    fs::path outDir(job->dest);
    fs::path canon;
    try{ canon=fs::weakly_canonical(outDir);}catch(...){ canon=fs::absolute(outDir).lexically_normal();}
    int count=0;
    for(uint32_t i=0;i<ah.entryCount;++i){
        std::array<uint8_t,24> ehB; read_exact(in,ehB.data(),24);
        EntryHeader eh=EntryHeader::deserialize(ehB.data());
        std::string path; path.resize(eh.pathLen); if(eh.pathLen) read_exact(in,(uint8_t*)path.data(),eh.pathLen);
        std::vector<uint8_t> comp(eh.compressedSize);
        if(eh.compressedSize) read_exact(in,comp.data(),comp.size());
        if(!is_valid_stored_path(path)){ job->error="Invalid path: "+path; g_idle_add(extract_done,job); return nullptr; }
        fs::path dest = fs::path(job->dest)/fs::path(path);
        // traversal check
        fs::path destCanon = fs::absolute(dest).lexically_normal();
        std::string outStr=canon.generic_string(), destStr=destCanon.generic_string();
        if(destStr.size()<outStr.size() || destStr.compare(0,outStr.size(),outStr)!=0 || (destStr.size()>outStr.size() && destStr[outStr.size()]!='/')){
            if(destStr!=outStr){ job->error="Path traversal: "+path; g_idle_add(extract_done,job); return nullptr; }
        }
        if(fs::exists(dest)){ job->error="Refusing to overwrite: "+dest.string(); g_idle_add(extract_done,job); return nullptr; }
        fs::create_directories(dest.parent_path());
        std::vector<uint8_t> dec(eh.originalSize);
        if(eh.originalSize==0){
            if(eh.compressedSize!=0){
                unsigned long long fcs=ZSTD_getFrameContentSize(comp.data(),comp.size());
                if(fcs!=0 && fcs!=ZSTD_CONTENTSIZE_UNKNOWN){ job->error="Frame mismatch for "+path; g_idle_add(extract_done,job); return nullptr; }
            }
        } else {
            size_t dSize=ZSTD_decompress(dec.data(), dec.size(), comp.data(), comp.size());
            if(ZSTD_isError(dSize)){ job->error=ZSTD_getErrorName(dSize); g_idle_add(extract_done,job); return nullptr; }
            if(dSize!=eh.originalSize){ job->error="Size mismatch "+path; g_idle_add(extract_done,job); return nullptr; }
            uint32_t crc=crc32(dec.data(), dec.size());
            if(crc!=eh.crc32){ job->error="CRC mismatch "+path; g_idle_add(extract_done,job); return nullptr; }
        }
        std::ofstream out(dest, std::ios::binary);
        if(!out){ job->error="Cannot write "+dest.string(); g_idle_add(extract_done,job); return nullptr; }
        if(!dec.empty()) out.write((char*)dec.data(), dec.size());
        count++;
        // progress update via idle? For now just final
    }
    job->extracted=count;
    job->success=true;
    g_idle_add(extract_done,job);
    return nullptr;
}

// ---------------------------------------------------------------------------
// Info/Test
// ---------------------------------------------------------------------------
static void append_info_text(AppState *st, const std::string &txt) {
    GtkTextIter end; gtk_text_buffer_get_end_iter(st->info_buffer, &end);
    gtk_text_buffer_insert(st->info_buffer, &end, txt.c_str(), -1);
    gtk_text_buffer_insert(st->info_buffer, &end, "\n", 1);
}

static gboolean info_done(gpointer data) {
    AppState *st=(AppState*)data;
    gtk_widget_set_sensitive(st->info_test_btn, TRUE);
    return G_SOURCE_REMOVE;
}

struct InfoJob { AppState *st; std::string archive; };

static gpointer info_test_thread(gpointer data){
    InfoJob *job=(InfoJob*)data;
    AppState *st=job->st;
    // Do test and info
    // Clear buffer in main thread? We'll append via idle
    g_idle_add([](gpointer d)->gboolean{ AppState *s=(AppState*)d; gtk_text_buffer_set_text(s->info_buffer,"",0); return G_SOURCE_REMOVE; }, st);
    std::ifstream in; uint64_t fsize=0;
    ArchiveHeader ah;
    try{
        fsize=fs::file_size(job->archive);
        in.open(job->archive, std::ios::binary);
        std::array<uint8_t,12> b; read_exact(in,b.data(),12);
        ah=ArchiveHeader::deserialize(b.data());
        std::string line="File: " + job->archive + "\nMagic: HAZF\nVersion: " + std::to_string(ah.version) + "\nEntries: " + std::to_string(ah.entryCount) + "\nArchive size: " + std::to_string(fsize) + " bytes\n";
        g_idle_add([](gpointer d)->gboolean{ auto *p=(std::pair<AppState*,std::string>*)d; append_info_text(p->first,p->second); delete p; return G_SOURCE_REMOVE; }, new std::pair<AppState*,std::string>(st,line));
    }catch(std::exception &e){
        g_idle_add([](gpointer d)->gboolean{ auto *p=(std::pair<AppState*,std::string>*)d; append_info_text(p->first, "Error: "+p->second); delete p; return G_SOURCE_REMOVE; }, new std::pair<AppState*,std::string>(st,e.what()));
        g_idle_add(info_done, st); delete job; return nullptr;
    }
    bool allOk=true;
    for(uint32_t i=0;i<ah.entryCount;++i){
        std::array<uint8_t,24> ehB; read_exact(in,ehB.data(),24);
        EntryHeader eh=EntryHeader::deserialize(ehB.data());
        std::string path; path.resize(eh.pathLen); if(eh.pathLen) read_exact(in,(uint8_t*)path.data(),eh.pathLen);
        std::vector<uint8_t> comp(eh.compressedSize); if(eh.compressedSize) read_exact(in,comp.data(),comp.size());
        std::vector<uint8_t> dec(eh.originalSize);
        std::string result;
        if(eh.originalSize==0){
            result="OK: " + path + " (empty, CRC 0)";
        } else {
            size_t dSize=ZSTD_decompress(dec.data(), dec.size(), comp.data(), comp.size());
            if(ZSTD_isError(dSize)){ result="FAIL: " + path + " decompress error: " + ZSTD_getErrorName(dSize); allOk=false; }
            else {
                uint32_t crc=crc32(dec.data(), dec.size());
                if(crc!=eh.crc32){ result="FAIL: " + path + " CRC mismatch"; allOk=false; }
                else { char buf[128]; snprintf(buf,sizeof(buf),"OK: %s (%llu bytes, CRC %08x)", path.c_str(), (unsigned long long)eh.originalSize, eh.crc32); result=buf; }
            }
        }
        g_idle_add([](gpointer d)->gboolean{ auto *p=(std::pair<AppState*,std::string>*)d; append_info_text(p->first,p->second); delete p; return G_SOURCE_REMOVE; }, new std::pair<AppState*,std::string>(st,result));
    }
    std::string finalLine = allOk? "\nAll entries verified successfully.\n" : "\nVerification FAILED.\n";
    g_idle_add([](gpointer d)->gboolean{ auto *p=(std::pair<AppState*,std::string>*)d; append_info_text(p->first,p->second); delete p; return G_SOURCE_REMOVE; }, new std::pair<AppState*,std::string>(st,finalLine));
    g_idle_add(info_done, st);
    delete job;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Page builders
// ---------------------------------------------------------------------------
static GtkWidget* create_card(const char* title, const char* subtitle, const char* icon_name) {
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(card, "card");
    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 28);
    gtk_widget_set_valign(icon, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(card), icon);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_hexpand(vbox, TRUE);
    GtkWidget *t = gtk_label_new(title);
    gtk_widget_add_css_class(t, "card-title");
    gtk_label_set_xalign(GTK_LABEL(t), 0);
    gtk_box_append(GTK_BOX(vbox), t);
    if(subtitle){
        GtkWidget *s = gtk_label_new(subtitle);
        gtk_widget_add_css_class(s, "card-subtitle");
        gtk_label_set_xalign(GTK_LABEL(s), 0);
        gtk_label_set_wrap(GTK_LABEL(s), TRUE);
        gtk_box_append(GTK_BOX(vbox), s);
    }
    gtk_box_append(GTK_BOX(card), vbox);
    return card;
}

static GtkWidget* build_create_page(AppState *st) {
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_margin_top(root, 16); gtk_widget_set_margin_bottom(root, 16);
    gtk_widget_set_margin_start(root, 16); gtk_widget_set_margin_end(root, 16);

    GtkWidget *card = create_card("Create Archive", "Drag & drop files and folders, then choose destination .hazf", "package-x-generic-symbolic");
    gtk_box_append(GTK_BOX(root), card);

    // Drop area
    st->create_drop_area = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(st->create_drop_area, "drop-area");
    gtk_widget_set_size_request(st->create_drop_area, -1, 140);
    GtkWidget *drop_icon = gtk_image_new_from_icon_name("folder-open-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(drop_icon), 40);
    gtk_widget_add_css_class(drop_icon, "drop-icon");
    gtk_widget_set_halign(drop_icon, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(drop_icon, 18);
    gtk_box_append(GTK_BOX(st->create_drop_area), drop_icon);
    GtkWidget *drop_title = gtk_label_new("Drop files or folders here");
    gtk_widget_add_css_class(drop_title, "drop-title");
    gtk_widget_set_halign(drop_title, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(st->create_drop_area), drop_title);
    GtkWidget *drop_sub = gtk_label_new("or click Add Files");
    gtk_widget_add_css_class(drop_sub, "drop-subtitle");
    gtk_widget_set_halign(drop_sub, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(st->create_drop_area), drop_sub);
    GtkWidget *add_btn = gtk_button_new_with_label("Add Files…");
    gtk_widget_add_css_class(add_btn, "pill");
    gtk_widget_set_halign(add_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_bottom(add_btn, 16);
    gtk_box_append(GTK_BOX(st->create_drop_area), add_btn);
    gtk_box_append(GTK_BOX(root), st->create_drop_area);

    // Drop target
    GtkDropTarget *drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect(drop, "drop", G_CALLBACK(+[](GtkDropTarget*, const GValue *val, double, double, gpointer data)->gboolean{
        AppState *s=(AppState*)data;
        if (G_VALUE_HOLDS(val, GDK_TYPE_FILE_LIST)) {
            GSList *files = (GSList*)g_value_get_boxed(val);
            for(GSList *l=files;l;l=l->next){
                GFile *f = (GFile*)l->data;
                char *path = g_file_get_path(f);
                if(path){ s->create_files.push_back(std::string(path)); g_free(path); }
            }
            refresh_create_list(s);
            gtk_widget_set_sensitive(s->create_create_btn, !s->create_files.empty() && !s->create_archive_path.empty());
        }
        return TRUE;
    }), st);
    g_signal_connect(drop, "enter", G_CALLBACK(+[](GtkDropTarget*, double, double, gpointer data)->GdkDragAction{
        AppState *s=(AppState*)data; gtk_widget_add_css_class(s->create_drop_area, "drag-over"); return GDK_ACTION_COPY;
    }), st);
    g_signal_connect(drop, "leave", G_CALLBACK(+[](GtkDropTarget*, gpointer data){ AppState *s=(AppState*)data; gtk_widget_remove_css_class(s->create_drop_area, "drag-over"); }), st);
    gtk_widget_add_controller(st->create_drop_area, GTK_EVENT_CONTROLLER(drop));
    g_signal_connect(add_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data){
        AppState *s=(AppState*)data;
        choose_files_open(s, s->window, [s](std::vector<std::string> files){
            for(auto &f: files) s->create_files.push_back(f);
            refresh_create_list(s);
            gtk_widget_set_sensitive(s->create_create_btn, !s->create_files.empty() && !s->create_archive_path.empty());
        });
    }), st);

    // File list
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 180);
    gtk_widget_add_css_class(scroll, "card");
    st->create_file_listbox = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(st->create_file_listbox), GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), st->create_file_listbox);
    gtk_box_append(GTK_BOX(root), scroll);

    st->create_count_label = gtk_label_new("No files selected — drag & drop or click Add");
    gtk_widget_add_css_class(st->create_count_label, "file-meta");
    gtk_label_set_xalign(GTK_LABEL(st->create_count_label), 0);
    gtk_box_append(GTK_BOX(root), st->create_count_label);

    // Archive path row
    GtkWidget *arc_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *arc_label = gtk_label_new("Archive:");
    gtk_widget_set_size_request(arc_label, 70, -1);
    gtk_label_set_xalign(GTK_LABEL(arc_label), 0);
    gtk_box_append(GTK_BOX(arc_box), arc_label);
    st->create_archive_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(st->create_archive_entry), "Choose destination…  (e.g., ~/Documents/archive.hazf)");
    gtk_widget_set_hexpand(st->create_archive_entry, TRUE);
    gtk_widget_add_css_class(st->create_archive_entry, "pill");
    gtk_box_append(GTK_BOX(arc_box), st->create_archive_entry);
    st->create_archive_choose_btn = gtk_button_new_from_icon_name("document-open-symbolic");
    gtk_widget_add_css_class(st->create_archive_choose_btn, "pill");
    gtk_box_append(GTK_BOX(arc_box), st->create_archive_choose_btn);
    gtk_box_append(GTK_BOX(root), arc_box);
    g_signal_connect(st->create_archive_choose_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data){
        AppState *s=(AppState*)data;
        choose_archive_save(s, s->window, [s](std::string p){
            if(!p.ends_with(".hazf")) p+=".hazf";
            s->create_archive_path=p;
            gtk_editable_set_text(GTK_EDITABLE(s->create_archive_entry), p.c_str());
            gtk_widget_set_sensitive(s->create_create_btn, !s->create_files.empty() && !p.empty());
        });
    }), st);
    g_signal_connect(st->create_archive_entry, "changed", G_CALLBACK(+[](GtkEditable*, gpointer data){
        AppState *s=(AppState*)data;
        const char *t = gtk_editable_get_text(GTK_EDITABLE(s->create_archive_entry));
        s->create_archive_path = t ? std::string(t) : "";
        gtk_widget_set_sensitive(s->create_create_btn, !s->create_files.empty() && !s->create_archive_path.empty());
    }), st);

    // Buttons
    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    st->create_clear_btn = gtk_button_new_with_label("Clear");
    gtk_widget_add_css_class(st->create_clear_btn, "pill");
    gtk_box_append(GTK_BOX(btn_box), st->create_clear_btn);
    st->create_create_btn = gtk_button_new_with_label("Create Archive");
    gtk_widget_add_css_class(st->create_create_btn, "pill");
    gtk_widget_add_css_class(st->create_create_btn, "suggested-action");
    GtkWidget *create_icon = gtk_image_new_from_icon_name("package-x-generic-symbolic");
    gtk_button_set_child(GTK_BUTTON(st->create_create_btn), create_icon);
    gtk_button_set_label(GTK_BUTTON(st->create_create_btn), "Create Archive");
    // Actually set both icon and label via box
    {
        GtkWidget *b = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *ic = gtk_image_new_from_icon_name("package-x-generic-symbolic");
        GtkWidget *lb = gtk_label_new("Create Archive");
        gtk_box_append(GTK_BOX(b), ic); gtk_box_append(GTK_BOX(b), lb);
        gtk_button_set_child(GTK_BUTTON(st->create_create_btn), b);
    }
    gtk_widget_set_sensitive(st->create_create_btn, FALSE);
    gtk_box_append(GTK_BOX(btn_box), st->create_create_btn);
    gtk_box_append(GTK_BOX(root), btn_box);

    g_signal_connect(st->create_clear_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data){
        AppState *s=(AppState*)data;
        s->create_files.clear();
        refresh_create_list(s);
        gtk_widget_set_sensitive(s->create_create_btn, FALSE);
    }), st);

    // Progress & status
    st->create_progress = gtk_progress_bar_new();
    gtk_widget_set_visible(st->create_progress, TRUE);
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(st->create_progress), FALSE);
    gtk_box_append(GTK_BOX(root), st->create_progress);
    st->create_status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(st->create_status), 0);
    gtk_label_set_wrap(GTK_LABEL(st->create_status), TRUE);
    gtk_widget_add_css_class(st->create_status, "file-meta");
    gtk_box_append(GTK_BOX(root), st->create_status);

    g_signal_connect(st->create_create_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data){
        AppState *s=(AppState*)data;
        if(s->create_files.empty() || s->create_archive_path.empty()){ set_status(s->create_status, "Add files and choose archive path", true); return; }
        gtk_widget_set_sensitive(s->create_create_btn, FALSE);
        gtk_widget_set_sensitive(s->create_clear_btn, FALSE);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(s->create_progress), 0.15);
        gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(s->create_progress), FALSE);
        set_status(s->create_status, "Creating archive...");
        CreateJob *job = new CreateJob{s, s->create_files, s->create_archive_path};
        gtk_progress_bar_pulse(GTK_PROGRESS_BAR(s->create_progress));
        g_thread_new("hazf-create", create_thread, job);
    }), st);

    return root;
}

static GtkWidget* build_browse_page(AppState *st) {
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(root,16); gtk_widget_set_margin_bottom(root,16);
    gtk_widget_set_margin_start(root,16); gtk_widget_set_margin_end(root,16);

    GtkWidget *card = create_card("Browse Archive", "Open a .hazf file to inspect its contents. Zstd + CRC32 verified.", "view-list-symbolic");
    gtk_box_append(GTK_BOX(root), card);

    GtkWidget *chooser = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    st->browse_archive_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(st->browse_archive_entry), "No archive selected — choose a .hazf file");
    gtk_widget_set_hexpand(st->browse_archive_entry, TRUE);
    gtk_widget_add_css_class(st->browse_archive_entry, "pill");
    gtk_box_append(GTK_BOX(chooser), st->browse_archive_entry);
    st->browse_choose_btn = gtk_button_new_with_label("Open…");
    {
        GtkWidget *b=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,6);
        gtk_box_append(GTK_BOX(b), gtk_image_new_from_icon_name("document-open-symbolic"));
        gtk_box_append(GTK_BOX(b), gtk_label_new("Open…"));
        gtk_button_set_child(GTK_BUTTON(st->browse_choose_btn), b);
    }
    gtk_widget_add_css_class(st->browse_choose_btn, "pill");
    gtk_box_append(GTK_BOX(chooser), st->browse_choose_btn);
    st->browse_open_btn = gtk_button_new_with_label("List");
    {
        GtkWidget *b=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,6);
        gtk_box_append(GTK_BOX(b), gtk_image_new_from_icon_name("view-list-symbolic"));
        gtk_box_append(GTK_BOX(b), gtk_label_new("List"));
        gtk_button_set_child(GTK_BUTTON(st->browse_open_btn), b);
    }
    gtk_widget_add_css_class(st->browse_open_btn, "suggested-action");
    gtk_widget_add_css_class(st->browse_open_btn, "pill");
    gtk_box_append(GTK_BOX(chooser), st->browse_open_btn);
    gtk_box_append(GTK_BOX(root), chooser);

    // Search
    st->browse_search_entry = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(st->browse_search_entry), "Filter files…");
    gtk_widget_add_css_class(st->browse_search_entry, "pill");
    gtk_box_append(GTK_BOX(root), st->browse_search_entry);
    g_signal_connect(st->browse_search_entry, "search-changed", G_CALLBACK(+[](GtkSearchEntry*, gpointer data){
        AppState *s=(AppState*)data; refresh_browse_list(s);
    }), st);

    // List
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_size_request(scroll, -1, 300);
    gtk_widget_add_css_class(scroll, "card");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    st->browse_listbox = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(st->browse_listbox), GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), st->browse_listbox);
    gtk_box_append(GTK_BOX(root), scroll);

    st->browse_status = gtk_label_new("");
    gtk_widget_add_css_class(st->browse_status, "file-meta");
    gtk_label_set_xalign(GTK_LABEL(st->browse_status),0);
    gtk_box_append(GTK_BOX(root), st->browse_status);

    g_signal_connect(st->browse_choose_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data){
        AppState *s=(AppState*)data;
        choose_archive_open(s, s->window, [s](std::string p){
            s->browse_archive_path=p;
            gtk_editable_set_text(GTK_EDITABLE(s->browse_archive_entry), p.c_str());
            do_browse_open(s);
        });
    }), st);
    g_signal_connect(st->browse_open_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data){
        AppState *s=(AppState*)data;
        const char *t=gtk_editable_get_text(GTK_EDITABLE(s->browse_archive_entry));
        if(t) s->browse_archive_path=t;
        do_browse_open(s);
    }), st);
    g_signal_connect(st->browse_archive_entry, "activate", G_CALLBACK(+[](GtkEntry*, gpointer data){
        AppState *s=(AppState*)data;
        const char *t=gtk_editable_get_text(GTK_EDITABLE(s->browse_archive_entry));
        if(t) s->browse_archive_path=t;
        do_browse_open(s);
    }), st);

    return root;
}

static GtkWidget* build_extract_page(AppState *st) {
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_margin_top(root,16); gtk_widget_set_margin_bottom(root,16);
    gtk_widget_set_margin_start(root,16); gtk_widget_set_margin_end(root,16);
    GtkWidget *card = create_card("Extract Archive", "Decompress with integrity check. Existing files are never overwritten.", "folder-symbolic");
    gtk_box_append(GTK_BOX(root), card);

    // Archive chooser
    GtkWidget *arc = gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);
    GtkWidget *al = gtk_label_new("Archive:");
    gtk_widget_set_size_request(al,70,-1); gtk_label_set_xalign(GTK_LABEL(al),0);
    gtk_box_append(GTK_BOX(arc), al);
    st->extract_archive_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(st->extract_archive_entry), "Choose .hazf archive");
    gtk_widget_set_hexpand(st->extract_archive_entry, TRUE);
    gtk_widget_add_css_class(st->extract_archive_entry, "pill");
    gtk_box_append(GTK_BOX(arc), st->extract_archive_entry);
    st->extract_archive_btn = gtk_button_new_from_icon_name("document-open-symbolic");
    gtk_widget_add_css_class(st->extract_archive_btn, "pill");
    gtk_box_append(GTK_BOX(arc), st->extract_archive_btn);
    gtk_box_append(GTK_BOX(root), arc);

    // Dest chooser
    GtkWidget *dest = gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);
    GtkWidget *dl = gtk_label_new("Destination:");
    gtk_widget_set_size_request(dl,70,-1); gtk_label_set_xalign(GTK_LABEL(dl),0);
    gtk_box_append(GTK_BOX(dest), dl);
    st->extract_dest_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(st->extract_dest_entry), "Choose output folder (default: current)");
    gtk_widget_set_hexpand(st->extract_dest_entry, TRUE);
    gtk_widget_add_css_class(st->extract_dest_entry, "pill");
    gtk_box_append(GTK_BOX(dest), st->extract_dest_entry);
    st->extract_dest_btn = gtk_button_new_from_icon_name("folder-open-symbolic");
    gtk_widget_add_css_class(st->extract_dest_btn, "pill");
    gtk_box_append(GTK_BOX(dest), st->extract_dest_btn);
    gtk_box_append(GTK_BOX(root), dest);

    // Buttons
    GtkWidget *btnbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);
    gtk_widget_set_halign(btnbox, GTK_ALIGN_END);
    st->extract_btn = gtk_button_new_with_label("Extract");
    {
        GtkWidget *b=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,6);
        gtk_box_append(GTK_BOX(b), gtk_image_new_from_icon_name("system-run-symbolic"));
        gtk_box_append(GTK_BOX(b), gtk_label_new("Extract"));
        gtk_button_set_child(GTK_BUTTON(st->extract_btn), b);
    }
    gtk_widget_add_css_class(st->extract_btn, "suggested-action");
    gtk_widget_add_css_class(st->extract_btn, "pill");
    gtk_box_append(GTK_BOX(btnbox), st->extract_btn);
    gtk_box_append(GTK_BOX(root), btnbox);

    st->extract_progress = gtk_progress_bar_new();
    gtk_box_append(GTK_BOX(root), st->extract_progress);
    st->extract_status = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(st->extract_status), TRUE);
    gtk_label_set_xalign(GTK_LABEL(st->extract_status),0);
    gtk_widget_add_css_class(st->extract_status,"file-meta");
    gtk_box_append(GTK_BOX(root), st->extract_status);

    g_signal_connect(st->extract_archive_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d){
        AppState *s=(AppState*)d;
        choose_archive_open(s, s->window, [s](std::string p){
            s->extract_archive_path=p;
            gtk_editable_set_text(GTK_EDITABLE(s->extract_archive_entry), p.c_str());
        });
    }), st);
    g_signal_connect(st->extract_archive_entry, "changed", G_CALLBACK(+[](GtkEditable*, gpointer d){
        AppState *s=(AppState*)d; const char *t=gtk_editable_get_text(GTK_EDITABLE(s->extract_archive_entry)); s->extract_archive_path=t?t:"";
    }), st);
    g_signal_connect(st->extract_dest_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d){
        AppState *s=(AppState*)d;
        choose_folder(s, s->window, [s](std::string p){
            s->extract_dest_path=p;
            gtk_editable_set_text(GTK_EDITABLE(s->extract_dest_entry), p.c_str());
        });
    }), st);
    g_signal_connect(st->extract_dest_entry, "changed", G_CALLBACK(+[](GtkEditable*, gpointer d){
        AppState *s=(AppState*)d; const char *t=gtk_editable_get_text(GTK_EDITABLE(s->extract_dest_entry)); s->extract_dest_path=t?t:"";
    }), st);
    g_signal_connect(st->extract_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d){
        AppState *s=(AppState*)d;
        if(s->extract_archive_path.empty()){ set_status(s->extract_status,"Choose an archive",true); return; }
        std::string dest = s->extract_dest_path.empty()? std::string(".") : s->extract_dest_path;
        gtk_widget_set_sensitive(s->extract_btn, FALSE);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(s->extract_progress), 0.1);
        set_status(s->extract_status, "Extracting...");
        ExtractJob *job=new ExtractJob{s, s->extract_archive_path, dest};
        g_thread_new("hazf-extract", extract_thread, job);
    }), st);

    return root;
}

static GtkWidget* build_info_page(AppState *st) {
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(root,16); gtk_widget_set_margin_bottom(root,16);
    gtk_widget_set_margin_start(root,16); gtk_widget_set_margin_end(root,16);
    GtkWidget *card = create_card("Info & Verify", "Show archive details and verify CRC32 integrity in memory.", "emblem-ok-symbolic");
    gtk_box_append(GTK_BOX(root), card);

    GtkWidget *chooser = gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);
    st->info_archive_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(st->info_archive_entry), "Choose .hazf archive to inspect");
    gtk_widget_set_hexpand(st->info_archive_entry, TRUE);
    gtk_widget_add_css_class(st->info_archive_entry, "pill");
    gtk_box_append(GTK_BOX(chooser), st->info_archive_entry);
    st->info_choose_btn = gtk_button_new_from_icon_name("document-open-symbolic");
    gtk_widget_add_css_class(st->info_choose_btn, "pill");
    gtk_box_append(GTK_BOX(chooser), st->info_choose_btn);
    st->info_test_btn = gtk_button_new_with_label("Verify");
    {
        GtkWidget *b=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,6);
        gtk_box_append(GTK_BOX(b), gtk_image_new_from_icon_name("emblem-ok-symbolic"));
        gtk_box_append(GTK_BOX(b), gtk_label_new("Verify"));
        gtk_button_set_child(GTK_BUTTON(st->info_test_btn), b);
    }
    gtk_widget_add_css_class(st->info_test_btn, "suggested-action");
    gtk_widget_add_css_class(st->info_test_btn, "pill");
    gtk_box_append(GTK_BOX(chooser), st->info_test_btn);
    gtk_box_append(GTK_BOX(root), chooser);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_add_css_class(scroll, "card");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    st->info_textview = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(st->info_textview), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(st->info_textview), GTK_WRAP_WORD);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(st->info_textview), TRUE);
    st->info_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(st->info_textview));
    gtk_text_buffer_set_text(st->info_buffer, "Open an archive and click Verify to check integrity.\nHAZF uses Zstandard compression and CRC32 (IEEE) for each file.", -1);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), st->info_textview);
    gtk_widget_set_size_request(scroll, -1, 260);
    gtk_box_append(GTK_BOX(root), scroll);

    st->info_status = gtk_label_new("");
    gtk_widget_add_css_class(st->info_status,"file-meta");
    gtk_label_set_xalign(GTK_LABEL(st->info_status),0);
    gtk_box_append(GTK_BOX(root), st->info_status);

    g_signal_connect(st->info_choose_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d){
        AppState *s=(AppState*)d;
        choose_archive_open(s, s->window, [s](std::string p){
            s->info_archive_path=p;
            gtk_editable_set_text(GTK_EDITABLE(s->info_archive_entry), p.c_str());
            // auto verify?
        });
    }), st);
    g_signal_connect(st->info_archive_entry, "changed", G_CALLBACK(+[](GtkEditable*, gpointer d){
        AppState *s=(AppState*)d; const char *t=gtk_editable_get_text(GTK_EDITABLE(s->info_archive_entry)); s->info_archive_path=t?t:"";
    }), st);
    g_signal_connect(st->info_test_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d){
        AppState *s=(AppState*)d;
        if(s->info_archive_path.empty()){ const char *t=gtk_editable_get_text(GTK_EDITABLE(s->info_archive_entry)); if(t) s->info_archive_path=t; }
        if(s->info_archive_path.empty()){ set_status(s->info_status,"Choose an archive first",true); return; }
        gtk_widget_set_sensitive(s->info_test_btn, FALSE);
        set_status(s->info_status, "Verifying...");
        gtk_text_buffer_set_text(s->info_buffer, "Verifying...\n", -1);
        InfoJob *job=new InfoJob{s, s->info_archive_path};
        g_thread_new("hazf-info", info_test_thread, job);
    }), st);

    return root;
}

// ---------------------------------------------------------------------------
// Activate
// ---------------------------------------------------------------------------
static void on_activate(GtkApplication *app, gpointer user_data) {
    AppState *st = (AppState*)user_data;
    st->app = app;

    GtkWidget *win = gtk_application_window_new(app);
    st->window = GTK_WINDOW(win);
    gtk_window_set_title(st->window, "HAZF Archive Manager");
    gtk_window_set_default_size(st->window, 900, 640);
    gtk_window_set_icon_name(st->window, "com.hyggshi.hazf");

    // HeaderBar — polished with centered StackSwitcher
    GtkWidget *header = gtk_header_bar_new();
    gtk_window_set_titlebar(st->window, header);
    // Left: app brand
    GtkWidget *brand_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *icon = gtk_image_new_from_icon_name("com.hyggshi.hazf");
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 22);
    gtk_box_append(GTK_BOX(brand_box), icon);
    GtkWidget *title_label = gtk_label_new("HAZF");
    gtk_widget_add_css_class(title_label, "title");
    gtk_box_append(GTK_BOX(brand_box), title_label);
    GtkWidget *ver = gtk_label_new("v1.0");
    gtk_widget_add_css_class(ver, "file-meta");
    gtk_widget_add_css_class(ver, "archive-badge");
    gtk_box_append(GTK_BOX(brand_box), ver);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), brand_box);

    GtkWidget *about_btn = gtk_button_new_from_icon_name("help-about-symbolic");
    gtk_widget_add_css_class(about_btn, "icon-button");
    gtk_widget_set_tooltip_text(about_btn, "About HAZF");
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), about_btn);
    g_signal_connect(about_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data){
        AppState *s=(AppState*)data;
        GtkWidget *dlg = gtk_about_dialog_new();
        gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(dlg), "HAZF Archive Manager");
        gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dlg), "1.0.0");
        gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dlg), "Hyggshi Archive Zip Format — Zstandard + CRC32, little-endian.\nCreate, browse, extract and verify .hazf archives.");
        gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(dlg), "https://github.com/Hyggshi-OS-Research-Technology/HAZF");
        gtk_about_dialog_set_logo_icon_name(GTK_ABOUT_DIALOG(dlg), "com.hyggshi.hazf");
        gtk_about_dialog_set_license_type(GTK_ABOUT_DIALOG(dlg), GTK_LICENSE_MIT_X11);
        const char *authors[] = {"Hyggshi OS Research Technology", nullptr};
        gtk_about_dialog_set_authors(GTK_ABOUT_DIALOG(dlg), authors);
        gtk_window_set_transient_for(GTK_WINDOW(dlg), s->window);
        gtk_window_present(GTK_WINDOW(dlg));
    }), st);

    // Stack + Switcher — switcher centered as header title
    st->stack = GTK_STACK(gtk_stack_new());
    gtk_stack_set_transition_type(st->stack, GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
    gtk_stack_set_transition_duration(st->stack, 220);
    gtk_widget_set_vexpand(GTK_WIDGET(st->stack), TRUE);

    st->switcher = GTK_STACK_SWITCHER(gtk_stack_switcher_new());
    gtk_stack_switcher_set_stack(st->switcher, st->stack);
    gtk_widget_add_css_class(GTK_WIDGET(st->switcher), "stack-switcher");
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), GTK_WIDGET(st->switcher));

    // Pages
    GtkWidget *create_page = build_create_page(st);
    gtk_stack_add_titled(st->stack, create_page, "create", "Create");
    GtkWidget *browse_page = build_browse_page(st);
    gtk_stack_add_titled(st->stack, browse_page, "browse", "Browse");
    GtkWidget *extract_page = build_extract_page(st);
    gtk_stack_add_titled(st->stack, extract_page, "extract", "Extract");
    GtkWidget *info_page = build_info_page(st);
    gtk_stack_add_titled(st->stack, info_page, "info", "Verify");

    // Apply CSS
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, kCss);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    // Main container
    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(main_box), GTK_WIDGET(st->stack));
    gtk_window_set_child(st->window, main_box);

    gtk_window_present(st->window);
}

int main(int argc, char *argv[]) {
    g_state = new AppState();
    GtkApplication *app = gtk_application_new("com.hyggshi.hazf", G_APPLICATION_HANDLES_OPEN);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), g_state);

    // if launched with file, open browse
    g_signal_connect(app, "open", G_CALLBACK(+[](GtkApplication *a, GFile **files, gint n, gchar*, gpointer data){
        AppState *s=(AppState*)data;
        g_signal_emit_by_name(a, "activate");
        // defer until window exists
        g_idle_add([](gpointer d)->gboolean{
            AppState *s=(AppState*)d;
            if(s->window && n>0){
                // not fully handled here
            }
            return G_SOURCE_REMOVE;
        }, s);
    }), g_state);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    delete g_state;
    return status;
}
