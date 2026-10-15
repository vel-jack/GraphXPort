#include "GraphXPortGitProcess.h"

#include "HAL/PlatformProcess.h"

bool FGraphXPortGitProcess::Run(const FString& Args, const FString& WorkingDirectory, TArray<uint8>& OutStdOut, int32& OutReturnCode, const TArray<uint8>* InStdIn)
{
    OutStdOut.Reset();
    OutReturnCode = -1;

    void* ReadPipe = nullptr;
    void* WritePipe = nullptr;
    if (!FPlatformProcess::CreatePipe(ReadPipe, WritePipe))
    {
        return false;
    }

    void* InputReadPipe = nullptr;
    void* InputWritePipe = nullptr;
    if (InStdIn && !FPlatformProcess::CreatePipe(InputReadPipe, InputWritePipe, /*bWritePipeLocal=*/true))
    {
        FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
        return false;
    }

    FProcHandle Proc = FPlatformProcess::CreateProc(
        TEXT("git"), *Args,
        /*bLaunchDetached=*/true,
        /*bLaunchHidden=*/true,
        /*bLaunchReallyHidden=*/true,
        /*OutProcessID=*/nullptr,
        /*PriorityModifier=*/0,
        WorkingDirectory.IsEmpty() ? nullptr : *WorkingDirectory,
        WritePipe,
        InputReadPipe);

    if (!Proc.IsValid())
    {
        FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
        if (InStdIn) FPlatformProcess::ClosePipe(InputReadPipe, InputWritePipe);
        return false;
    }

    bool bInputWritten = true;
    if (InStdIn)
    {
        int32 Written = 0;
        bInputWritten = FPlatformProcess::WritePipe(InputWritePipe, InStdIn->GetData(), InStdIn->Num(), &Written)
            && Written == InStdIn->Num();
        FPlatformProcess::ClosePipe(InputReadPipe, InputWritePipe);
    }

    // Drain while the process runs: a .uasset easily exceeds the pipe buffer, and a full
    // pipe would block git forever.
    TArray<uint8> Chunk;
    while (FPlatformProcess::IsProcRunning(Proc))
    {
        if (FPlatformProcess::ReadPipeToArray(ReadPipe, Chunk))
        {
            OutStdOut.Append(Chunk);
        }
        else
        {
            FPlatformProcess::Sleep(0.005f);
        }
    }

    while (FPlatformProcess::ReadPipeToArray(ReadPipe, Chunk))
    {
        OutStdOut.Append(Chunk);
    }

    FPlatformProcess::GetProcReturnCode(Proc, &OutReturnCode);
    FPlatformProcess::CloseProc(Proc);
    FPlatformProcess::ClosePipe(ReadPipe, WritePipe);

    return bInputWritten;
}

bool FGraphXPortGitProcess::RunText(const FString& Args, const FString& WorkingDirectory, FString& OutStdOut, int32& OutReturnCode)
{
    OutStdOut.Empty();

    TArray<uint8> Bytes;
    if (!Run(Args, WorkingDirectory, Bytes, OutReturnCode))
    {
        return false;
    }

    if (Bytes.Num() > 0)
    {
        FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
        OutStdOut = FString(Converted.Length(), Converted.Get());
    }

    return true;
}
