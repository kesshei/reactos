/*
 * reactos/win32ss/user/consrv/conio.c
 *
 * Console I/O functions
 *
 * ReactOS Operating System
 */

/* INCLUDES ******************************************************************/

#include "consrv.h"
#include "guiconsole.h"
#include "tuiconsole.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS *****************************************************************/

/*** Taken from win32ss/user/win32csr/desktopbg.c ***/
BOOL FASTCALL
DtbgIsDesktopVisible(VOID)
{
    HWND VisibleDesktopWindow = GetDesktopWindow(); // DESKTOPWNDPROC

    if (VisibleDesktopWindow != NULL &&
            !IsWindowVisible(VisibleDesktopWindow))
    {
        VisibleDesktopWindow = NULL;
    }

    return VisibleDesktopWindow != NULL;
}
/****************************************************/

NTSTATUS FASTCALL
ConioConsoleFromProcessData(PCSR_PROCESS ProcessData, PCSRSS_CONSOLE *Console)
{
    PCSRSS_CONSOLE ProcessConsole;

    RtlEnterCriticalSection(&ProcessData->HandleTableLock);
    ProcessConsole = ProcessData->Console;

    if (!ProcessConsole)
    {
        *Console = NULL;
        RtlLeaveCriticalSection(&ProcessData->HandleTableLock);
        return STATUS_INVALID_HANDLE;
    }

    InterlockedIncrement(&ProcessConsole->ReferenceCount);
    RtlLeaveCriticalSection(&ProcessData->HandleTableLock);
    EnterCriticalSection(&(ProcessConsole->Lock));
    *Console = ProcessConsole;

    return STATUS_SUCCESS;
}

VOID FASTCALL
ConioConsoleCtrlEventTimeout(DWORD Event, PCSR_PROCESS ProcessData, DWORD Timeout)
{
    HANDLE Thread;

    DPRINT("ConioConsoleCtrlEvent Parent ProcessId = %x\n", ProcessData->ClientId.UniqueProcess);

    if (ProcessData->CtrlDispatcher)
    {

        Thread = CreateRemoteThread(ProcessData->ProcessHandle, NULL, 0,
                                    (LPTHREAD_START_ROUTINE) ProcessData->CtrlDispatcher,
                                    UlongToPtr(Event), 0, NULL);
        if (NULL == Thread)
        {
            DPRINT1("Failed thread creation (Error: 0x%x)\n", GetLastError());
            return;
        }
        WaitForSingleObject(Thread, Timeout);
        CloseHandle(Thread);
    }
}

VOID FASTCALL
ConioConsoleCtrlEvent(DWORD Event, PCSR_PROCESS ProcessData)
{
    ConioConsoleCtrlEventTimeout(Event, ProcessData, 0);
}

static NTSTATUS WINAPI
CsrInitConsole(PCSRSS_CONSOLE Console, int ShowCmd)
{
    NTSTATUS Status;
    SECURITY_ATTRIBUTES SecurityAttributes;
    PCSRSS_SCREEN_BUFFER NewBuffer;
    BOOL GuiMode;
    WCHAR Title[255];
    HINSTANCE hInst;

    Console->Title.MaximumLength = Console->Title.Length = 0;
    Console->Title.Buffer = NULL;

    hInst = GetModuleHandleW(L"win32csr");
    if (LoadStringW(hInst,IDS_COMMAND_PROMPT,Title,sizeof(Title)/sizeof(Title[0])))
    {
        RtlCreateUnicodeString(&Console->Title, Title);
    }
    else
    {
        RtlCreateUnicodeString(&Console->Title, L"Command Prompt");
    }

    Console->ReferenceCount = 0;
    Console->LineBuffer = NULL;
    Console->Header.Type = CONIO_CONSOLE_MAGIC;
    Console->Header.Console = Console;
    Console->Mode = ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_MOUSE_INPUT;
    InitializeListHead(&Console->BufferList);
    Console->ActiveBuffer = NULL;
    InitializeListHead(&Console->InputEvents);
    InitializeListHead(&Console->HistoryBuffers);
    Console->CodePage = GetOEMCP();
    Console->OutputCodePage = GetOEMCP();

    SecurityAttributes.nLength = sizeof(SECURITY_ATTRIBUTES);
    SecurityAttributes.lpSecurityDescriptor = NULL;
    SecurityAttributes.bInheritHandle = TRUE;

    Console->ActiveEvent = CreateEventW(&SecurityAttributes, TRUE, FALSE, NULL);
    if (NULL == Console->ActiveEvent)
    {
        RtlFreeUnicodeString(&Console->Title);
        return STATUS_UNSUCCESSFUL;
    }
    Console->PrivateData = NULL;
    InitializeCriticalSection(&Console->Lock);

    GuiMode = DtbgIsDesktopVisible();

    /* allocate console screen buffer */
    NewBuffer = HeapAlloc(ConSrvHeap, HEAP_ZERO_MEMORY, sizeof(CSRSS_SCREEN_BUFFER));
    if (NULL == NewBuffer)
    {
        RtlFreeUnicodeString(&Console->Title);
        DeleteCriticalSection(&Console->Lock);
        CloseHandle(Console->ActiveEvent);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    /* init screen buffer with defaults */
    NewBuffer->CursorInfo.bVisible = TRUE;
    NewBuffer->CursorInfo.dwSize = CSR_DEFAULT_CURSOR_SIZE;
    /* make console active, and insert into console list */
    Console->ActiveBuffer = (PCSRSS_SCREEN_BUFFER) NewBuffer;

    if (! GuiMode)
    {
        Status = TuiInitConsole(Console);
        if (! NT_SUCCESS(Status))
        {
            DPRINT1("Failed to open text-mode console, switching to gui-mode\n");
            GuiMode = TRUE;
        }
    }
    else /* GuiMode */
    {
        Status = GuiInitConsole(Console, ShowCmd);
        if (! NT_SUCCESS(Status))
        {
            HeapFree(ConSrvHeap,0, NewBuffer);
            RtlFreeUnicodeString(&Console->Title);
            DeleteCriticalSection(&Console->Lock);
            CloseHandle(Console->ActiveEvent);
            DPRINT1("GuiInitConsole: failed\n");
            return Status;
        }
    }

    Status = CsrInitConsoleScreenBuffer(Console, NewBuffer);
    if (! NT_SUCCESS(Status))
    {
        ConioCleanupConsole(Console);
        RtlFreeUnicodeString(&Console->Title);
        DeleteCriticalSection(&Console->Lock);
        CloseHandle(Console->ActiveEvent);
        HeapFree(ConSrvHeap, 0, NewBuffer);
        DPRINT1("CsrInitConsoleScreenBuffer: failed\n");
        return Status;
    }

    /* copy buffer contents to screen */
    ConioDrawConsole(Console);

    return STATUS_SUCCESS;
}

CSR_API(SrvAllocConsole)
{
    PCSRSS_ALLOC_CONSOLE AllocConsoleRequest = &((PCONSOLE_API_MESSAGE)ApiMessage)->Data.AllocConsoleRequest;
    PCSR_PROCESS ProcessData = CsrGetClientThread()->Process;
    PCSRSS_CONSOLE Console;
    NTSTATUS Status = STATUS_SUCCESS;
    BOOLEAN NewConsole = FALSE;

    DPRINT("SrvAllocConsole\n");

    RtlEnterCriticalSection(&ProcessData->HandleTableLock);
    if (ProcessData->Console)
    {
        DPRINT1("Process already has a console\n");
        RtlLeaveCriticalSection(&ProcessData->HandleTableLock);
        return STATUS_INVALID_PARAMETER;
    }

    /* If we don't need a console, then get out of here */
    if (!AllocConsoleRequest->ConsoleNeeded)
    {
        DPRINT("No console needed\n");
        RtlLeaveCriticalSection(&ProcessData->HandleTableLock);
        return STATUS_SUCCESS;
    }

    /* If we already have one, then don't create a new one... */
    if (!AllocConsoleRequest->Console ||
            AllocConsoleRequest->Console != ProcessData->ParentConsole)
    {
        /* Allocate a console structure */
        NewConsole = TRUE;
        Console = HeapAlloc(ConSrvHeap, HEAP_ZERO_MEMORY, sizeof(CSRSS_CONSOLE));
        if (NULL == Console)
        {
            DPRINT1("Not enough memory for console\n");
            RtlLeaveCriticalSection(&ProcessData->HandleTableLock);
            return STATUS_NO_MEMORY;
        }
        /* initialize list head */
        InitializeListHead(&Console->ProcessList);
        /* insert process data required for GUI initialization */
        InsertHeadList(&Console->ProcessList, &ProcessData->ConsoleLink);
        /* Initialize the Console */
        Status = CsrInitConsole(Console, AllocConsoleRequest->ShowCmd);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Console init failed\n");
            HeapFree(ConSrvHeap, 0, Console);
            RtlLeaveCriticalSection(&ProcessData->HandleTableLock);
            return Status;
        }
    }
    else
    {
        /* Reuse our current console */
        Console = AllocConsoleRequest->Console;
    }

    /* Set the Process Console */
    ProcessData->Console = Console;

    /* Return it to the caller */
    AllocConsoleRequest->Console = Console;

    /* Add a reference count because the process is tied to the console */
    _InterlockedIncrement(&Console->ReferenceCount);

    if (NewConsole || !ProcessData->bInheritHandles)
    {
        /* Insert the Objects */
        Status = Win32CsrInsertObject(ProcessData,
                                      &AllocConsoleRequest->InputHandle,
                                      &Console->Header,
                                      GENERIC_READ | GENERIC_WRITE,
                                      TRUE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE);
        if (! NT_SUCCESS(Status))
        {
            DPRINT1("Failed to insert object\n");
            ConioDeleteConsole((Object_t *) Console);
            ProcessData->Console = 0;
            RtlLeaveCriticalSection(&ProcessData->HandleTableLock);
            return Status;
        }

        Status = Win32CsrInsertObject(ProcessData,
                                      &AllocConsoleRequest->OutputHandle,
                                      &Console->ActiveBuffer->Header,
                                      GENERIC_READ | GENERIC_WRITE,
                                      TRUE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Failed to insert object\n");
            ConioDeleteConsole((Object_t *) Console);
            Win32CsrReleaseObject(ProcessData,
                                  AllocConsoleRequest->InputHandle);
            ProcessData->Console = 0;
            RtlLeaveCriticalSection(&ProcessData->HandleTableLock);
            return Status;
        }
    }

    /* Duplicate the Event */
    if (!DuplicateHandle(GetCurrentProcess(),
                         ProcessData->Console->ActiveEvent,
                         ProcessData->ProcessHandle,
                         &ProcessData->ConsoleEvent,
                         EVENT_ALL_ACCESS,
                         FALSE,
                         0))
    {
        DPRINT1("DuplicateHandle() failed: %lu\n", GetLastError());
        ConioDeleteConsole((Object_t *) Console);
        if (NewConsole || !ProcessData->bInheritHandles)
        {
            Win32CsrReleaseObject(ProcessData,
                                  AllocConsoleRequest->OutputHandle);
            Win32CsrReleaseObject(ProcessData,
                                  AllocConsoleRequest->InputHandle);
        }
        ProcessData->Console = 0;
        RtlLeaveCriticalSection(&ProcessData->HandleTableLock);
        return Status;
    }

    /* Set the Ctrl Dispatcher */
    ProcessData->CtrlDispatcher = AllocConsoleRequest->CtrlDispatcher;
    DPRINT("CSRSS:CtrlDispatcher address: %x\n", ProcessData->CtrlDispatcher);

    if (!NewConsole)
    {
        /* Insert into the list if it has not been added */
        InsertHeadList(&ProcessData->Console->ProcessList, &ProcessData->ConsoleLink);
    }

    RtlLeaveCriticalSection(&ProcessData->HandleTableLock);
    return STATUS_SUCCESS;
}

CSR_API(SrvFreeConsole)
{
    Win32CsrReleaseConsole(CsrGetClientThread()->Process);
    return STATUS_SUCCESS;
}

VOID WINAPI
ConioDeleteConsole(Object_t *Object)
{
    PCSRSS_CONSOLE Console = (PCSRSS_CONSOLE) Object;
    ConsoleInput *Event;

    DPRINT("ConioDeleteConsole\n");

    /* Drain input event queue */
    while (Console->InputEvents.Flink != &Console->InputEvents)
    {
        Event = (ConsoleInput *) Console->InputEvents.Flink;
        Console->InputEvents.Flink = Console->InputEvents.Flink->Flink;
        Console->InputEvents.Flink->Flink->Blink = &Console->InputEvents;
        HeapFree(ConSrvHeap, 0, Event);
    }

    ConioCleanupConsole(Console);
    if (Console->LineBuffer)
        RtlFreeHeap(ConSrvHeap, 0, Console->LineBuffer);
    while (!IsListEmpty(&Console->HistoryBuffers))
        HistoryDeleteBuffer((struct tagHISTORY_BUFFER *)Console->HistoryBuffers.Flink);

    ConioDeleteScreenBuffer(Console->ActiveBuffer);
    if (!IsListEmpty(&Console->BufferList))
    {
        DPRINT1("BUG: screen buffer list not empty\n");
    }

    CloseHandle(Console->ActiveEvent);
    if (Console->UnpauseEvent) CloseHandle(Console->UnpauseEvent);
    DeleteCriticalSection(&Console->Lock);
    RtlFreeUnicodeString(&Console->Title);
    IntDeleteAllAliases(Console->Aliases);
    HeapFree(ConSrvHeap, 0, Console);
}

VOID WINAPI
CsrInitConsoleSupport(VOID)
{
    DPRINT("CSR: CsrInitConsoleSupport()\n");

    /* Should call LoadKeyboardLayout */
}

VOID FASTCALL
ConioPause(PCSRSS_CONSOLE Console, UINT Flags)
{
    Console->PauseFlags |= Flags;
    if (!Console->UnpauseEvent)
        Console->UnpauseEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
}

VOID FASTCALL
ConioUnpause(PCSRSS_CONSOLE Console, UINT Flags)
{
    Console->PauseFlags &= ~Flags;
    if (Console->PauseFlags == 0 && Console->UnpauseEvent)
    {
        SetEvent(Console->UnpauseEvent);
        CloseHandle(Console->UnpauseEvent);
        Console->UnpauseEvent = NULL;
    }
}

CSR_API(SrvSetConsoleMode)
{
    NTSTATUS Status;
    PCSRSS_SET_CONSOLE_MODE SetConsoleModeRequest = &((PCONSOLE_API_MESSAGE)ApiMessage)->Data.SetConsoleModeRequest;
    PCSRSS_CONSOLE Console;
    PCSRSS_SCREEN_BUFFER Buff;

    DPRINT("SrvSetConsoleMode\n");

    Status = Win32CsrLockObject(CsrGetClientThread()->Process,
                                SetConsoleModeRequest->ConsoleHandle,
                                (Object_t **) &Console, GENERIC_WRITE, 0);
    if (! NT_SUCCESS(Status))
    {
        return Status;
    }

    Buff = (PCSRSS_SCREEN_BUFFER)Console;
    if (CONIO_CONSOLE_MAGIC == Console->Header.Type)
    {
        Console->Mode = SetConsoleModeRequest->Mode & CONSOLE_INPUT_MODE_VALID;
    }
    else if (CONIO_SCREEN_BUFFER_MAGIC == Console->Header.Type)
    {
        Buff->Mode = SetConsoleModeRequest->Mode & CONSOLE_OUTPUT_MODE_VALID;
    }
    else
    {
        Status = STATUS_INVALID_HANDLE;
    }

    Win32CsrUnlockObject((Object_t *)Console);

    return Status;
}

CSR_API(SrvGetConsoleMode)
{
    NTSTATUS Status;
    PCSRSS_GET_CONSOLE_MODE GetConsoleModeRequest = &((PCONSOLE_API_MESSAGE)ApiMessage)->Data.GetConsoleModeRequest;
    PCSRSS_CONSOLE Console;
    PCSRSS_SCREEN_BUFFER Buff;

    DPRINT("SrvGetConsoleMode\n");

    Status = Win32CsrLockObject(CsrGetClientThread()->Process, GetConsoleModeRequest->ConsoleHandle,
                                (Object_t **) &Console, GENERIC_READ, 0);
    if (! NT_SUCCESS(Status))
    {
        return Status;
    }
    Status = STATUS_SUCCESS;
    Buff = (PCSRSS_SCREEN_BUFFER) Console;
    if (CONIO_CONSOLE_MAGIC == Console->Header.Type)
    {
        GetConsoleModeRequest->ConsoleMode = Console->Mode;
    }
    else if (CONIO_SCREEN_BUFFER_MAGIC == Buff->Header.Type)
    {
        GetConsoleModeRequest->ConsoleMode = Buff->Mode;
    }
    else
    {
        Status = STATUS_INVALID_HANDLE;
    }

    Win32CsrUnlockObject((Object_t *)Console);
    return Status;
}

CSR_API(SrvSetConsoleTitle)
{
    NTSTATUS Status;
    PCSRSS_SET_TITLE SetTitleRequest = &((PCONSOLE_API_MESSAGE)ApiMessage)->Data.SetTitleRequest;
    PCSR_PROCESS ProcessData = CsrGetClientThread()->Process;
    PCSRSS_CONSOLE Console;
    PWCHAR Buffer;

    DPRINT("SrvSetConsoleTitle\n");

    if (!Win32CsrValidateBuffer(ProcessData, SetTitleRequest->Title,
                                SetTitleRequest->Length, 1))
    {
        return STATUS_ACCESS_VIOLATION;
    }

    Status = ConioConsoleFromProcessData(ProcessData, &Console);
    if(NT_SUCCESS(Status))
    {
        Buffer =  RtlAllocateHeap(RtlGetProcessHeap(), 0, SetTitleRequest->Length);
        if (Buffer)
        {
            /* copy title to console */
            RtlFreeUnicodeString(&Console->Title);
            Console->Title.Buffer = Buffer;
            Console->Title.Length = Console->Title.MaximumLength = SetTitleRequest->Length;
            memcpy(Console->Title.Buffer, SetTitleRequest->Title, Console->Title.Length);
            if (! ConioChangeTitle(Console))
            {
                Status = STATUS_UNSUCCESSFUL;
            }
            else
            {
                Status = STATUS_SUCCESS;
            }
        }
        else
        {
            Status = STATUS_NO_MEMORY;
        }
        ConioUnlockConsole(Console);
    }

    return Status;
}

CSR_API(SrvGetConsoleTitle)
{
    NTSTATUS Status;
    PCSRSS_GET_TITLE GetTitleRequest = &((PCONSOLE_API_MESSAGE)ApiMessage)->Data.GetTitleRequest;
    PCSR_PROCESS ProcessData = CsrGetClientThread()->Process;
    PCSRSS_CONSOLE Console;
    DWORD Length;

    DPRINT("SrvGetConsoleTitle\n");

    if (!Win32CsrValidateBuffer(ProcessData, GetTitleRequest->Title,
                                GetTitleRequest->Length, 1))
    {
        return STATUS_ACCESS_VIOLATION;
    }

    Status = ConioConsoleFromProcessData(ProcessData, &Console);
    if (! NT_SUCCESS(Status))
    {
        DPRINT1("Can't get console\n");
        return Status;
    }

    /* Copy title of the console to the user title buffer */
    if (GetTitleRequest->Length >= sizeof(WCHAR))
    {
        Length = min(GetTitleRequest->Length - sizeof(WCHAR), Console->Title.Length);
        memcpy(GetTitleRequest->Title, Console->Title.Buffer, Length);
        GetTitleRequest->Title[Length / sizeof(WCHAR)] = L'\0';
    }

    GetTitleRequest->Length = Console->Title.Length;

    ConioUnlockConsole(Console);
    return STATUS_SUCCESS;
}

/**********************************************************************
 *	HardwareStateProperty
 *
 *	DESCRIPTION
 *		Set/Get the value of the HardwareState and switch
 *		between direct video buffer ouput and GDI windowed
 *		output.
 *	ARGUMENTS
 *		Client hands us a CSRSS_CONSOLE_HARDWARE_STATE
 *		object. We use the same object to Request.
 *	NOTE
 *		ConsoleHwState has the correct size to be compatible
 *		with NT's, but values are not.
 */
static NTSTATUS FASTCALL
SetConsoleHardwareState(PCSRSS_CONSOLE Console, DWORD ConsoleHwState)
{
    DPRINT1("Console Hardware State: %d\n", ConsoleHwState);

    if ((CONSOLE_HARDWARE_STATE_GDI_MANAGED == ConsoleHwState)
            ||(CONSOLE_HARDWARE_STATE_DIRECT == ConsoleHwState))
    {
        if (Console->HardwareState != ConsoleHwState)
        {
            /* TODO: implement switching from full screen to windowed mode */
            /* TODO: or back; now simply store the hardware state */
            Console->HardwareState = ConsoleHwState;
        }

        return STATUS_SUCCESS;
    }

    return STATUS_INVALID_PARAMETER_3; /* Client: (handle, set_get, [mode]) */
}

CSR_API(SrvGetConsoleHardwareState)
{
    PCSRSS_SETGET_CONSOLE_HW_STATE ConsoleHardwareStateRequest = &((PCONSOLE_API_MESSAGE)ApiMessage)->Data.ConsoleHardwareStateRequest;
    PCSRSS_CONSOLE Console;
    NTSTATUS Status;

    DPRINT("SrvGetConsoleHardwareState\n");

    Status = ConioLockConsole(CsrGetClientThread()->Process,
                              ConsoleHardwareStateRequest->ConsoleHandle,
                              &Console,
                              GENERIC_READ);
    if (! NT_SUCCESS(Status))
    {
        DPRINT1("Failed to get console handle in SetConsoleHardwareState\n");
        return Status;
    }

    switch (ConsoleHardwareStateRequest->SetGet)
    {
    case CONSOLE_HARDWARE_STATE_GET:
        ConsoleHardwareStateRequest->State = Console->HardwareState;
        break;

    case CONSOLE_HARDWARE_STATE_SET:
        DPRINT("Setting console hardware state.\n");
        Status = SetConsoleHardwareState(Console, ConsoleHardwareStateRequest->State);
        break;

    default:
        Status = STATUS_INVALID_PARAMETER_2; /* Client: (handle, [set_get], mode) */
        break;
    }

    ConioUnlockConsole(Console);

    return Status;
}

CSR_API(SrvSetConsoleHardwareState)
{
    PCSRSS_SETGET_CONSOLE_HW_STATE ConsoleHardwareStateRequest = &((PCONSOLE_API_MESSAGE)ApiMessage)->Data.ConsoleHardwareStateRequest;
    PCSRSS_CONSOLE Console;
    NTSTATUS Status;

    DPRINT("SrvSetConsoleHardwareState\n");

    Status = ConioLockConsole(CsrGetClientThread()->Process,
                              ConsoleHardwareStateRequest->ConsoleHandle,
                              &Console,
                              GENERIC_READ);
    if (! NT_SUCCESS(Status))
    {
        DPRINT1("Failed to get console handle in SetConsoleHardwareState\n");
        return Status;
    }

    switch (ConsoleHardwareStateRequest->SetGet)
    {
    case CONSOLE_HARDWARE_STATE_GET:
        ConsoleHardwareStateRequest->State = Console->HardwareState;
        break;

    case CONSOLE_HARDWARE_STATE_SET:
        DPRINT("Setting console hardware state.\n");
        Status = SetConsoleHardwareState(Console, ConsoleHardwareStateRequest->State);
        break;

    default:
        Status = STATUS_INVALID_PARAMETER_2; /* Client: (handle, [set_get], mode) */
        break;
    }

    ConioUnlockConsole(Console);

    return Status;
}

CSR_API(SrvGetConsoleWindow)
{
    PCSRSS_GET_CONSOLE_WINDOW GetConsoleWindowRequest = &((PCONSOLE_API_MESSAGE)ApiMessage)->Data.GetConsoleWindowRequest;
    PCSRSS_CONSOLE Console;
    NTSTATUS Status;

    DPRINT("SrvGetConsoleWindow\n");

    Status = ConioConsoleFromProcessData(CsrGetClientThread()->Process, &Console);
    if (! NT_SUCCESS(Status))
    {
        return Status;
    }

    GetConsoleWindowRequest->WindowHandle = Console->hWindow;
    ConioUnlockConsole(Console);

    return STATUS_SUCCESS;
}

CSR_API(SrvSetConsoleIcon)
{
    PCSRSS_SET_CONSOLE_ICON SetConsoleIconRequest = &((PCONSOLE_API_MESSAGE)ApiMessage)->Data.SetConsoleIconRequest;
    PCSRSS_CONSOLE Console;
    NTSTATUS Status;

    DPRINT("SrvSetConsoleIcon\n");

    Status = ConioConsoleFromProcessData(CsrGetClientThread()->Process, &Console);
    if (! NT_SUCCESS(Status))
    {
        return Status;
    }

    Status = (ConioChangeIcon(Console, SetConsoleIconRequest->WindowIcon)
              ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL);
    ConioUnlockConsole(Console);

    return Status;
}

CSR_API(SrvGetConsoleCP)
{
    PCSRSS_GET_CONSOLE_CP GetConsoleCodePage = &((PCONSOLE_API_MESSAGE)ApiMessage)->Data.GetConsoleCodePage;
    PCSRSS_CONSOLE Console;
    NTSTATUS Status;

    DPRINT("SrvGetConsoleCP\n");

    Status = ConioConsoleFromProcessData(CsrGetClientThread()->Process, &Console);
    if (! NT_SUCCESS(Status))
    {
        return Status;
    }

    GetConsoleCodePage->CodePage = Console->CodePage;
    ConioUnlockConsole(Console);
    return STATUS_SUCCESS;
}

CSR_API(CsrGetConsoleOutputCodePage) // TODO: Merge this function with the other one.
{
    PCSRSS_GET_CONSOLE_OUTPUT_CP GetConsoleOutputCodePage = &((PCONSOLE_API_MESSAGE)ApiMessage)->Data.GetConsoleOutputCodePage;
    PCSRSS_CONSOLE Console;
    NTSTATUS Status;

    DPRINT("CsrGetConsoleOutputCodePage\n");

    Status = ConioConsoleFromProcessData(CsrGetClientThread()->Process, &Console);
    if (! NT_SUCCESS(Status))
    {
        return Status;
    }

    GetConsoleOutputCodePage->CodePage = Console->OutputCodePage;
    ConioUnlockConsole(Console);
    return STATUS_SUCCESS;
}

CSR_API(SrvSetConsoleCP)
{
    PCSRSS_SET_CONSOLE_CP SetConsoleCodePage = &((PCONSOLE_API_MESSAGE)ApiMessage)->Data.SetConsoleCodePage;
    PCSRSS_CONSOLE Console;
    NTSTATUS Status;

    DPRINT("SrvSetConsoleCP\n");

    Status = ConioConsoleFromProcessData(CsrGetClientThread()->Process, &Console);
    if (! NT_SUCCESS(Status))
    {
        return Status;
    }

    if (IsValidCodePage(SetConsoleCodePage->CodePage))
    {
        Console->CodePage = SetConsoleCodePage->CodePage;
        ConioUnlockConsole(Console);
        return STATUS_SUCCESS;
    }

    ConioUnlockConsole(Console);
    return STATUS_INVALID_PARAMETER;
}

CSR_API(CsrSetConsoleOutputCodePage) // TODO: Merge this function with the other one.
{
    PCSRSS_SET_CONSOLE_OUTPUT_CP SetConsoleOutputCodePage = &((PCONSOLE_API_MESSAGE)ApiMessage)->Data.SetConsoleOutputCodePage;
    PCSRSS_CONSOLE Console;
    NTSTATUS Status;

    DPRINT("CsrSetConsoleOutputCodePage\n");

    Status = ConioConsoleFromProcessData(CsrGetClientThread()->Process, &Console);
    if (! NT_SUCCESS(Status))
    {
        return Status;
    }

    if (IsValidCodePage(SetConsoleOutputCodePage->CodePage))
    {
        Console->OutputCodePage = SetConsoleOutputCodePage->CodePage;
        ConioUnlockConsole(Console);
        return STATUS_SUCCESS;
    }

    ConioUnlockConsole(Console);
    return STATUS_INVALID_PARAMETER;
}

CSR_API(SrvGetConsoleProcessList)
{
    PDWORD Buffer;
    PCSR_PROCESS ProcessData = CsrGetClientThread()->Process;
    PCSRSS_CONSOLE Console;
    PCSR_PROCESS current;
    PLIST_ENTRY current_entry;
    ULONG nItems = 0;
    NTSTATUS Status;

    DPRINT("SrvGetConsoleProcessList\n");

    Buffer = ApiMessage->Data.GetProcessListRequest.ProcessId;
    if (!Win32CsrValidateBuffer(ProcessData, Buffer, ApiMessage->Data.GetProcessListRequest.nMaxIds, sizeof(DWORD)))
        return STATUS_ACCESS_VIOLATION;

    Status = ConioConsoleFromProcessData(ProcessData, &Console);
    if (! NT_SUCCESS(Status))
    {
        return Status;
    }

    for (current_entry = Console->ProcessList.Flink;
         current_entry != &Console->ProcessList;
         current_entry = current_entry->Flink)
    {
        current = CONTAINING_RECORD(current_entry, CSR_PROCESS, ConsoleLink);
        if (++nItems <= ApiMessage->Data.GetProcessListRequest.nMaxIds)
        {
            *Buffer++ = HandleToUlong(current->ClientId.UniqueProcess);
        }
    }

    ConioUnlockConsole(Console);

    ApiMessage->Data.GetProcessListRequest.nProcessIdsTotal = nItems;
    return STATUS_SUCCESS;
}

CSR_API(SrvGenerateConsoleCtrlEvent)
{
    PCSRSS_GENERATE_CTRL_EVENT GenerateCtrlEvent = &((PCONSOLE_API_MESSAGE)ApiMessage)->Data.GenerateCtrlEvent;
    PCSRSS_CONSOLE Console;
    PCSR_PROCESS current;
    PLIST_ENTRY current_entry;
    DWORD Group;
    NTSTATUS Status;

    Status = ConioConsoleFromProcessData(CsrGetClientThread()->Process, &Console);
    if (! NT_SUCCESS(Status))
    {
        return Status;
    }

    Group = GenerateCtrlEvent->ProcessGroup;
    Status = STATUS_INVALID_PARAMETER;
    for (current_entry = Console->ProcessList.Flink;
            current_entry != &Console->ProcessList;
            current_entry = current_entry->Flink)
    {
        current = CONTAINING_RECORD(current_entry, CSR_PROCESS, ConsoleLink);
        if (Group == 0 || current->ProcessGroupId == Group)
        {
            ConioConsoleCtrlEvent(GenerateCtrlEvent->Event, current);
            Status = STATUS_SUCCESS;
        }
    }

    ConioUnlockConsole(Console);

    return Status;
}

CSR_API(SrvGetConsoleSelectionInfo)
{
    NTSTATUS Status;
    PCSRSS_GET_CONSOLE_SELECTION_INFO GetConsoleSelectionInfo = &((PCONSOLE_API_MESSAGE)ApiMessage)->Data.GetConsoleSelectionInfo;
    PCSRSS_CONSOLE Console;

    Status = ConioConsoleFromProcessData(CsrGetClientThread()->Process, &Console);
    if (NT_SUCCESS(Status))
    {
        memset(&GetConsoleSelectionInfo->Info, 0, sizeof(CONSOLE_SELECTION_INFO));
        if (Console->Selection.dwFlags != 0)
            GetConsoleSelectionInfo->Info = Console->Selection;
        ConioUnlockConsole(Console);
    }
    return Status;
}

/* EOF */
