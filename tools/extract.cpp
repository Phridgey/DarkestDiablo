// Copies files out of the game's CASC storage. Usage:
//   extract <gameDir> <outDir> <substring>...      extract every file whose name contains one of them
//   extract <gameDir> - <substring>...             only list the matches
//   extract <gameDir> <outDir> @<listFile>         substrings from a file, one per line (# comments)
// Names look like data/global/excel/levels.txt. tools/vanilla_files.txt lists what the build needs.
// Build: tools\build-extract.cmd (Windows) or CMake in tools/extract (needs CascLib, fetched there).
#include "CascLib.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <direct.h>
static void make_dir(const char* p) { _mkdir(p); }
#else
#include <sys/stat.h>
static void make_dir(const char* p) { mkdir(p, 0755); }
#endif

static void mkdirs(std::string p) {
  for (size_t i = 1; i < p.size(); i++)
    if (p[i] == '/') { p[i] = 0; make_dir(p.c_str()); p[i] = '/'; }
}

int main(int argc, char** argv) {
  if (argc < 4) { fprintf(stderr, "usage: extract <gameDir> <outDir|-> <substring|@listFile>...\n"); return 1; }
  std::vector<std::string> want;
  for (int i = 3; i < argc; i++) {
    if (argv[i][0] == '@') {
      std::ifstream in(argv[i] + 1);
      if (!in) { fprintf(stderr, "can't read %s\n", argv[i] + 1); return 1; }
      std::string line;
      while (std::getline(in, line)) {
        line = line.substr(0, line.find('#'));
        while (!line.empty() && (line.back() == ' ' || line.back() == '\r' || line.back() == '\t')) line.pop_back();
        while (!line.empty() && (line[0] == ' ' || line[0] == '\t')) line.erase(0, 1);
        if (!line.empty()) want.push_back(line);
      }
    } else {
      want.push_back(argv[i]);
    }
  }
  HANDLE st;
  if (!CascOpenStorage(argv[1], 0, &st)) { fprintf(stderr, "open failed %d\n", GetCascError()); return 2; }
  CASC_FIND_DATA fd;
  HANDLE f = CascFindFirstFile(st, "*", &fd, NULL);
  if (!f) { fprintf(stderr, "find failed\n"); return 3; }
  bool list = !strcmp(argv[2], "-");
  long n = 0; long long bytes = 0; int failed = 0;
  do {
    std::string name = fd.szFileName;
    for (auto& c : name) if (c == '\\') c = '/';
    bool hit = false;
    for (auto& w : want) if (name.find(w) != std::string::npos) hit = true;
    if (!hit || !fd.bFileAvailable) continue;
    if (list) { printf("%s\n", name.c_str()); n++; continue; }
    std::string rel = name; size_t c = rel.find(':'); if (c != std::string::npos) rel = rel.substr(c + 1);
    HANDLE h;
    if (!CascOpenFile(st, fd.szFileName, 0, CASC_OPEN_BY_NAME, &h)) { fprintf(stderr, "skip %s\n", name.c_str()); failed++; continue; }
    std::string out = std::string(argv[2]) + "/" + rel; mkdirs(out);
    FILE* o = fopen(out.c_str(), "wb");
    if (!o) { fprintf(stderr, "can't write %s\n", out.c_str()); CascCloseFile(h); failed++; continue; }
    char buf[65536]; DWORD r;
    while (CascReadFile(h, buf, sizeof buf, &r) && r) { fwrite(buf, 1, r, o); bytes += r; }
    fclose(o); CascCloseFile(h); n++;
  } while (CascFindNextFile(f, &fd));
  CascFindClose(f); CascCloseStorage(st);
  fprintf(stderr, "%ld files, %lld bytes, %d failed\n", n, bytes, failed);
  if (!list && n == 0) { fprintf(stderr, "nothing matched\n"); return 4; }
  return 0;
}
