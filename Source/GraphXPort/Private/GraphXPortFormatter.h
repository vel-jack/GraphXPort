#pragma once

#include "CoreMinimal.h"
#include "GraphXPortDocument.h"

enum class EGraphXPortFormat : uint8
{
    Json,
    AiText,
    Ascii,
    Mermaid
};

class FGraphXPortFormatter
{
public:
    static FString Format(const FGraphXPortDocument& Document, EGraphXPortFormat Format);
    static FString Extension(EGraphXPortFormat Format);
    static FString FileFilter(EGraphXPortFormat Format);

private:
    static FString ToJson(const FGraphXPortDocument& Document);
    static FString ToAiText(const FGraphXPortDocument& Document);
    static FString ToAscii(const FGraphXPortDocument& Document);
    static FString ToMermaid(const FGraphXPortDocument& Document);
};
