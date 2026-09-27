// Library view (extension-4 §9.2): category tree, live search, annotated table, the
// inspector's annotation editor, the background annotate job and the annotator probe.

#include "gui/app_state.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include "soundpalette/ucs.h"

namespace spapp {

namespace {

void copy_to(char *dst, std::size_t n, const std::string &s) {
    std::snprintf(dst, n, "%s", s.c_str());
}

std::vector<std::string> split_keywords(const char *text) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        while (!cur.empty() && cur.front() == ' ') {
            cur.erase(cur.begin());
        }
        while (!cur.empty() && cur.back() == ' ') {
            cur.pop_back();
        }
        if (!cur.empty()) {
            out.push_back(cur);
        }
        cur.clear();
    };
    for (const char *p = text; *p; ++p) {
        if (*p == ',' || *p == ';') {
            flush();
        } else {
            cur.push_back(*p);
        }
    }
    flush();
    return out;
}

std::string join_keywords(const std::vector<std::string> &kw) {
    std::string out;
    for (const std::string &k : kw) {
        out += (out.empty() ? "" : ", ") + k;
    }
    return out;
}

void load_edit_buffers(AppState &state, const std::string &path) {
    state.lib_edit_path = path;
    state.lib_edit_row = state.library ? state.library->get(path) : std::nullopt;
    const sp::Annotation empty;
    const sp::Annotation &a = state.lib_edit_row && state.lib_edit_row->annotation
                                  ? *state.lib_edit_row->annotation
                                  : empty;
    copy_to(state.lib_edit_catid, sizeof state.lib_edit_catid, a.cat_id);
    copy_to(state.lib_edit_fx, sizeof state.lib_edit_fx, a.fx_name);
    copy_to(state.lib_edit_desc, sizeof state.lib_edit_desc, a.description);
    copy_to(state.lib_edit_kw, sizeof state.lib_edit_kw, join_keywords(a.keywords));
}

const ImVec4 kGreen(0.58f, 0.85f, 0.35f, 1.0f);
const ImVec4 kRed(1.0f, 0.45f, 0.45f, 1.0f);
const ImVec4 kAmber(0.9f, 0.75f, 0.4f, 1.0f);
const ImVec4 kDim(0.6f, 0.6f, 0.65f, 1.0f);

} // namespace

// ---------------------------------------------------------------------------------------------
// Index lifecycle

void library_open_for_root(AppState &state) {
    state.library.reset();
    state.library_error.clear();
    state.lib_rows.clear();
    state.lib_row_file.clear();
    state.lib_edit_path.clear();
    state.lib_edit_row.reset();
    state.lib_dirty = true;
    if (state.root_dir.empty()) {
        return;
    }
    std::error_code ec;
    if (!std::filesystem::exists(sp::Library::index_path(state.root_dir), ec)) {
        return; // no index yet: the view offers to create one
    }
    std::string err;
    state.library = sp::Library::open(state.root_dir, err);
    if (!state.library) {
        state.library_error = err;
    }
}

bool library_create(AppState &state) {
    if (state.root_dir.empty()) {
        return false;
    }
    std::string err;
    state.library = sp::Library::create(state.root_dir, err);
    if (!state.library) {
        state.library_error = err;
        state.status_message = err;
        return false;
    }
    sp::UpdateReport rep = state.library->update(sp::UpdateOptions{}, err);
    sp::ClassifyReport crep = state.library->classify_offline(false);
    state.status_message = "library created: " + std::to_string(rep.analyzed) + " analyzed, " +
                           std::to_string(crep.written) + " classified from names";
    state.lib_dirty = true;
    return true;
}

void library_update(AppState &state) {
    if (!state.library) {
        return;
    }
    std::string err;
    sp::UpdateReport rep = state.library->update(sp::UpdateOptions{}, err);
    if (!err.empty()) {
        state.status_message = err;
        return;
    }
    state.library->classify_offline(false);
    state.status_message = "library updated: " + std::to_string(rep.added) + " added, " +
                           std::to_string(rep.changed) + " changed, " + std::to_string(rep.moved) +
                           " moved, " + std::to_string(rep.missing) + " missing";
    state.lib_dirty = true;
}

void library_requery(AppState &state) {
    state.lib_dirty = false;
    state.lib_rows.clear();
    state.lib_row_file.clear();
    state.lib_category_counts.clear();
    state.lib_catid_counts.clear();
    if (!state.library) {
        return;
    }
    sp::SearchQuery q;
    q.text = state.lib_search;
    q.category = state.lib_category;
    q.cat_id_glob = state.lib_catid;
    q.unannotated = state.lib_only_unannotated;
    q.limit = 5000;
    std::string err;
    std::vector<sp::LibraryRow> rows = state.library->search(q, err);
    if (!err.empty()) {
        state.status_message = err;
    }
    std::unordered_map<std::string, int> index;
    for (std::size_t i = 0; i < state.manifest.files.size(); ++i) {
        index.emplace(state.manifest.files[i].path, static_cast<int>(i));
    }
    for (sp::LibraryRow &r : rows) {
        if (state.lib_only_low_conf && r.annotation && r.annotation->confidence >= 0.5) {
            continue;
        }
        if (state.lib_only_low_conf && !r.annotation) {
            continue; // "low confidence" means annotated but unsure; unannotated has its own chip
        }
        if (state.lib_only_locked && !(r.annotation && r.annotation->locked)) {
            continue;
        }
        auto it = index.find(r.path);
        state.lib_row_file.push_back(it == index.end() ? -1 : it->second);
        state.lib_rows.push_back(std::move(r));
    }
    state.lib_category_counts = state.library->category_counts();
    state.lib_file_count = state.library->count_files(true);
    state.lib_annotated_count = state.library->count_annotated();
    if (!state.lib_edit_path.empty()) {
        load_edit_buffers(state, state.lib_edit_path);
    }
}

void library_select_row(AppState &state, int row) {
    if (row < 0 || row >= static_cast<int>(state.lib_rows.size())) {
        return;
    }
    const std::string &path = state.lib_rows[static_cast<std::size_t>(row)].path;
    state.selected = state.lib_row_file[static_cast<std::size_t>(row)];
    load_edit_buffers(state, path);
}

// ---------------------------------------------------------------------------------------------
// Annotate job + probe

void start_annotate(AppState &state, std::vector<std::string> rel_paths) {
    if (!state.library || state.annotating.load() || rel_paths.empty()) {
        return;
    }
    shutdown_annotate_thread(state);
    state.annotate_cancel = false;
    state.annotate_done = 0;
    state.annotate_total = rel_paths.size();
    {
        std::lock_guard<std::mutex> lock(state.annotate_mutex);
        state.annotate_result.reset();
        state.annotate_last_path.clear();
    }
    state.annotating = true;
    sp::Library *lib = state.library.get();
    AppState *st = &state;
    std::string cmd = state.annotator_command;
    state.annotate_thread = std::thread([lib, st, cmd, paths = std::move(rel_paths)] {
        sp::AnnotatorOptions opt;
        opt.command = cmd;
        opt.should_cancel = [st] { return st->annotate_cancel.load(); };
        opt.on_progress = [st](std::size_t done, std::size_t, const std::string &path,
                               const std::string &) {
            st->annotate_done = done;
            std::lock_guard<std::mutex> lock(st->annotate_mutex);
            st->annotate_last_path = path;
        };
        sp::AnnotateReport rep = sp::annotate_paths(*lib, paths, opt);
        {
            std::lock_guard<std::mutex> lock(st->annotate_mutex);
            st->annotate_result = std::move(rep);
        }
        st->annotating = false;
    });
}

void poll_annotate(AppState &state) {
    if (state.annotating.load()) {
        return;
    }
    std::optional<sp::AnnotateReport> rep;
    {
        std::lock_guard<std::mutex> lock(state.annotate_mutex);
        if (state.annotate_result.has_value()) {
            rep = std::move(state.annotate_result);
            state.annotate_result.reset();
        }
    }
    if (!rep) {
        return;
    }
    if (state.annotate_thread.joinable()) {
        state.annotate_thread.join();
    }
    if (!rep->fatal.empty()) {
        state.status_message = "annotate failed: " + rep->fatal;
    } else {
        state.status_message = "annotated " + std::to_string(rep->annotated) + " of " +
                               std::to_string(rep->requested) + " (" + std::to_string(rep->errors) +
                               " errors, " + std::to_string(rep->skipped_locked) + " locked)";
    }
    state.lib_dirty = true;
}

void shutdown_annotate_thread(AppState &state) {
    if (state.annotate_thread.joinable()) {
        state.annotate_cancel = true;
        state.annotate_thread.join();
    }
    if (state.probe_thread.joinable()) {
        state.probe_thread.join();
    }
}

void start_annotator_probe(AppState &state) {
    if (state.probe_state.load() == 1) {
        return;
    }
    if (state.probe_thread.joinable()) {
        state.probe_thread.join();
    }
    state.probe_state = 1;
    state.probed_command = state.annotator_command;
    AppState *st = &state;
    std::string cmd = state.annotator_command;
    state.probe_thread = std::thread([st, cmd] {
        sp::AnnotatorOptions opt;
        opt.command = cmd;
        opt.timeout_s = 30.0;
        std::string err;
        std::optional<sp::AnnotatorInfo> info = sp::probe_annotator(opt, err);
        std::lock_guard<std::mutex> lock(st->probe_mutex);
        if (info) {
            st->probe_info = *info;
            st->probe_error.clear();
            st->probe_state = 2;
        } else {
            st->probe_error = err;
            st->probe_state = 3;
        }
    });
}

// ---------------------------------------------------------------------------------------------
// Menu + footer

void draw_library_menu(AppState &state) {
    if (!ImGui::BeginMenu("Library")) {
        return;
    }
    const bool have_root = !state.root_dir.empty();
    const bool have_lib = state.library != nullptr;
    const bool busy = state.annotating.load();
    if (ImGui::MenuItem("Create index for this folder", nullptr, false, have_root && !have_lib)) {
        library_create(state);
    }
    if (ImGui::MenuItem("Update index", nullptr, false, have_lib && !busy)) {
        library_update(state);
    }
    if (ImGui::MenuItem("Classify from names (offline)", nullptr, false, have_lib && !busy)) {
        sp::ClassifyReport c = state.library->classify_offline(false);
        state.status_message = "offline classify: " + std::to_string(c.written) + " written, " +
                               std::to_string(c.unmatched) + " unmatched";
        state.lib_dirty = true;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Annotate unannotated", nullptr, false, have_lib && !busy)) {
        sp::SearchQuery q;
        q.unannotated = true;
        q.limit = 0;
        std::string err;
        std::vector<std::string> paths;
        for (const sp::LibraryRow &r : state.library->search(q, err)) {
            paths.push_back(r.path);
        }
        start_annotate(state, std::move(paths));
    }
    const bool have_sel = state.selected >= 0 || !state.multi_selected.empty();
    if (ImGui::MenuItem("Annotate selection", nullptr, false, have_lib && !busy && have_sel)) {
        std::vector<std::string> paths;
        std::set<int> idx = state.multi_selected;
        if (state.selected >= 0) {
            idx.insert(state.selected);
        }
        for (int i : idx) {
            if (i >= 0 && i < static_cast<int>(state.manifest.files.size())) {
                paths.push_back(state.manifest.files[static_cast<std::size_t>(i)].path);
            }
        }
        start_annotate(state, std::move(paths));
    }
    if (ImGui::MenuItem("Annotate current search results", nullptr, false,
                        have_lib && !busy && !state.lib_rows.empty())) {
        std::vector<std::string> paths;
        for (const sp::LibraryRow &r : state.lib_rows) {
            paths.push_back(r.path);
        }
        start_annotate(state, std::move(paths));
    }
    if (ImGui::MenuItem("Cancel annotation", nullptr, false, busy)) {
        state.annotate_cancel = true;
    }
    ImGui::Separator();
    if (ImGui::BeginMenu("Annotator command")) {
        static char buf[512] = {0};
        static bool loaded = false;
        if (!loaded) {
            copy_to(buf, sizeof buf, state.annotator_command);
            loaded = true;
        }
        ImGui::TextDisabled("empty = $SP_ANNOTATOR or '%s' on PATH", sp::kDefaultAnnotatorCommand);
        ImGui::SetNextItemWidth(28.0f * ImGui::GetFontSize());
        if (ImGui::InputText("##annotator_cmd", buf, sizeof buf)) {
            state.annotator_command = buf;
            state.probe_state = 0;
        }
        if (ImGui::MenuItem("Probe now", nullptr, false, !busy)) {
            start_annotator_probe(state);
        }
        ImGui::EndMenu();
    }
    ImGui::EndMenu();
}

void draw_library_status(AppState &state) {
    if (!state.library) {
        return;
    }
    ImGui::SameLine();
    ImGui::TextColored(kGreen, "| library: %zu files, %zu tagged", state.lib_file_count,
                       state.lib_annotated_count);
    if (state.annotating.load()) {
        std::string last;
        {
            std::lock_guard<std::mutex> lock(state.annotate_mutex);
            last = state.annotate_last_path;
        }
        ImGui::SameLine();
        ImGui::TextColored(kAmber, "| annotating %zu/%zu %s", state.annotate_done.load(),
                           state.annotate_total.load(), last.c_str());
        return;
    }
    const int ps = state.probe_state.load();
    if (ps == 1) {
        ImGui::SameLine();
        ImGui::TextColored(kDim, "| annotator: probing...");
    } else if (ps == 2) {
        std::lock_guard<std::mutex> lock(state.probe_mutex);
        ImGui::SameLine();
        ImGui::TextColored(kGreen, "| annotator: %s (%s)", state.probe_info.name.c_str(),
                           state.probe_info.model.empty() ? "no model id"
                                                          : state.probe_info.model.c_str());
    } else if (ps == 3) {
        std::lock_guard<std::mutex> lock(state.probe_mutex);
        ImGui::SameLine();
        ImGui::TextColored(kRed, "| annotator unreachable: %s", state.probe_error.c_str());
    }
}

// ---------------------------------------------------------------------------------------------
// The view

namespace {

void draw_category_tree(AppState &state) {
    const bool all = state.lib_category.empty() && !state.lib_only_unannotated;
    if (ImGui::Selectable("All", all)) {
        state.lib_category.clear();
        state.lib_catid.clear();
        state.lib_only_unannotated = false;
        state.lib_dirty = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu", state.lib_file_count);
    const std::size_t untagged = state.lib_file_count - state.lib_annotated_count;
    if (ImGui::Selectable("Unannotated", state.lib_only_unannotated)) {
        state.lib_category.clear();
        state.lib_catid.clear();
        state.lib_only_unannotated = !state.lib_only_unannotated;
        state.lib_dirty = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu", untagged);
    ImGui::Separator();
    for (const auto &[cat, count] : state.lib_category_counts) {
        ImGui::PushID(cat.c_str());
        const bool selected_cat = state.lib_category == cat;
        ImGuiTreeNodeFlags flags =
            ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (selected_cat && state.lib_catid.empty()) {
            flags |= ImGuiTreeNodeFlags_Selected;
        }
        const bool open = ImGui::TreeNodeEx("##cat", flags, "%s", cat.c_str());
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            state.lib_category = selected_cat && state.lib_catid.empty() ? "" : cat;
            state.lib_catid.clear();
            state.lib_only_unannotated = false;
            state.lib_dirty = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%zu", count);
        if (open) {
            auto it = state.lib_catid_counts.find(cat);
            if (it == state.lib_catid_counts.end() && state.library) {
                it = state.lib_catid_counts.emplace(cat, state.library->cat_id_counts(cat)).first;
            }
            if (it != state.lib_catid_counts.end()) {
                for (const auto &[cid, n] : it->second) {
                    const sp::UcsEntry *e = sp::ucs_find(cid);
                    std::string label = cid;
                    if (e) {
                        label += "  " + std::string(e->sub_category);
                    }
                    const bool sel = state.lib_catid == cid;
                    if (ImGui::Selectable(label.c_str(), sel)) {
                        state.lib_category = cat;
                        state.lib_catid = sel ? "" : cid;
                        state.lib_only_unannotated = false;
                        state.lib_dirty = true;
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%zu", n);
                }
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

void draw_transport(AppState &state, int file_index) {
    if (file_index < 0) {
        return;
    }
    const bool is_playing = state.playing_index == file_index;
    if (is_playing) {
        if (ImGui::SmallButton(state.paused ? ">" : "||")) {
            playback_toggle_pause(state);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("stop")) {
            playback_stop(state);
        }
        ImGui::SameLine();
        ImGui::Text("%.1fs", playback_remaining_s(state));
    } else if (ImGui::SmallButton(">")) {
        playback_start(state, file_index);
    }
}

} // namespace

void draw_library(AppState &state) {
    poll_annotate(state);
    if (state.root_dir.empty()) {
        ImGui::TextDisabled("No folder open (File > Open folder...)");
        return;
    }
    if (!state.library) {
        if (!state.library_error.empty()) {
            ImGui::TextColored(kRed, "%s", state.library_error.c_str());
        }
        ImGui::TextWrapped("This folder has no library index yet. The index stores every "
                           "file's analysis and its UCS annotation in %s/%s and makes the "
                           "folder searchable.",
                           sp::Library::kDirName, sp::Library::kIndexName);
        if (ImGui::Button("Create index")) {
            library_create(state);
        }
        return;
    }
    if (state.probe_state.load() == 0 && !state.smoke_mode) {
        start_annotator_probe(state);
    }
    if (state.lib_dirty) {
        library_requery(state);
    }

    const float s = state.ui_scale();
    const float tree_w = 190.0f * s;
    ImGui::BeginChild("lib_tree", ImVec2(tree_w, 0.0f), ImGuiChildFlags_Borders);
    draw_category_tree(state);
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("lib_main", ImVec2(0.0f, 0.0f));
    ImGui::SetNextItemWidth(16.0f * ImGui::GetFontSize());
    if (ImGui::InputTextWithHint("##lib_search", "search names, descriptions, keywords",
                                 state.lib_search, sizeof state.lib_search)) {
        state.lib_dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("untagged", &state.lib_only_unannotated)) {
        state.lib_dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("low conf", &state.lib_only_low_conf)) {
        state.lib_dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("locked", &state.lib_only_locked)) {
        state.lib_dirty = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu rows", state.lib_rows.size());
    if (state.annotating.load()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("cancel annotation")) {
            state.annotate_cancel = true;
        }
    }

    const float cell = 36.0f * s;
    ImGuiTableFlags tflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                             ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                             ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("lib_table", 8, tflags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, cell);
        ImGui::TableSetupColumn("path", ImGuiTableColumnFlags_WidthStretch, 4.0f);
        ImGui::TableSetupColumn("CatID", ImGuiTableColumnFlags_WidthFixed,
                                5.5f * ImGui::GetFontSize());
        ImGui::TableSetupColumn("FX name", ImGuiTableColumnFlags_WidthStretch, 2.5f);
        ImGui::TableSetupColumn("description", ImGuiTableColumnFlags_WidthStretch, 5.0f);
        ImGui::TableSetupColumn("conf", ImGuiTableColumnFlags_WidthFixed,
                                2.6f * ImGui::GetFontSize());
        ImGui::TableSetupColumn("src", ImGuiTableColumnFlags_WidthFixed,
                                4.2f * ImGui::GetFontSize());
        ImGui::TableSetupColumn("play", ImGuiTableColumnFlags_WidthFixed,
                                5.5f * ImGui::GetFontSize());
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(state.lib_rows.size()), cell + 4.0f);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const sp::LibraryRow &r = state.lib_rows[static_cast<std::size_t>(row)];
                const int fi = state.lib_row_file[static_cast<std::size_t>(row)];
                ImGui::PushID(row);
                ImGui::TableNextRow(0, cell + 4.0f);
                ImGui::TableSetColumnIndex(0);
                {
                    ImVec2 pos = ImGui::GetCursorScreenPos();
                    ImGui::Dummy(ImVec2(cell, cell));
                    if (fi >= 0) {
                        const sp::FileEntry &e = state.manifest.files[static_cast<std::size_t>(fi)];
                        if (e.error.empty()) {
                            draw_glyph(ImGui::GetWindowDrawList(), e.visual,
                                       ImVec2(pos.x + cell * 0.5f, pos.y + cell * 0.5f), cell);
                        }
                    }
                }
                ImGui::TableSetColumnIndex(1);
                const bool selected = state.lib_edit_path == r.path;
                if (ImGui::Selectable(r.path.c_str(), selected,
                                      ImGuiSelectableFlags_SpanAllColumns |
                                          ImGuiSelectableFlags_AllowOverlap)) {
                    library_select_row(state, row);
                }
                if (ImGui::IsItemHovered() && r.annotation && !r.annotation->description.empty()) {
                    ImGui::SetTooltip("%s", r.annotation->description.c_str());
                }
                ImGui::TableSetColumnIndex(2);
                if (r.annotation && !r.annotation->cat_id.empty()) {
                    ImGui::TextUnformatted(r.annotation->cat_id.c_str());
                } else {
                    ImGui::TextDisabled("-");
                }
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(r.annotation ? r.annotation->fx_name.c_str() : "");
                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(r.annotation ? r.annotation->description.c_str() : "");
                ImGui::TableSetColumnIndex(5);
                if (r.annotation) {
                    const double c = r.annotation->confidence;
                    ImGui::TextColored(c >= 0.7 ? kGreen : (c >= 0.4 ? kAmber : kRed), "%.2f", c);
                }
                ImGui::TableSetColumnIndex(6);
                if (r.annotation) {
                    ImGui::TextUnformatted(sp::annotation_source_name(r.annotation->source));
                    if (r.annotation->locked) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("[L]");
                    }
                }
                ImGui::TableSetColumnIndex(7);
                draw_transport(state, fi);
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------------------------
// Inspector: annotation editor

void draw_annotation_section(AppState &state, int file_index) {
    if (!state.library || file_index < 0 ||
        file_index >= static_cast<int>(state.manifest.files.size())) {
        return;
    }
    if (!ImGui::CollapsingHeader("Annotation (UCS)", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    const std::string &path = state.manifest.files[static_cast<std::size_t>(file_index)].path;
    if (state.lib_edit_path != path) {
        load_edit_buffers(state, path);
    }
    if (!state.lib_edit_row) {
        ImGui::TextDisabled("not in the index yet (Library > Update index)");
        return;
    }
    const sp::Annotation *current =
        state.lib_edit_row->annotation ? &*state.lib_edit_row->annotation : nullptr;
    const float fs = ImGui::GetFontSize();

    // CatID with type-ahead suggestions.
    ImGui::SetNextItemWidth(8.0f * fs);
    ImGui::InputText("CatID", state.lib_edit_catid, sizeof state.lib_edit_catid);
    const sp::UcsEntry *exact = sp::ucs_find(state.lib_edit_catid);
    if (exact) {
        ImGui::TextColored(kGreen, "%s / %s", std::string(exact->category).c_str(),
                           std::string(exact->sub_category).c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", std::string(exact->explanation).c_str());
        }
    } else if (state.lib_edit_catid[0] != '\0') {
        // prefix matches on the CatID first, then keyword ranking over the typed words
        std::string typed = state.lib_edit_catid;
        std::string lower = typed;
        for (char &c : lower) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        std::vector<const sp::UcsEntry *> hits;
        for (const sp::UcsEntry &e : sp::ucs_entries()) {
            std::string id(e.cat_id);
            for (char &c : id) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (id.rfind(lower, 0) == 0) {
                hits.push_back(&e);
            }
            if (hits.size() >= 8) {
                break;
            }
        }
        if (hits.empty()) {
            for (const sp::UcsMatch &m : sp::ucs_rank(sp::ucs_tokenize(typed))) {
                hits.push_back(m.entry);
                if (hits.size() >= 8) {
                    break;
                }
            }
        }
        for (const sp::UcsEntry *e : hits) {
            std::string label = std::string(e->cat_id) + "  " + std::string(e->category) + " / " +
                                std::string(e->sub_category);
            if (ImGui::Selectable(label.c_str())) {
                copy_to(state.lib_edit_catid, sizeof state.lib_edit_catid, std::string(e->cat_id));
            }
        }
        if (hits.empty()) {
            ImGui::TextColored(kAmber, "no matching CatID");
        }
    }

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##fx", "FX name (2-6 words)", state.lib_edit_fx,
                             sizeof state.lib_edit_fx);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##desc", "one-line description", state.lib_edit_desc,
                             sizeof state.lib_edit_desc);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##kw", "keywords, comma separated", state.lib_edit_kw,
                             sizeof state.lib_edit_kw);

    const bool cat_ok = state.lib_edit_catid[0] == '\0' || exact != nullptr;
    if (!cat_ok) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Save (locks)")) {
        sp::Annotation a = current ? *current : sp::Annotation{};
        a.cat_id = state.lib_edit_catid;
        a.fx_name = state.lib_edit_fx;
        a.description = state.lib_edit_desc;
        a.keywords = split_keywords(state.lib_edit_kw);
        a.source = sp::AnnotationSource::kHuman;
        a.confidence = 1.0;
        a.model.clear();
        a.prompt_version.clear();
        a.annotated_at.clear();
        a.locked = true;
        state.library->set_annotation(path, a, true);
        state.status_message = "annotation saved: " + path;
        state.lib_dirty = true;
    }
    if (!cat_ok) {
        ImGui::EndDisabled();
    }
    if (current && current->locked) {
        ImGui::SameLine();
        if (ImGui::Button("Unlock")) {
            state.library->set_locked(path, false);
            state.lib_dirty = true;
        }
    }
    if (current) {
        ImGui::SameLine();
        if (ImGui::Button("Clear")) {
            state.library->clear_annotation(path);
            state.lib_dirty = true;
        }
    }
    if (!state.annotating.load()) {
        ImGui::SameLine();
        if (ImGui::Button("Ask model")) {
            start_annotate(state, {path});
        }
    }

    if (current) {
        ImGui::TextDisabled(
            "%s%s  conf %.2f%s%s", sp::annotation_source_name(current->source),
            current->locked ? " [locked]" : "", current->confidence,
            current->model.empty() ? "" : ("  " + current->model).c_str(),
            current->prompt_version.empty() ? "" : ("  " + current->prompt_version).c_str());
        if (!current->annotated_at.empty()) {
            ImGui::TextDisabled("%s", current->annotated_at.c_str());
        }
        if (!current->candidates_json.empty()) {
            try {
                nlohmann::json cj = nlohmann::json::parse(current->candidates_json);
                if (cj.is_array() && !cj.empty()) {
                    ImGui::TextDisabled("model shortlist:");
                    int n = 0;
                    for (const auto &c : cj) {
                        std::string cid = c.value("cat_id", "");
                        if (cid.empty() || cid == current->cat_id) {
                            continue;
                        }
                        if (n++ > 0) {
                            ImGui::SameLine();
                        }
                        if (ImGui::SmallButton(cid.c_str())) {
                            copy_to(state.lib_edit_catid, sizeof state.lib_edit_catid, cid);
                        }
                        if (n >= 6) {
                            break;
                        }
                    }
                }
            } catch (const std::exception &) {
                // ignore malformed shortlist
            }
        }
    } else {
        ImGui::TextDisabled("unannotated");
    }
}

} // namespace spapp
