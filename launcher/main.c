/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2020-2021 Eduard Permyakov 
 *
 *  Permafrost Engine is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Permafrost Engine is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 *  Linking this software statically or dynamically with other modules is making 
 *  a combined work based on this software. Thus, the terms and conditions of 
 *  the GNU General Public License cover the whole combination. 
 *  
 *  As a special exception, the copyright holders of Permafrost Engine give 
 *  you permission to link Permafrost Engine with independent modules to produce 
 *  an executable, regardless of the license terms of these independent 
 *  modules, and to copy and distribute the resulting executable under 
 *  terms of your choice, provided that you also meet, for each linked 
 *  independent module, the terms and conditions of the license of that 
 *  module. An independent module is a module which is not derived from 
 *  or based on Permafrost Engine. If you modify Permafrost Engine, you may 
 *  extend this exception to your version of Permafrost Engine, but you are not 
 *  obliged to do so. If you do not wish to do so, delete this exception 
 *  statement from your version.
 *
 */

#include <stdlib.h>

#ifndef BIN_PATH
#error "BIN_PATH must be defined"
#endif

#ifndef SCRIPT_PATH
#error "SCRIPT_PATH must be defined"
#endif

#define STR2(s) #s
#define STR(s) STR2(s)

#if defined(_WIN32)

#include <windows.h>
#include <Dbghelp.h>
#include <debugapi.h>
#include <strsafe.h>

#define BUFFSIZE    (2048)
#define CONTACT_STR "Contact: edward.permyakov@gmail.com"

static void launcher_dump_stream(HANDLE hStream, LPSTR filepath)
{
    DWORD dwRead, dwWritten; 
    CHAR buff[BUFFSIZE];
    BOOL success = FALSE;

    HANDLE hFile = CreateFile(filepath,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    if(!hFile)
        return;
 
    while(TRUE) { 
        success = ReadFile(hStream, buff, BUFFSIZE, &dwRead, NULL);
        if(!success || dwRead == 0) 
            break; 
        
        success = WriteFile(hFile, buff, dwRead, &dwWritten, NULL);
        if(!success) 
            break; 
    } 
    CloseHandle(hFile);
}

static BOOL launcher_get_thread_ctx(DWORD dwThreadId, LPCONTEXT out_ctx)
{
    ZeroMemory(out_ctx, sizeof(CONTEXT));
    out_ctx->ContextFlags = CONTEXT_FULL;

    HANDLE exc_thread = OpenThread(THREAD_GET_CONTEXT, FALSE, dwThreadId);
    if(!exc_thread)
        return FALSE;
    if(!GetThreadContext(exc_thread, out_ctx))
        return FALSE;
    CloseHandle(exc_thread);
    return TRUE;
}

static void launcher_date_string(LPSTR buff, SIZE_T bufflen)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    StringCchPrintf(buff, bufflen, "%04d.%02d.%02d-%02d.%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
}

static DWORD launcher_exe_directory(LPSTR buff, SIZE_T bufflen)
{
    DWORD nwritten = GetModuleFileNameA(NULL, buff, bufflen);
    LPSTR curr = buff + nwritten;
    while(curr > buff && *curr != '\\') {
        curr--;
    }
    *curr = '\0';
    return (curr - buff);
}

static void launcher_stdout_filepath(LPSTR out, SIZE_T bufflen)
{
    CHAR dir[MAX_PATH] = {0};
    CHAR date[MAX_PATH] = {0};

    launcher_exe_directory(dir, MAX_PATH);
    launcher_date_string(date, MAX_PATH);
    StringCchPrintf(out, bufflen, "%s\\%s-stdout.txt", dir, date);
}

static void launcher_stderr_filepath(LPSTR out, SIZE_T bufflen)
{
    CHAR dir[MAX_PATH] = {0};
    CHAR date[MAX_PATH] = {0};

    launcher_exe_directory(dir, MAX_PATH);
    launcher_date_string(date, MAX_PATH);
    StringCchPrintf(out, bufflen, "%s\\%s-stderr.txt", dir, date);
}

static void launcher_minidump_filepath(LPSTR out, SIZE_T bufflen)
{
    CHAR dir[MAX_PATH] = {0};
    CHAR date[MAX_PATH] = {0};

    launcher_exe_directory(dir, MAX_PATH);
    launcher_date_string(date, MAX_PATH);
    StringCchPrintf(out, bufflen, "%s\\%s-minidump.dmp", dir, date);
}

static BOOL launcher_write_minidump(PEXCEPTION_POINTERS exc_info, DWORD exc_pid, DWORD exc_tid, LPSTR filepath)
{
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, exc_pid);
    if(!hProcess)
        return FALSE;
    HANDLE hFile = CreateFile(filepath,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    if(!hFile) {
        CloseHandle(hProcess);
        return FALSE;
    }

    MINIDUMP_EXCEPTION_INFORMATION info;
    info.ThreadId = exc_tid;
    info.ExceptionPointers = exc_info;
    info.ClientPointers = FALSE;

    BOOL ret = MiniDumpWriteDump(hProcess, exc_pid, hFile, MiniDumpWithIndirectlyReferencedMemory, &info, NULL, NULL);

    CloseHandle(hFile);
    CloseHandle(hProcess);
    return ret;
}

static void launcher_error_message_box(BOOL md_written, LPSTR minidump_path, LPSTR stdout_path, LPSTR stderr_path)
{
    CHAR message[BUFFSIZE];
    StringCchPrintf(message, sizeof(message),"Permafrost Engine has encountered an error. "
        "The following diagnostic files have been written:\n\n");
    if(md_written) {
        StringCchCat(message, sizeof(message), minidump_path);
        StringCchCat(message, sizeof(message), "\n");
    }
    StringCchCat(message, sizeof(message), stdout_path);
    StringCchCat(message, sizeof(message), "\n");
    StringCchCat(message, sizeof(message), stderr_path);
    StringCchCat(message, sizeof(message), "\n\n");

    StringCchCat(message, sizeof(message), "Please report the error (along with the diagnostic files) "
        "to the developers so that the issue can be resolved. Thank you.\n\n");
    StringCchCat(message, sizeof(message), CONTACT_STR);

    MessageBox(NULL, message, "Permafrost Engine Error", MB_OK | MB_ICONEXCLAMATION);
}

int CALLBACK WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, 
                     LPSTR lpCmdLine, int nCmdShow)
{
    SECURITY_ATTRIBUTES sattr = {0}; 
    sattr.nLength = sizeof(SECURITY_ATTRIBUTES); 
    sattr.bInheritHandle = TRUE; 
    sattr.lpSecurityDescriptor = NULL; 

    HANDLE hChildStdOutRd = NULL;
    HANDLE hChildStdOutWr = NULL;

    if(!CreatePipe(&hChildStdOutRd, &hChildStdOutWr, &sattr, 0))
        return GetLastError();
    if(!SetHandleInformation(hChildStdOutRd, HANDLE_FLAG_INHERIT, 0))
        return GetLastError();
    CloseHandle(hChildStdOutRd);

    HANDLE hChildStdErrRd = NULL;
    HANDLE hChildStdErrWr = NULL;

    if(!CreatePipe(&hChildStdErrRd, &hChildStdErrWr, &sattr, 0))
        return GetLastError();
    if(!SetHandleInformation(hChildStdErrRd, HANDLE_FLAG_INHERIT, 0))
        return GetLastError();
    CloseHandle(hChildStdErrRd);

    PROCESS_INFORMATION pi = {0};
    STARTUPINFO si = {0};
    si.cb = sizeof(STARTUPINFO);
    si.hStdError = hChildStdOutWr;
    si.hStdOutput = hChildStdOutWr;
    si.dwFlags |= STARTF_USESTDHANDLES;

    LPTSTR cmdline = STR(BIN_PATH) " .\\ " STR(SCRIPT_PATH);
    if(!CreateProcess(NULL, cmdline, NULL, NULL, TRUE, CREATE_NO_WINDOW | DEBUG_ONLY_THIS_PROCESS , NULL, NULL, &si, &pi))
        return GetLastError();

    CHAR minidump_path[MAX_PATH] = {0};
    launcher_minidump_filepath(minidump_path, MAX_PATH);
    BOOL md_written = FALSE;

    DEBUG_EVENT dbg_event;
    EXCEPTION_RECORD exc_rec;
    EXCEPTION_POINTERS exc_info;
    CONTEXT exc_ctx;

    do{
        DWORD status = DBG_EXCEPTION_NOT_HANDLED;

        WaitForDebugEvent(&dbg_event, INFINITE);
        switch(dbg_event.dwDebugEventCode) {
        case EXCEPTION_DEBUG_EVENT:

            if(!dbg_event.u.Exception.dwFirstChance) {
                ContinueDebugEvent(dbg_event.dwProcessId, dbg_event.dwThreadId, status);
                continue;
            }

            if(!launcher_get_thread_ctx(dbg_event.dwThreadId, &exc_ctx)) {
                ContinueDebugEvent(dbg_event.dwProcessId, dbg_event.dwThreadId, status);
                continue;
            }

            exc_rec = dbg_event.u.Exception.ExceptionRecord;
            exc_info.ExceptionRecord = &dbg_event.u.Exception.ExceptionRecord;
            exc_info.ContextRecord = &exc_ctx;

            switch(exc_rec.ExceptionCode) {
            case EXCEPTION_BREAKPOINT:
                /* Windows sends a single breakpoint on load which must be handled */
                status = DBG_CONTINUE;
                break;
            default:
                md_written = launcher_write_minidump(&exc_info, dbg_event.dwProcessId, dbg_event.dwThreadId, minidump_path);
                break;
            }
            break;
        }

        ContinueDebugEvent(dbg_event.dwProcessId, dbg_event.dwThreadId, status);
    }while(dbg_event.dwDebugEventCode != EXIT_PROCESS_DEBUG_EVENT);

    CHAR stdout_path[MAX_PATH] = {0};
    launcher_stdout_filepath(stdout_path, MAX_PATH);

    CHAR stderr_path[MAX_PATH] = {0};
    launcher_stderr_filepath(stderr_path, MAX_PATH);

    DWORD dwExitCode = dbg_event.u.ExitProcess.dwExitCode;
    if(dwExitCode != 0) {

        launcher_dump_stream(hChildStdOutRd, stdout_path);
        launcher_dump_stream(hChildStdOutRd, stderr_path);
        launcher_error_message_box(md_written, minidump_path, stdout_path, stderr_path);
    }

    CloseHandle(hChildStdOutWr);
    CloseHandle(hChildStdErrWr);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return 0;
}
#else

#define LIB_PATH "./lib"

int main(int argc, char **argv)
{
    putenv("LD_LIBRARY_PATH=" STR(LIB_PATH));
    return system(STR(BIN_PATH) " ./ " STR(SCRIPT_PATH));
}
#endif

