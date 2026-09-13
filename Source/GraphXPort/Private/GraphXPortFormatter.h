#pragma once

#include "CoreMinimal.h"
#include "GraphXPortDocument.h"

enum class EGraphXPortFormat : uint8
{
    AiText,
    Ascii,
    Mermaid
};

class FGraphXPortFormatter
{
public:
    static FString Format(const FGraphXPortDocument& Document, EGraphXPortFormat Format);

private:
    static FString ToAiText(const FGraphXPortDocument& Document);
    static FString ToAscii(const FGraphXPortDocument& Document);
    static FString ToMermaid(const FGraphXPortDocument& Document);
};
