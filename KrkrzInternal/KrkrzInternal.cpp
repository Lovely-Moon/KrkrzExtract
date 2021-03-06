#include "stdafx.h"
#include "KrkrzInternal.h"
#include "tp_stub.h"
#include <my.h>
#include <string>
#include "SectionProtector.h"

#pragma comment(lib, "MyLibrary_x86_static.lib")

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

BEGIN_MESSAGE_MAP(CKrkrzInternalApp, CWinApp)
END_MESSAGE_MAP()



EXTERN_C MY_DLL_EXPORT HRESULT NTAPI V2Link(iTVPFunctionExporter *exporter)
{
	TVPInitImportStub(exporter);
	return S_OK;
}

EXTERN_C MY_DLL_EXPORT HRESULT NTAPI V2Unlink()
{
	return S_OK;
}


CKrkrzInternalApp::CKrkrzInternalApp()
{
	ml::MlInitialize();

	m_HostAlloc               = NULL;
	m_IStreamAdapterVtable    = NULL;
	m_CallTVPCreateStreamCall = NULL;
	m_TVPFunctionExporter     = NULL;
	m_Inited                  = FALSE;
	m_Module                  = NULL;
	m_V2LinkStub              = NULL;

	RtlZeroMemory(m_Path, sizeof(m_Path));

	InitializeCriticalSection(&m_LoadCS);
}

CKrkrzInternalApp theApp;

BOOL CKrkrzInternalApp::InitInstance()
{
	CWinApp::InitInstance();
	AFX_MODULE_STATE* State = AfxGetModuleState();

	m_Module = State->m_hCurrentInstanceHandle;
	GetModuleFileNameW(State->m_hCurrentInstanceHandle, m_Path, countof(m_Path));
	InitKrkrExtract(State->m_hCurrentInstanceHandle);

	return TRUE;
}


BOOL CKrkrzInternalApp::GetTVPCreateStreamCall()
{
	PVOID CallIStreamStub, CallIStream, CallTVPCreateStreamCall;
	ULONG OpSize, OpOffset;
	WORD  WordOpcode;

	static char funcname[] = "IStream * ::TVPCreateIStream(const ttstr &,tjs_uint32)";

	LOOP_ONCE
	{
		CallTVPCreateStreamCall = NULL;

		CallIStreamStub = TVPGetImportFuncPtr(funcname);
		if (!CallIStreamStub)
			break;

		CallIStream = NULL;
		OpOffset = 0;

		LOOP_FOREVER
		{
			if (((PBYTE)CallIStreamStub + OpOffset)[0] == 0xCC)
				break;

			WordOpcode = *(PWORD)((ULONG_PTR)CallIStreamStub + OpOffset);

			//mov edx,dword ptr [ebp+0xC]
			if (WordOpcode == 0x558B)
			{
				OpOffset += 2;
				if (((PBYTE)CallIStreamStub + OpOffset)[0] == 0xC)
				{
					OpOffset++;
					WordOpcode = *(PWORD)((ULONG_PTR)CallIStreamStub + OpOffset);
					//mov edx,dword ptr [ebp+0x8]
					if (WordOpcode == 0x4D8B)
					{
						OpOffset += 2;
						if (((PBYTE)CallIStreamStub + OpOffset)[0] == 0x8)
						{
							OpOffset++;
							if (((PBYTE)CallIStreamStub + OpOffset)[0] == CALL)
							{
								CallIStream = (PVOID)GetCallDestination(((ULONG_PTR)CallIStreamStub + OpOffset));
								OpOffset += 5;
								break;
							}
						}
					}
				}
			}

			//the next opcode
			OpSize = GetOpCodeSize32(((PBYTE)CallIStreamStub + OpOffset));
			OpOffset += OpSize;
		}

		if (!CallIStream)
			break;

		OpOffset = 0;
		LOOP_FOREVER
		{
			if (((PBYTE)CallIStream + OpOffset)[0] == 0xC3)
				break;

			//find the first call
			if (((PBYTE)CallIStream + OpOffset)[0] == CALL)
			{
				CallTVPCreateStreamCall = (PVOID)GetCallDestination(((ULONG_PTR)CallIStream + OpOffset));
				OpOffset += 5;
				break;
			}

			//the next opcode
			OpSize = GetOpCodeSize32(((PBYTE)CallIStream + OpOffset));
			OpOffset += OpSize;
		}

		LOOP_FOREVER
		{
			if (((PBYTE)CallIStream + OpOffset)[0] == 0xC3)
				break;

			if (((PBYTE)CallIStream + OpOffset)[0] == CALL)
			{
				//push 0xC
				//call HostAlloc
				//add esp, 0x4
				if (((PBYTE)CallIStream + OpOffset - 2)[0] == 0x6A &&
					((PBYTE)CallIStream + OpOffset - 2)[1] == 0x0C)
				{
					m_HostAlloc = (PVOID)GetCallDestination(((ULONG_PTR)CallIStream + OpOffset));
					OpOffset += 5;
				}
				break;
			}

			//the next opcode
			OpSize = GetOpCodeSize32(((PBYTE)CallIStream + OpOffset));
			OpOffset += OpSize;
		}

		LOOP_FOREVER
		{
			if (((PBYTE)CallIStream + OpOffset)[0] == 0xC3)
				break;

			//mov eax, mem.offset
			if (((PBYTE)CallIStream + OpOffset)[0] == 0xC7 &&
				((PBYTE)CallIStream + OpOffset)[1] == 0x00)
			{
				OpOffset += 2;
				m_IStreamAdapterVtable = *(PULONG_PTR)((PBYTE)CallIStream + OpOffset);
				OpOffset += 4;
				break;
			}

			//the next opcode
			OpSize = GetOpCodeSize32(((PBYTE)CallIStream + OpOffset));
			OpOffset += OpSize;
		}
	}


	//Find virtual table offset
	//IStreamAdapter

	if (m_HostAlloc &&m_IStreamAdapterVtable)
	{
		m_CallTVPCreateStreamCall = CallTVPCreateStreamCall;
		return TRUE;
	}

	m_HostAlloc            = NULL;
	m_IStreamAdapterVtable = NULL;
	return FALSE;
}


DWORD WINAPI InitWindowThread(LPVOID lpParam)
{
	CKrkrzInternalApp* o = (CKrkrzInternalApp*)lpParam;
	return o->m_Viewer.DoModal();
}


CKrkrzInternalApp* CKrkrzInternalApp::GetApp()
{
	return &theApp;
}


NTSTATUS NTAPI InitExporter(iTVPFunctionExporter *exporter)
{
	BOOL                  Result;
	ULONG64               Crc;
	NTSTATUS              Status;

	LOOP_ONCE
	{
		Result = TVPInitImportStub(exporter);
		if (!Result)
			break;
	}
	return Result ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

struct LANGANDCODEPAGE
{
	WORD wLanguage;
	WORD wCodePage;
} *lpTranslate;

std::wstring GetVersionString()
{
	WCHAR Path[MAX_PATH]     = { 0 };
	WCHAR VersionBuffer[200];
	DWORD dwHandle, InfoSize;

	::GetModuleFileName((HMODULE)CKrkrzInternalApp::GetApp()->m_Module, Path, sizeof(Path));
	InfoSize = GetFileVersionInfoSizeW(Path, &dwHandle);
	if (InfoSize == 0)
	{
		return L"Unknown";
	}

	auto InfoBuf = new WCHAR[InfoSize];
	GetFileVersionInfoW(Path, 0, InfoSize, InfoBuf);
	unsigned int cbTranslate = 0;

	VerQueryValueW(InfoBuf, TEXT("\\VarFileInfo\\Translation"), (LPVOID*)&lpTranslate, &cbTranslate);

	std::wstring Version;
	for (int i = 0; i < min((cbTranslate / sizeof(LANGANDCODEPAGE)), 1); i++)
	{
		WCHAR  SubBlock[200];
		wsprintfW(SubBlock,
			TEXT("\\StringFileInfo\\%04x%04x\\FileVersion"),
			lpTranslate[i].wLanguage,
			lpTranslate[i].wCodePage);
		void *lpBuffer = NULL;
		unsigned int dwBytes = 0;
		VerQueryValueW(InfoBuf,
			SubBlock,
			&lpBuffer,
			&dwBytes);

		Version = (PWSTR)lpBuffer;
	}

	delete InfoBuf;
	return Version;
}


HRESULT WINAPI HookV2Link(iTVPFunctionExporter *exporter)
{
	CKrkrzInternalApp*    Handle;
	BOOL                  Success;

	Handle = CKrkrzInternalApp::GetApp();
	InitExporter(exporter);
	Handle->m_TVPFunctionExporter = exporter;

	Success = Handle->GetTVPCreateStreamCall();

	if (Success)
	{
		Handle->m_Viewer.Init(Handle->m_HostAlloc, 
			Handle->m_CallTVPCreateStreamCall, 
			Handle->m_IStreamAdapterVtable,
			GetVersionString());
		Nt_CreateThread(InitWindowThread, Handle);
	}

	return Handle->m_V2LinkStub(exporter);
}

API_POINTER(LoadLibraryA) StubLoadLibraryA = NULL;
API_POINTER(LoadLibraryW) StubLoadLibraryW = NULL;

PVOID WINAPI HookLoadLibraryA(LPCSTR lpFileName)
{
	PVOID   Result;
	PWSTR   UnicodeName;
	ULONG   Length, OutLength;

	Length = (StrLengthA(lpFileName) + 1) * 2;
	UnicodeName = (PWSTR)AllocStack(Length);

	RtlZeroMemory(UnicodeName, Length);
	RtlMultiByteToUnicodeN(UnicodeName, Length, &OutLength, lpFileName, Length / 2 - 1);

	Result = StubLoadLibraryW(UnicodeName);

	//some smc will check self, just ingore.
	if (LookupImportTable(Result, "KERNEL32.dll", "FlushInstructionCache") != IMAGE_INVALID_VA)
		return Result;

	CKrkrzInternalApp::GetApp()->InitHook(UnicodeName, Result);
	return Result;
}


PVOID WINAPI HookLoadLibraryW(LPCWSTR lpFileName)
{
	PVOID   Result;

	Result = StubLoadLibraryW(lpFileName);

	if (LookupImportTable(Result, "KERNEL32.dll", "FlushInstructionCache") != IMAGE_INVALID_VA)
		return (HMODULE)Result;

	CKrkrzInternalApp::GetApp()->InitHook(lpFileName, Result);

	return Result;
}


API_POINTER(MultiByteToWideChar) StubMultiByteToWideChar = NULL;
INT NTAPI HookMultiByteToWideChar(
	UINT   CodePage,
	DWORD  dwFlags,
	LPCSTR lpMultiByteStr,
	int    cbMultiByte,
	LPWSTR lpWideCharStr,
	int    cchWideChar
	)
{
	switch (CodePage)
	{
	case CP_ACP:
	case CP_OEMCP:
	case CP_THREAD_ACP:
		CodePage = 932;
		break;

	default:
		break;
	}

	return
		StubMultiByteToWideChar(
		CodePage,
		dwFlags,
		lpMultiByteStr,
		cbMultiByte,
		lpWideCharStr,
		cchWideChar
		);
}


API_POINTER(CreateProcessInternalW) StubCreateProcessInternalW = NULL;

BOOL
WINAPI
HookCreateProcessInternalW(
HANDLE                  hToken,
LPCWSTR                 lpApplicationName,
LPWSTR                  lpCommandLine,
LPSECURITY_ATTRIBUTES   lpProcessAttributes,
LPSECURITY_ATTRIBUTES   lpThreadAttributes,
BOOL                    bInheritHandles,
ULONG                   dwCreationFlags,
LPVOID                  lpEnvironment,
LPCWSTR                 lpCurrentDirectory,
LPSTARTUPINFOW          lpStartupInfo,
LPPROCESS_INFORMATION   lpProcessInformation,
PHANDLE                 phNewToken
)
{
	BOOL             Result, IsSuspended;
	NTSTATUS         Status;
	UNICODE_STRING   FullDllPath;

	RtlInitUnicodeString(&FullDllPath, CKrkrzInternalApp::GetApp()->m_Path);

	IsSuspended = !!(dwCreationFlags & CREATE_SUSPENDED);
	dwCreationFlags |= CREATE_SUSPENDED;
	Result = StubCreateProcessInternalW(
		hToken,
		lpApplicationName,
		lpCommandLine,
		lpProcessAttributes,
		lpThreadAttributes,
		bInheritHandles,
		dwCreationFlags,
		lpEnvironment,
		lpCurrentDirectory,
		lpStartupInfo,
		lpProcessInformation,
		phNewToken);

	if (!Result)
		return Result;

	Status = InjectDllToRemoteProcess(
		lpProcessInformation->hProcess,
		lpProcessInformation->hThread,
		&FullDllPath,
		IsSuspended
		);

	return TRUE;
}


BOOL CheckIsKrkrZ()
{
	NTSTATUS   Status;
	BOOL       Success;
	NtFileDisk File;
	ULONG_PTR  FileSize;
	PBYTE      FileBuffer;
	WCHAR      FileName[MAX_PATH];

	static WCHAR Pattern[] = L"TVP(KIRIKIRI) Z core / Scripting Platform for Win32";

	RtlZeroMemory(FileName, countof(FileName) * sizeof(FileName[0]));
	Nt_GetModuleFileName(Nt_GetExeModuleHandle(), FileName, MAX_PATH * 2);

	Success = FALSE;

	LOOP_ONCE
	{
		Status = File.Open(FileName);
		if (NT_FAILED(Status))
			break;

		Status = STATUS_NO_MEMORY;
		FileSize = File.GetSize32();
		FileBuffer = (PBYTE)AllocateMemoryP(FileSize);

		if (!FileBuffer)
			break;

		File.Read(FileBuffer, FileSize);
		if (KMP((PCChar)FileBuffer, FileSize, (PCHAR)Pattern, StrLengthW(Pattern) * 2))
		{
			Mp::PATCH_MEMORY_DATA f[] =
			{
				Mp::FunctionJumpVa(MultiByteToWideChar, HookMultiByteToWideChar, &StubMultiByteToWideChar)
			};

			Mp::PatchMemory(f, countof(f));
			Success = TRUE;

		}
		FreeMemoryP(FileBuffer);
	}
	File.Close();
	return Success;
}



NTSTATUS WINAPI InitModuleTemper()
{
	NtFileDisk            File;
	BOOL                  Success;
	NTSTATUS              Status;
	PVOID                 Target;
	PVOID                 ExporterPointer;
	ULONG                 Size;

	Mp::PATCH_MEMORY_DATA f[] =
	{
		Mp::FunctionJumpVa(LoadLibraryA, HookLoadLibraryA, &StubLoadLibraryA),
		Mp::FunctionJumpVa(LoadLibraryW, HookLoadLibraryW, &StubLoadLibraryW)
	};

	Status = Mp::PatchMemory(f, countof(f));

	*(PVOID*)&Target = Nt_GetProcAddress(Nt_LoadLibrary(L"KERNEL32.dll"), "CreateProcessInternalW");

	Mp::PATCH_MEMORY_DATA ooxx[] =
	{
		Mp::FunctionJumpVa(Target, HookCreateProcessInternalW, &StubCreateProcessInternalW)
	};

	Status = Mp::PatchMemory(ooxx, countof(ooxx));
	return Status;
}


NTSTATUS CKrkrzInternalApp::InitHook(LPCWSTR ModuleName, PVOID ImageBase)
{
	NTSTATUS    Status;
	ULONG_PTR   Length;
	ULONG64     Extension;
	DWORD       ThreadId;
	PVOID       pV2Link;

	SectionProtector<PRTL_CRITICAL_SECTION> Protector(&m_LoadCS);
	LOOP_ONCE
	{
		Status = STATUS_ALREADY_REGISTERED;
		if (m_Inited == TRUE)
			break;

		Status = STATUS_UNSUCCESSFUL;
		if (ImageBase == NULL)
			break;

		Length = StrLengthW(ModuleName);
		if (Length <= 4)
			break;

		Extension = *(PULONG64)&ModuleName[Length - 4];

		if (Extension != TAG4W('.dll') && Extension != TAG4W('.tpm'))
			break;

		if (Nt_GetProcAddress(ImageBase, "FlushInstructionCache"))
			break;

		pV2Link = Nt_GetProcAddress(ImageBase, "V2Link");
		if (pV2Link == NULL)
			break;

		Mp::PATCH_MEMORY_DATA f[] =
		{
			Mp::FunctionJumpVa(pV2Link, HookV2Link, &m_V2LinkStub)
		};

		Status = Mp::PatchMemory(f, countof(f));

		m_Inited = TRUE;
	}
	return Status;
}



BOOL CKrkrzInternalApp::InitKrkrExtract(HMODULE hModule)
{
	BOOL            Success;

	LOOP_ONCE
	{
		Success = CheckIsKrkrZ();
		if (!Success)
		{
			MessageBoxW(NULL, L"Sorry, KrkrzExtract only support krkrz engine!", L"FATAL", MB_OK | MB_ICONERROR);
			break;
		}

		Success = NT_FAILED(InitModuleTemper()) ? FALSE : TRUE;
	};

	
	return Success;
}

