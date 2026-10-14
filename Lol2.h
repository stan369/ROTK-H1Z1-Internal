#ifndef MAIN_H
#define MAIN_H

#pragma once

#include <Windows.h>
#include <TlHelp32.h>
#include <Winternl.h>
#include <ntstatus.h>
#include <Psapi.h>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <functional>
#include <list>
#include <dwmapi.h>
#include <Lmcons.h>
#include <cmath>
#include <math.h>
#include <vector>
#include <map>
#include <algorithm>
#include "Lol1.h"
#include "DX9.h"
#include "Math.h"
#include "Offs.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment (lib, "ntdll.lib")
#define INFO_BUFFER_SIZE 32767
TCHAR  infoBuf[INFO_BUFFER_SIZE];
DWORD  bufCharCount = INFO_BUFFER_SIZE;

EXTERN_C NTSTATUS NTAPI NtReadVirtualMemory(HANDLE, PVOID, PVOID, ULONG, PULONG);
EXTERN_C NTSTATUS NTAPI NtWriteVirtualMemory(HANDLE, PVOID, PVOID, ULONG, PULONG);

struct PN {
	char name[32] = "";
};

#endif