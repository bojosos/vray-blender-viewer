// vray_viewport_viewer - minimal standalone viewer for V-Ray shared-memory images
//
// ---- Build ----
//   cmake -S . -B build -G "Visual Studio 17 2022" -A x64
//   cmake --build build --config Release
//
//   GLFW is downloaded and built automatically by CMake (FetchContent).
//   No other dependencies required.
//
// ---- Setup ----
//   Viewport IPR  : works out of the box - just start rendering in Blender
//   Production    : set VRAY_BLENDER_PROGRESSIVE_UPDATES=1 before launching Blender
//   VFB IPR       : does NOT work - VFB images go through ZMQ, not shared memory
//
// ---- Run ----
//   vray_viewport_viewer.exe           (auto-detects VRayZmqServer.exe)
//   vray_viewport_viewer.exe <pid>     (explicit ZMQ server PID)
//
// Esc to quit

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <GLFW/glfw3.h>

// GL 1.5+ enums absent from the Windows SDK gl.h (stuck at GL 1.1)
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER   0x8B31
#endif
#ifndef GL_RGBA32F
#define GL_RGBA32F         0x8814
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE   0x812F
#endif

typedef char GLchar;

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Shared memory
//
// The ZMQ server writes rendered frames to Windows named file mappings:
//   name   : "vray-zmq-{pid}-imgid_{slot}-db"
//   layout : [uint32 payloadSize][int32 width][int32 height][float RGBA ...]
//
// Slot = generation*2 + bufferIndex. Generation increments on viewport resize.
// The server flips rows once before writing, so data is already in OpenGL
// bottom-up order.
// ---------------------------------------------------------------------------

struct ShmHeader { uint32_t payloadSize; int32_t width, height; };

static std::string slotName(const std::string& pid, int slot)
{
    return "vray-zmq-" + pid + "-imgid_" + std::to_string(slot) + "-db";
}

static std::vector<DWORD> findServerPIDs()
{
    std::vector<DWORD> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32 pe{ sizeof(pe) };
    if (Process32First(snap, &pe))
        do { if (!_stricmp(pe.szExeFile, "VRayZmqServer.exe")) out.push_back(pe.th32ProcessID); }
        while (Process32Next(snap, &pe));
    CloseHandle(snap);
    return out;
}

struct Frame { int w = 0, h = 0; std::vector<float> rgba; };

static bool readSlot(const std::string& pid, int slot, Frame& out)
{
    HANDLE hMap = OpenFileMappingA(FILE_MAP_READ, FALSE, slotName(pid, slot).c_str());
    if (!hMap) return false;
    const void* ptr = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(hMap);
    if (!ptr) return false;

    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(ptr, &mbi, sizeof(mbi));
    const size_t mapSize = mbi.RegionSize;

    bool ok = false;
    if (mapSize >= sizeof(ShmHeader)) {
        const auto* h = static_cast<const ShmHeader*>(ptr);
        const bool valid = h->width > 0 && h->width <= 16384 && h->height > 0 && h->height <= 16384;
        const size_t need = valid ? sizeof(ShmHeader) + (size_t)h->width * h->height * 4 * sizeof(float) : 0;
        if (valid && mapSize >= need) {
            const float* px = reinterpret_cast<const float*>(static_cast<const char*>(ptr) + sizeof(ShmHeader));
            out.w = h->width; out.h = h->height;
            out.rgba.assign(px, px + out.w * out.h * 4);
            ok = true;
        }
    }
    UnmapViewOfFile(ptr);
    return ok;
}

static int g_gen = 0;
static bool pollImage(const std::string& pid, Frame& out)
{
    // On Windows, old shared memory segments persist after new ones are created.
    // Higher generation number = more recent render. Keep advancing until no
    // higher generation exists, so we always display the newest image.
    for (;;) {
        bool step = false;
        for (int b = 0; b < 2; ++b)
            if (readSlot(pid, (g_gen + 1) * 2 + b, out)) { ++g_gen; step = true; break; }
        if (!step) break;
    }
    for (int b = 0; b < 2; ++b)
        if (readSlot(pid, g_gen * 2 + b, out)) return true;
    g_gen = 0; // nothing found - server restarted, start over next poll
    return false;
}

// ---------------------------------------------------------------------------
// OpenGL 3.3 loader (no GLAD - manual glfwGetProcAddress)
// ---------------------------------------------------------------------------

#define GL3_FUNCS(X) \
    X(GLuint, glCreateShader,    (GLenum t)) \
    X(void,   glShaderSource,    (GLuint s, GLsizei c, const GLchar** str, const GLint* len)) \
    X(void,   glCompileShader,   (GLuint s)) \
    X(GLuint, glCreateProgram,   (void)) \
    X(void,   glAttachShader,    (GLuint p, GLuint s)) \
    X(void,   glLinkProgram,     (GLuint p)) \
    X(void,   glUseProgram,      (GLuint p)) \
    X(void,   glDeleteShader,    (GLuint s)) \
    X(void,   glGenVertexArrays, (GLsizei n, GLuint* a)) \
    X(void,   glBindVertexArray, (GLuint a))

#define X_DECL(ret, name, args) static ret (*name) args = nullptr;
GL3_FUNCS(X_DECL)
#undef X_DECL

static void loadGL()
{
#define X_LOAD(ret, name, args) name = reinterpret_cast<decltype(name)>(glfwGetProcAddress(#name));
    GL3_FUNCS(X_LOAD)
#undef X_LOAD
}

static GLuint buildProgram(const char* vert, const char* frag)
{
    auto compile = [](GLenum t, const char* src) {
        GLuint s = glCreateShader(t); glShaderSource(s, 1, &src, nullptr); glCompileShader(s); return s;
    };
    GLuint vs = compile(GL_VERTEX_SHADER, vert), fs = compile(GL_FRAGMENT_SHADER, frag);
    GLuint p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs); glLinkProgram(p);
    glDeleteShader(vs); glDeleteShader(fs);
    return p;
}

// Fullscreen triangle via gl_VertexID - no vertex buffer needed.
static const char* kVert = R"GLSL(
#version 330 core
out vec2 vUV;
void main() {
    vec2 uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    vUV = uv;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
})GLSL";

// Linear RGBA from V-Ray -> sRGB gamma
static const char* kFrag = R"GLSL(
#version 330 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uTex;
void main() {
    vec3 c = texture(uTex, vUV).rgb;
    fragColor = vec4(pow(clamp(c, 0.0, 1.0), vec3(1.0 / 2.2)), 1.0);
})GLSL";

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    std::string pid = (argc > 1) ? argv[1] : "";
    if (pid.empty()) {
        auto pids = findServerPIDs();
        if (pids.empty()) {
            fprintf(stderr, "No VRayZmqServer.exe found - start a V-Ray render in Blender first.\n");
            return 1;
        }
        if (pids.size() > 1) {
            fprintf(stderr, "Multiple VRayZmqServer.exe instances found - pass the desired PID.\n");
            return 1;
        }
        pid = std::to_string(pids[0]);
    }

    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* win = glfwCreateWindow(960, 540, "V-Ray Viewer", nullptr, nullptr);
    if (!win) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);
    loadGL();

    GLuint prog = buildProgram(kVert, kFrag);
    GLuint vao;
    glGenVertexArrays(1, &vao);

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    int   texW = 0, texH = 0;
    Frame frame;
    auto  lastPoll = std::chrono::steady_clock::now() - std::chrono::seconds(1);

    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();
        if (glfwGetKey(win, GLFW_KEY_ESCAPE) == GLFW_PRESS) glfwSetWindowShouldClose(win, GLFW_TRUE);

        auto now = std::chrono::steady_clock::now();
        if (now - lastPoll >= std::chrono::milliseconds(33)) {
            lastPoll = now;
            if (pollImage(pid, frame) && frame.w > 0) {
                glBindTexture(GL_TEXTURE_2D, tex);
                if (frame.w != texW || frame.h != texH) {
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F,
                                 frame.w, frame.h, 0, GL_RGBA, GL_FLOAT, frame.rgba.data());
                    texW = frame.w; texH = frame.h;
                    glfwSetWindowSize(win, texW, texH);
                    char title[64]; snprintf(title, sizeof(title), "V-Ray Viewer  %dx%d", texW, texH);
                    glfwSetWindowTitle(win, title);
                } else {
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                                    frame.w, frame.h, GL_RGBA, GL_FLOAT, frame.rgba.data());
                }
            }
        }

        int fbW, fbH;
        glfwGetFramebufferSize(win, &fbW, &fbH);
        glViewport(0, 0, fbW, fbH);
        glClear(GL_COLOR_BUFFER_BIT);

        if (texW > 0) {
            glUseProgram(prog);
            glBindTexture(GL_TEXTURE_2D, tex);
            glBindVertexArray(vao);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }

        glfwSwapBuffers(win);
    }

    glfwTerminate();
    return 0;
}
