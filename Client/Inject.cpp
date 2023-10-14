#include <vector>
#include <stdio.h>
#include "Inject.h"
#include "Library/skCrypter.h"
#include "Driver/Driver.h"
#include "Utils/MemoryUtils.h"
#include "Utils/Hijack.h"

#pragma comment(lib, "ntdll.lib")

extern "C" NTSYSAPI PIMAGE_NT_HEADERS NTAPI RtlImageNtHeader(PVOID Base);

// FACE Injector

BOOL
RelocateImage(
	PVOID pRemoteImg,
	PVOID pLocalImg,
	PIMAGE_NT_HEADERS NtHead
) {
	typedef struct _RELOC_ENTRY {
		ULONG ToRVA;
		ULONG Size;
		struct
		{
			WORD Offset : 12;
			WORD Type : 4;
		} Item[1];
	} RELOC_ENTRY, * PRELOC_ENTRY;

	ULONGLONG DeltaOffset = (ULONGLONG)pRemoteImg - NtHead->OptionalHeader.ImageBase;
	if (!DeltaOffset)
		return TRUE;
	else if (!(NtHead->OptionalHeader.DllCharacteristics & IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE))
		return FALSE;

	PRELOC_ENTRY RelocEnt = (PRELOC_ENTRY)MemoryUtils::RVA_VA(NtHead->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress, NtHead, pLocalImg);
	ULONGLONG RelocEnd = (ULONGLONG)RelocEnt + NtHead->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size;

	if (RelocEnt == nullptr)
		return TRUE;

	while ((uintptr_t)RelocEnt < RelocEnd && RelocEnt->Size) {
		DWORD RecordsCount = (RelocEnt->Size - 8) >> 1;
		for (DWORD i = 0; i < RecordsCount; i++) {
			WORD FixType = (RelocEnt->Item[i].Type);
			WORD ShiftDelta = (RelocEnt->Item[i].Offset) % 4096;

			if (FixType == IMAGE_REL_BASED_ABSOLUTE)
				continue;

			if (FixType == IMAGE_REL_BASED_HIGHLOW || FixType == IMAGE_REL_BASED_DIR64) {
				uintptr_t FixVA = (uintptr_t)MemoryUtils::RVA_VA(RelocEnt->ToRVA, NtHead, pLocalImg);

				if (!FixVA)
					FixVA = (uintptr_t)pLocalImg;

				*(uintptr_t*)(FixVA + ShiftDelta) += DeltaOffset;
			}
		}

		RelocEnt = (PRELOC_ENTRY)((LPBYTE)RelocEnt + RelocEnt->Size);
	}
	return TRUE;
}

ULONGLONG
ResolveFunctionAddress(
	LPCSTR ModName,
	LPCSTR ModFunc
) {
	HMODULE hModule = LoadLibraryExA(ModName, NULL, DONT_RESOLVE_DLL_REFERENCES);
	ULONGLONG FuncOffset = (ULONGLONG)GetProcAddress(hModule, ModFunc);
	FuncOffset -= (ULONGLONG)hModule;
	FreeLibrary(hModule);

	return FuncOffset;
}

BOOL
ResolveImport(
	DWORD ThreadId,
	DWORD ProcessId,
	PVOID pLocalImg,
	PIMAGE_NT_HEADERS NtHead
) {
	PIMAGE_IMPORT_DESCRIPTOR ImportDesc = (PIMAGE_IMPORT_DESCRIPTOR)MemoryUtils::RVA_VA(NtHead->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress, NtHead, pLocalImg);
	if (!NtHead->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress || !NtHead->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size) \
		return TRUE;

	LPSTR ModuleName = NULL;
	while ((ModuleName = (LPSTR)MemoryUtils::RVA_VA(ImportDesc->Name, NtHead, pLocalImg))) {
		uintptr_t BaseImage = (uintptr_t)LoadLibraryA(ModuleName);

		if (!BaseImage)
			return FALSE;

		PIMAGE_THUNK_DATA IhData = (PIMAGE_THUNK_DATA)MemoryUtils::RVA_VA(ImportDesc->FirstThunk, NtHead, pLocalImg);
		while (IhData->u1.AddressOfData) {
			if (IhData->u1.Ordinal & IMAGE_ORDINAL_FLAG)
				IhData->u1.Function = BaseImage + ResolveFunctionAddress(ModuleName, (LPCSTR)(IhData->u1.Ordinal & 0xFFFF));
			else {
				IMAGE_IMPORT_BY_NAME* IBN = (PIMAGE_IMPORT_BY_NAME)MemoryUtils::RVA_VA(IhData->u1.AddressOfData, NtHead, pLocalImg);
				IhData->u1.Function = BaseImage + ResolveFunctionAddress(ModuleName, (LPCSTR)IBN->Name);
			} IhData++;
		} ImportDesc++;
	} return true;
}

VOID
WriteSections(
	DWORD ProcessId,
	PVOID pModuleBase,
	PVOID LocalImage,
	PIMAGE_NT_HEADERS NtHead
) {
	PIMAGE_SECTION_HEADER Section = IMAGE_FIRST_SECTION(NtHead);
	for (WORD SectionCount = 0; SectionCount < NtHead->FileHeader.NumberOfSections; SectionCount++, Section++) {
		NTSTATUS WriteStatus = Driver::API::WriteMemory((HANDLE)ProcessId, (DWORD64)((ULONGLONG)pModuleBase + Section->VirtualAddress), Section->SizeOfRawData, (PVOID)((ULONGLONG)LocalImage + Section->PointerToRawData));
	}
}

VOID
EraseDiscardableSect(
	DWORD ProcessId,
	PVOID pModuleBase,
	PIMAGE_NT_HEADERS NtHead
) {
	PIMAGE_SECTION_HEADER Section = IMAGE_FIRST_SECTION(NtHead);
	for (WORD SectionCount = 0; SectionCount < NtHead->FileHeader.NumberOfSections; SectionCount++, Section++) {
		if (Section->SizeOfRawData == 0)
			continue;

		if (Section->Characteristics & IMAGE_SCN_MEM_DISCARDABLE) {
			PVOID pZeroMemory = VirtualAlloc(NULL, Section->SizeOfRawData, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
			Driver::API::WriteMemory((HANDLE)ProcessId, (DWORD64)((ULONGLONG)pModuleBase + Section->VirtualAddress), Section->SizeOfRawData, pZeroMemory);
			VirtualFree(pZeroMemory, 0, MEM_RELEASE);
		}
	}
}

VOID
Inject::Map(
	LPCSTR WindowClassName,
	LPCSTR DllPath,
	PCHAR SpoofPageProtection,
	PCHAR RemoveVADNode,
	PCHAR AllocateBehindThreadStack
) {
	PVOID DllImage = MemoryUtils::GetDllFromFile(DllPath);
	if (!DllImage) {
		auto Text = skCrypt("Invalid dll\n");
		printf(Text);
		Text.clear();
		return;
	}

	PIMAGE_NT_HEADERS DllNtHead = RtlImageNtHeader(DllImage);
	if (!DllNtHead) {
		auto Text = skCrypt("Invalid PE header\n");
		printf(Text);
		Text.clear();
		return;
	}

	ULONG ThreadId = NULL, ProcessId = NULL;
	MemoryUtils::GetProcessIdAndThreadIdFromWindowClass(WindowClassName, &ProcessId, &ThreadId);
	if (!ThreadId || !ProcessId) {
		auto Text = skCrypt("Invalid thread id / process id\n");
		printf(Text);
		Text.clear();
		return;
	}

	PVOID AllocateBase = NULL;
	if (!strcmp(AllocateBehindThreadStack, "1")) {
		if (AllocateBase = MemoryUtils::GetLastThreadStack(ProcessId))
			Driver::API::AllocateVAD((HANDLE)ProcessId, AllocateBase, DllNtHead->OptionalHeader.SizeOfImage);
	}
	else {
		Driver::API::AllocMemory((HANDLE)ProcessId,
			&AllocateBase, 
			DllNtHead->OptionalHeader.SizeOfImage, 
			!strcmp(SpoofPageProtection, "1") ? PAGE_READWRITE : PAGE_EXECUTE_READWRITE);
	}

	if (!AllocateBase) {
		auto Text = skCrypt("Failed to allocate memory");
		printf(Text);
		Text.clear();
		return;
	}

	ULONG DllSize = DllNtHead->OptionalHeader.SizeOfImage;
	ULONG DllEntryPointOffset = DllNtHead->OptionalHeader.AddressOfEntryPoint;

	if (!RelocateImage(AllocateBase, DllImage, DllNtHead)) {
		Driver::API::FreeMemory((HANDLE)ProcessId, AllocateBase);
		auto Text = skCrypt("Failed to relocate image\n");
		printf(Text);
		Text.clear();
		return;
	}

	if (!ResolveImport(ThreadId, ProcessId, DllImage, DllNtHead)) {
		Driver::API::FreeMemory((HANDLE)ProcessId, AllocateBase);
		auto Text = skCrypt("Failed to resolve imports\n");
		printf(Text);
		Text.clear();
		return;
	}

	WriteSections(ProcessId, AllocateBase, DllImage, DllNtHead);
	EraseDiscardableSect(ProcessId, AllocateBase, DllNtHead);

	printf("Wrote DLL to process %i at address 0x%p\n", ProcessId, AllocateBase);

	if (!strcmp(SpoofPageProtection, "1")) {
		MemoryUtils::FlipExecutableBitForMemoryRegion((HANDLE)ProcessId, AllocateBase, 0);
	}
	else if (!strcmp(SpoofPageProtection, "2")) {
		MMVAD_FLAGS VadFlags{};
		Driver::API::GetVADFlags((HANDLE)ProcessId, AllocateBase, &VadFlags);
		VadFlags.Protection = PAGE_READWRITE;
		Driver::API::SetVADFlags((HANDLE)ProcessId, AllocateBase, VadFlags);
	}

	Hijack::CallViaSetWindowsHookEx(ProcessId, ThreadId, AllocateBase, DllNtHead);
	
	if (!strcmp(RemoveVADNode, "1")) {
		Driver::API::RemoveVADNode((HANDLE)ProcessId, AllocateBase);
	}

	VirtualFree(DllImage, 0, MEM_RELEASE);
}
