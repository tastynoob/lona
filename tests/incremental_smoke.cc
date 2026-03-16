#include "lona/driver/session.hh"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

[[noreturn]] void
finishProcess(int exitCode) {
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(exitCode);
}

void
writeFile(const fs::path &path, const std::string &content) {
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("failed to open `" + path.string() + "` for writing");
    }
    out << content;
}

bool
expect(bool condition, const std::string &message) {
    if (condition) {
        return true;
    }
    std::cerr << message << '\n';
    return false;
}

bool
runBuild(lona::CompilerSession &session, const fs::path &rootPath,
         std::size_t expectedCompiled, std::size_t expectedReused) {
    lona::SessionOptions options;
    options.outputMode = lona::OutputMode::LLVMIR;
    options.compile.verifyIR = true;

    std::ostringstream out;
    std::ostringstream diag;
    int exitCode = session.runFile(rootPath.string(), options, out, diag);
    if (!expect(exitCode == 0, "incremental smoke compile failed:\n" + diag.str())) {
        return false;
    }
    if (!expect(diag.str().empty(), "incremental smoke emitted diagnostics unexpectedly")) {
        return false;
    }
    if (!expect(!out.str().empty(), "incremental smoke produced empty LLVM IR")) {
        return false;
    }
    if (!expect(session.lastStats().compiledModules == expectedCompiled,
                "unexpected compiled module count")) {
        return false;
    }
    if (!expect(session.lastStats().reusedModules == expectedReused,
                "unexpected reused module count")) {
        return false;
    }
    return true;
}

}  // namespace

int
main() {
    try {
        const auto dir = fs::temp_directory_path() / "lona_incremental_smoke";
        fs::create_directories(dir);

        const auto depPath = dir / "dep.lo";
        const auto appPath = dir / "app.lo";

        writeFile(depPath,
                  "def inc(a i32) i32 {\n"
                  "    ret a + 1\n"
                  "}\n");
        writeFile(appPath,
                  "import dep\n"
                  "\n"
                  "def main() i32 {\n"
                  "    ret dep.inc(4)\n"
                  "}\n");

        lona::CompilerSession session;
        if (!runBuild(session, appPath, 2, 0)) {
            finishProcess(1);
        }

        writeFile(depPath,
                  "def inc(a i32) i32 {\n"
                  "    ret a + 2\n"
                  "}\n");
        if (!runBuild(session, appPath, 1, 1)) {
            finishProcess(1);
        }

        writeFile(depPath,
                  "def inc(a i32) i32 {\n"
                  "    ret a + 2\n"
                  "}\n"
                  "\n"
                  "def helper(a i32) i32 {\n"
                  "    ret a\n"
                  "}\n");
        if (!runBuild(session, appPath, 2, 0)) {
            finishProcess(1);
        }

        const auto dirA = dir / "project_a";
        const auto dirB = dir / "project_b";
        fs::create_directories(dirA);
        fs::create_directories(dirB);

        writeFile(dirA / "dep.lo",
                  "def inc(a i32) i32 {\n"
                  "    ret a + 10\n"
                  "}\n");
        writeFile(dirA / "app.lo",
                  "import dep\n"
                  "\n"
                  "def main() i32 {\n"
                  "    ret dep.inc(1)\n"
                  "}\n");
        if (!runBuild(session, dirA / "app.lo", 2, 0)) {
            finishProcess(1);
        }

        writeFile(dirB / "dep.lo",
                  "def inc(a i32) i32 {\n"
                  "    ret a + 20\n"
                  "}\n");
        writeFile(dirB / "app.lo",
                  "import dep\n"
                  "\n"
                  "def main() i32 {\n"
                  "    ret dep.inc(2)\n"
                  "}\n");
        if (!runBuild(session, dirB / "app.lo", 2, 0)) {
            finishProcess(1);
        }
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << '\n';
        finishProcess(1);
    }

    finishProcess(0);
}
