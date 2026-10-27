// Midnight Club LA VR.exe: the release's one-click start.
//
// Built on this PC, the game is in bin\mcla.exe: the launcher starts it in VR (any options given to the
// launcher are passed on; --setup runs the first-run setup again). Not built yet, it asks for your own
// disc image and runs setup\setup.ps1, which extracts the game from it, recompiles the game's code and
// builds bin\mcla.exe here; then it starts the game. Nothing of the game ships with the release.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shobjidl.h>
#include <shellapi.h>

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kTitle[] = L"Midnight Club LA VR";

fs::path ExeFolder() {
  wchar_t path[MAX_PATH * 4];
  const DWORD length = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
  return fs::path(std::wstring(path, length)).parent_path();
}

std::wstring Quote(const std::wstring& text) { return L"\"" + text + L"\""; }

// The launcher's own options after the program name, as typed.
std::wstring Arguments() {
  std::wstring line = GetCommandLineW();
  size_t i = 0;
  if (!line.empty() && line[0] == L'"') {
    i = line.find(L'"', 1);
    i = i == std::wstring::npos ? line.size() : i + 1;
  } else {
    while (i < line.size() && line[i] != L' ' && line[i] != L'\t') ++i;
  }
  while (i < line.size() && (line[i] == L' ' || line[i] == L'\t')) ++i;
  return line.substr(i);
}

bool Run(const std::wstring& command, const fs::path& folder, DWORD flags, bool wait, DWORD* exit_code) {
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  std::wstring mutable_command = command;
  if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, flags, nullptr,
                      folder.c_str(), &startup, &process)) {
    return false;
  }
  if (wait) {
    WaitForSingleObject(process.hProcess, INFINITE);
    if (exit_code) GetExitCodeProcess(process.hProcess, exit_code);
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}

// The disc image, from a file dialog; empty when cancelled.
std::wstring PickIso() {
  std::wstring result;
  IFileOpenDialog* dialog = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
    return result;
  }
  const COMDLG_FILTERSPEC filters[] = {{L"Xbox 360 disc image (*.iso)", L"*.iso"}, {L"All files", L"*.*"}};
  dialog->SetFileTypes(UINT(std::size(filters)), filters);
  dialog->SetTitle(L"Your Midnight Club: Los Angeles Complete Edition disc image (Xbox 360, .iso)");
  if (SUCCEEDED(dialog->Show(nullptr))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item))) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        result = path;
        CoTaskMemFree(path);
      }
      item->Release();
    }
  }
  dialog->Release();
  return result;
}

bool Setup(const fs::path& root) {
  const int answer = MessageBoxW(
      nullptr,
      L"First run: the game is built on this PC from your own disc image.\n\n"
      L"You need Midnight Club: Los Angeles Complete Edition for Xbox 360 (USA/Europe, title 545407F8) as an "
      L".iso file. The release contains none of the game: the files are extracted from your image and the game's "
      L"code is recompiled here.\n\n"
      L"It takes 10 to 30 minutes and about 15 GB of disk. If Microsoft's C++ Build Tools are not installed, setup "
      L"installs them from Microsoft (several GB; Windows asks for administrator rights once).\n\n"
      L"Choose your disc image now?",
      kTitle, MB_OKCANCEL | MB_ICONINFORMATION);
  if (answer != IDOK) {
    return false;
  }
  const std::wstring iso = PickIso();
  if (iso.empty()) {
    return false;
  }
  const fs::path script = root / L"setup" / L"setup.ps1";
  const std::wstring command = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File " + Quote(script.wstring()) +
                               L" -Iso " + Quote(iso);
  DWORD code = 1;
  if (!Run(command, root, CREATE_NEW_CONSOLE, true, &code)) {
    MessageBoxW(nullptr, L"Could not start PowerShell to run setup\\setup.ps1.", kTitle, MB_OK | MB_ICONERROR);
    return false;
  }
  if (code != 0) {
    MessageBoxW(nullptr, L"Setup did not finish. The logs are in setup\\logs; start the game again to continue.",
                kTitle, MB_OK | MB_ICONERROR);
    return false;
  }
  return true;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  const fs::path root = ExeFolder();
  const fs::path game = root / L"bin" / L"mcla.exe";
  std::wstring arguments = Arguments();
  bool setup = false;
  if (const size_t at = arguments.find(L"--setup"); at != std::wstring::npos) {
    arguments.erase(at, 7);
    setup = true;
  }
  std::error_code error;
  const bool built = fs::exists(game, error) && fs::exists(root / L"game" / L"default.xex", error);
  if ((setup || !built) && !Setup(root)) {
    return 1;
  }
  if (!fs::exists(game, error)) {
    return 1;
  }
  // VR unless the options say otherwise (--vr=false plays on the monitor).
  if (arguments.find(L"--vr") == std::wstring::npos) {
    arguments = L"--vr=true " + arguments;
  }
  if (!Run(Quote(game.wstring()) + L" " + arguments, game.parent_path(), 0, false, nullptr)) {
    MessageBoxW(nullptr, L"Could not start bin\\mcla.exe.", kTitle, MB_OK | MB_ICONERROR);
    return 1;
  }
  return 0;
}
