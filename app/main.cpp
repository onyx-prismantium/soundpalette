// soundpalette-app: GLFW + OpenGL 3 + Dear ImGui (docking) bootstrap and --smoke mode (§10).

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <nfd.h>

#include "miniaudio.h"
#include "stb_image_write.h" // implementation lives in gui/vendor_stb_impl.cpp

#include "gui/app_state.h"
#include "soundpalette/manifest.h"

namespace {

void glfw_error_callback(int error, const char *description) {
    std::fprintf(stderr, "soundpalette-app: GLFW error %d: %s\n", error, description);
}

bool write_framebuffer_png(GLFWwindow *window, const std::string &out_path) {
    int width = 0, height = 0;
    glfwGetFramebufferSize(window, &width, &height);
    if (width <= 0 || height <= 0) {
        std::fprintf(stderr, "soundpalette-app: zero-sized framebuffer\n");
        return false;
    }
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) *
                                      static_cast<std::size_t>(height) * 4u);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    if (glGetError() != GL_NO_ERROR) {
        std::fprintf(stderr, "soundpalette-app: glReadPixels failed\n");
        return false;
    }
    stbi_flip_vertically_on_write(1); // GL rows are bottom-up
    if (stbi_write_png(out_path.c_str(), width, height, 4, pixels.data(), width * 4) == 0) {
        std::fprintf(stderr, "soundpalette-app: cannot write %s\n", out_path.c_str());
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char **argv) {
    std::string smoke_out;
    std::string initial_dir;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke") == 0 && i + 1 < argc) {
            smoke_out = argv[++i];
        } else if (std::strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
            initial_dir = argv[++i];
        } else {
            std::fprintf(stderr, "usage: soundpalette-app [--smoke <out.png> [--dir <folder>]]\n");
            return 2;
        }
    }
    const bool smoke = !smoke_out.empty();

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        std::fprintf(stderr, "soundpalette-app: glfwInit failed\n");
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow *window = glfwCreateWindow(1280, 800, "SoundPalette", nullptr, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "soundpalette-app: glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = nullptr; // deterministic layout; no imgui.ini side effects
    ImGui::StyleColorsDark();

    if (!ImGui_ImplGlfw_InitForOpenGL(window, true) || !ImGui_ImplOpenGL3_Init("#version 330")) {
        std::fprintf(stderr, "soundpalette-app: ImGui backend init failed\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    if (!smoke) {
        NFD_Init(); // dialogs are only opened from menu/tuner actions, never in smoke mode
    }

    spapp::AppState state;
    state.smoke_mode = smoke;

    // Playback engine (§10): one ma_engine; absent sound device (headless) is not an error.
    ma_engine engine;
    if (ma_engine_init(nullptr, &engine) == MA_SUCCESS) {
        state.engine = &engine;
        state.audio_ok = true;
    }

    if (!initial_dir.empty()) {
        // Smoke mode scans synchronously so the grid is populated in the captured frame.
        auto t0 = std::chrono::steady_clock::now();
        state.root_dir = initial_dir;
        state.manifest = sp::scan_directory(initial_dir, sp::ScanOptions{});
        state.last_scan_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        spapp::rebuild_visuals(state);
    }

    int exit_code = 0;
    int frames_rendered = 0;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        spapp::draw_ui(state);
        if (state.want_quit) {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

        ImGui::Render();
        int fb_w = 0, fb_h = 0;
        glfwGetFramebufferSize(window, &fb_w, &fb_h);
        glViewport(0, 0, fb_w, fb_h);
        glClearColor(0.10f, 0.10f, 0.11f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        ++frames_rendered;
        if (smoke && frames_rendered >= 30) {
            // Capture before the swap: the back buffer holds the frame just rendered.
            if (!write_framebuffer_png(window, smoke_out)) {
                exit_code = 1;
            }
            glfwSwapBuffers(window);
            break;
        }
        glfwSwapBuffers(window);
    }

    spapp::shutdown_scan_thread(state);
    if (state.audio_ok) {
        ma_engine_uninit(&engine);
    }
    if (!smoke) {
        NFD_Quit();
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return exit_code;
}
