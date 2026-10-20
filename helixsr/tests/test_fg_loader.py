#!/usr/bin/env python3
"""Execute production provider selection against isolated fake installation trees."""
from pathlib import Path
import subprocess, tempfile, os
root=Path(__file__).resolve().parents[2]
s=(root/'OptiScaler/helixsr/HelixSR.cpp').read_text(); a=s.index('HMODULE LoadFfxFrameGenDll()'); b=s.index('\nbool LoadsAsIs',a)
code=r'''
#include <filesystem>
#include <vector>
#include <string>
#include <optional>
#include <fstream>
#include <cassert>
#include <cwchar>
namespace fs=std::filesystem;
using HMODULE=void*;
#define LOG_INFO(...)
#define LOG_ERROR(...)
int _wcsicmp(const wchar_t* a,const wchar_t* b){return std::wcscmp(a,b);}
struct Option {std::wstring value_or(const wchar_t*){return L"";}};
struct Config {Option FfxDx12SRPath; static Config* Instance(){static Config c;return &c;}};
fs::path rootPath; fs::path loaded; bool valid=true; bool legacyFound=false;
namespace Util {fs::path ExePath(){return rootPath/"game.exe";} fs::path DllPath(){return rootPath/"OptiScaler.dll";}}
bool FsrDllFound(){return true;} fs::path FsrDllPath(){return rootPath/"HelixSR-fsrfg"/"amd_fidelityfx_dx12.dll";}
bool Found(){return legacyFound;} fs::path DllPath(){return rootPath/"HelixSR"/"helixsr.dll";}
HMODULE LoadNgxFgDll(const fs::path& p){loaded=p;return valid?(void*)1:nullptr;}
'''+s[a:b]+r'''
int main(int,char** argv){rootPath=argv[1]; fs::create_directories(FsrDllPath().parent_path());
assert(!LoadFfxFrameGenDll()); assert(loaded.empty());
std::ofstream(FsrDllPath()).put('x'); assert(LoadFfxFrameGenDll()); assert(loaded==FsrDllPath());
const auto dedicated=FsrDllPath().parent_path()/"helixsr_fg.dll";
std::ofstream(dedicated).put('x'); assert(LoadFfxFrameGenDll()); assert(loaded==dedicated);
valid=false; assert(!LoadFfxFrameGenDll()); assert(loaded==dedicated);
fs::remove(dedicated); fs::create_directory(dedicated); assert(!LoadFfxFrameGenDll()); assert(loaded==dedicated);
fs::remove(dedicated); fs::remove(FsrDllPath()); valid=true; legacyFound=true; fs::create_directories(DllPath().parent_path()); std::ofstream(DllPath()).put('x'); assert(LoadFfxFrameGenDll()); assert(loaded==DllPath());
}
'''
with tempfile.TemporaryDirectory(prefix='helixsr-loader-',dir=Path.home()) as tmp:
 p=Path(tmp);(p/'test.cpp').write_text(code)
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-Wall','-Wextra','-Werror',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test'),str(p/'installation')],check=True)
print('PASS: production loader: absent, legacy fallback, dedicated priority, malformed dedicated blocks fallback, old folder')
