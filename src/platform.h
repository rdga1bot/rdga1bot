#pragma once
// platform.h — централізовані платформо-залежні типи та включення (MR66).
// Замінює прямі #include <sys/types.h> / <sys/uio.h> в заголовках.
// Windows: визначає pid_t/ssize_t через Windows.h-типи.
// Linux: включає POSIX-заголовки.

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <tlhelp32.h>
  #include <psapi.h>
  using pid_t   = DWORD;
  using ssize_t = SSIZE_T;
#else
  #include <sys/types.h>
  #include <sys/uio.h>
  #include <unistd.h>
  #include <dirent.h>
#endif
