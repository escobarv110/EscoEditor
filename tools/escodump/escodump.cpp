// =============================================================================
//  EscoDump.asi - a decrypted copy of the game's own code, for porting
//  EscoEditor to GTA V Enhanced.
//
//  GTA5_Enhanced.exe on disk is packed (its .text has 7.99 bits of entropy and
//  not a single function prologue in it), so the byte patterns EscoEditor scans
//  for cannot be worked out from the file. In memory the code is plain, so this
//  plugin reads its own process and writes the module out as a flat image where
//  a file offset IS the rva - which is all the offline analysis needs.
//
//  It hooks nothing, writes nothing into the game and changes no memory: it
//  only reads. Story Mode only - never load it with GTA Online.
//
//  F9 writes a dump. One is also written by itself 150 s after the game starts,
//  in case F9 is inconvenient. Six at most, then it stops.
// =============================================================================
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>

namespace
{
	char g_dir[MAX_PATH] = {};       // the folder this .asi sits in
	char g_logPath[MAX_PATH] = {};
	int  g_dumps = 0;

	void logf(const char* fmt, ...)
	{
		char line[1200];
		SYSTEMTIME t; GetLocalTime(&t);
		const int head = snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
		va_list ap; va_start(ap, fmt);
		vsnprintf(line + head, sizeof(line) - (size_t)head - 2, fmt, ap);
		va_end(ap);
		strcat_s(line, "\r\n");
		HANDLE h = CreateFileA(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h != INVALID_HANDLE_VALUE)
		{
			DWORD w = 0;
			WriteFile(h, line, (DWORD)strlen(line), &w, nullptr);
			CloseHandle(h);
		}
	}

	// memcpy behind __try: a page can vanish between the query and the read.
	bool copyGuarded(uint8_t* dst, const uint8_t* src, size_t n)
	{
		__try { memcpy(dst, src, n); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	bool writeAll(const char* path, const uint8_t* p, size_t n)
	{
		HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE) return false;
		bool ok = true;
		for (size_t off = 0; off < n; )
		{
			const DWORD chunk = (DWORD)((n - off > (16u << 20)) ? (16u << 20) : (n - off));
			DWORD w = 0;
			if (!WriteFile(h, p + off, chunk, &w, nullptr) || w != chunk) { ok = false; break; }
			off += w;
		}
		CloseHandle(h);
		return ok;
	}

	void dumpNow(const char* why)
	{
		if (g_dumps >= 6) return;
		const uint8_t* base = (const uint8_t*)GetModuleHandleA(nullptr);
		if (!base) { logf("dump: no main module?"); return; }
		const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
		const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE) { logf("dump: the main module has no PE header"); return; }
		const size_t size = nt->OptionalHeader.SizeOfImage;

		uint8_t* buf = (uint8_t*)VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (!buf) { logf("dump: could not get %llu bytes of scratch", (unsigned long long)size); return; }

		size_t got = 0, missed = 0;
		for (size_t off = 0; off < size; )
		{
			const uint8_t* q = base + off;
			MEMORY_BASIC_INFORMATION mbi;
			if (!VirtualQuery(q, &mbi, sizeof(mbi))) { missed += 0x1000; off += 0x1000; continue; }
			size_t len = (size_t)((const uint8_t*)mbi.BaseAddress + mbi.RegionSize - q);
			if (off + len > size) len = size - off;
			if (!len) break;
			const bool readable = mbi.State == MEM_COMMIT &&
				!(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && mbi.Protect != 0;
			if (readable && copyGuarded(buf + off, q, len)) got += len;
			else missed += len;
			off += len;
		}

		const int n = ++g_dumps;
		char bin[MAX_PATH], txt[MAX_PATH];
		snprintf(bin, sizeof(bin), "%sEscoDump_%d.bin", g_dir, n);
		snprintf(txt, sizeof(txt), "%sEscoDump_%d.txt", g_dir, n);

		if (!writeAll(bin, buf, size))
			logf("dump %d: could not write %s (disk full, or no permission to write in the game folder)", n, bin);
		else
		{
			// the sidecar: everything the offline analysis needs to read the .bin
			char info[4096];
			int k = snprintf(info, sizeof(info),
				"EscoDump %d - %s\r\n"
				"module base   0x%llX\r\n"
				"SizeOfImage   0x%llX (%llu bytes)\r\n"
				"TimeDateStamp 0x%08X, checksum 0x%08X\r\n"
				"pages read    %llu, unreadable (zero filled) %llu\r\n"
				"file offset IS the rva - offset 0 is the module base.\r\n"
				"sections (from the headers in memory):\r\n",
				n, why, (unsigned long long)(uintptr_t)base, (unsigned long long)size, (unsigned long long)size,
				(unsigned)nt->FileHeader.TimeDateStamp, (unsigned)nt->OptionalHeader.CheckSum,
				(unsigned long long)got, (unsigned long long)missed);
			const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
			for (int i = 0; i < nt->FileHeader.NumberOfSections && k < (int)sizeof(info) - 200; ++i, ++sec)
				k += snprintf(info + k, sizeof(info) - (size_t)k, "  %-10.8s rva 0x%08X  vsize 0x%08X  flags 0x%08X\r\n",
					(const char*)sec->Name, (unsigned)sec->VirtualAddress, (unsigned)sec->Misc.VirtualSize, (unsigned)sec->Characteristics);
			writeAll(txt, (const uint8_t*)info, (size_t)k);
			logf("dump %d written: %s (%llu MB, %llu bytes unreadable) - %s", n, bin,
				(unsigned long long)(size >> 20), (unsigned long long)missed, why);
		}
		VirtualFree(buf, 0, MEM_RELEASE);
	}

	DWORD WINAPI Worker(LPVOID)
	{
		logf("EscoDump attached (build " __DATE__ " " __TIME__ ") - F9 writes a dump, one comes by itself after 150 s");
		const ULONGLONG start = GetTickCount64();
		bool autoDone = false, held = false;
		for (;;)
		{
			Sleep(100);
			const bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
			if (down && !held) dumpNow("F9");
			held = down;
			if (!autoDone && GetTickCount64() - start > 150000)
			{
				autoDone = true;
				dumpNow("150 s after the game started");
			}
		}
	}
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(self);
		GetModuleFileNameA(self, g_dir, sizeof(g_dir));
		char* slash = strrchr(g_dir, '\\');
		if (slash) slash[1] = 0;
		snprintf(g_logPath, sizeof(g_logPath), "%sEscoDump.log", g_dir);
		CloseHandle(CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr));
	}
	return TRUE;
}
