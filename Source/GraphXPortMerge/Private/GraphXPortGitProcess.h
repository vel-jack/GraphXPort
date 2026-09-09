#pragma once

#include "CoreMinimal.h"

/**
 * Runs git directly (never through cmd.exe) with stdout on a pipe.
 *
 * The process is launched detached so Windows gives it no console at all - launching
 * merely "hidden" is not enough on Windows 11, where Windows Terminal hosts the console
 * and ignores SW_HIDE, popping a visible window for every git call.
 *
 * Output is collected as raw bytes so binary .uasset payloads survive intact; text
 * callers convert via RunText.
 */
class FGraphXPortGitProcess
{
public:
    static bool Run(const FString& Args, const FString& WorkingDirectory, TArray<uint8>& OutStdOut, int32& OutReturnCode);
    static bool RunText(const FString& Args, const FString& WorkingDirectory, FString& OutStdOut, int32& OutReturnCode);
};
